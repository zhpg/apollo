/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file piecewise_jerk_fallback_speed.cc
 **/

#include "modules/planning/tasks/optimizers/piecewise_jerk_speed/piecewise_jerk_speed_optimizer.h"

#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "modules/common/proto/pnc_point.pb.h"

#include "modules/common/vehicle_state/vehicle_state_provider.h"
#include "modules/planning/common/planning_gflags.h"
#include "modules/planning/common/st_graph_data.h"
#include "modules/planning/math/piecewise_jerk/piecewise_jerk_speed_problem.h"

namespace apollo {
namespace planning {

using apollo::common::ErrorCode;
using apollo::common::PathPoint;
using apollo::common::SpeedPoint;
using apollo::common::Status;
using apollo::common::TrajectoryPoint;

PiecewiseJerkSpeedOptimizer::PiecewiseJerkSpeedOptimizer(
    const TaskConfig& config)
    : SpeedOptimizer(config) {
  // TODO(Hongyi): recover this hacked task_name for dreamview
  SetName("QpSplineStSpeedOptimizer");
  CHECK(config_.has_piecewise_jerk_speed_config());
}

Status PiecewiseJerkSpeedOptimizer::Process(
    const SLBoundary& adc_sl_boundary, const PathData& path_data,
    const TrajectoryPoint& init_point, const ReferenceLine& reference_line,
    const SpeedData& reference_speed_data, PathDecision* const path_decision,
    SpeedData* const speed_data) {
  if (reference_line_info_->ReachedDestination()) {
    return Status::OK();
  }

  if (path_data.discretized_path().empty()) {
    std::string msg("Empty path data");
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  for (const auto* obstacle : path_decision->obstacles().Items()) {
    DCHECK(obstacle->HasLongitudinalDecision());
  }
  StGraphData& st_graph_data = *reference_line_info_->mutable_st_graph_data();

  const auto& veh_param =
      common::VehicleConfigHelper::GetConfig().vehicle_param();

  std::array<double, 3> init_s = {0.0, st_graph_data.init_point().v(),
                                  st_graph_data.init_point().a()};
  double delta_t = 0.1;
  double total_length = st_graph_data.path_length_by_conf();
  double total_time = st_graph_data.total_time_by_conf();
  int num_of_knots = static_cast<int>(total_time / delta_t) + 1;

  PiecewiseJerkSpeedProblem piecewise_jerk_problem(num_of_knots, delta_t, init_s, {0.0, 0.0, 0.0});

  const auto& piecewise_jerk_speed_config =
      config_.piecewise_jerk_speed_config();
  piecewise_jerk_problem.set_weight_x(piecewise_jerk_speed_config.s_weight());
  piecewise_jerk_problem.set_weight_dx(piecewise_jerk_speed_config.velocity_weight());
  piecewise_jerk_problem.set_weight_ddx(piecewise_jerk_speed_config.acc_weight());
  piecewise_jerk_problem.set_weight_dddx(piecewise_jerk_speed_config.jerk_weight());
  piecewise_jerk_problem.set_weight_x_reference(piecewise_jerk_speed_config.ref_weight());

  piecewise_jerk_problem.SetZeroOrderBounds(0.0, total_length);
  piecewise_jerk_problem.SetFirstOrderBounds(0.0,
                                    std::fmax(FLAGS_planning_upper_speed_limit,
                                              st_graph_data.init_point().v()));
  piecewise_jerk_problem.SetSecondOrderBounds(veh_param.max_deceleration(),
                                     veh_param.max_acceleration());
  piecewise_jerk_problem.SetThirdOrderBound(FLAGS_longitudinal_jerk_bound);

  // TODO(Hongyi): delete this when ready to use vehicle_params
  piecewise_jerk_problem.SetSecondOrderBounds(-4.4, 2.0);
  piecewise_jerk_problem.SetFirstOrderReference(FLAGS_default_cruise_speed);

  // Update STBoundary
  std::vector<std::pair<double, double>> s_bounds;
  for (int i = 0; i < num_of_knots; ++i) {
    double curr_t = i * delta_t;
    double s_lower_bound = 0.0;
    double s_upper_bound = total_length;
    for (const STBoundary* boundary : st_graph_data.st_boundaries()) {
      double s_lower = 0.0;
      double s_upper = 0.0;
      if (!boundary->GetUnblockSRange(curr_t, &s_upper, &s_lower)) {
        continue;
      }
      switch (boundary->boundary_type()) {
        case STBoundary::BoundaryType::STOP:
        case STBoundary::BoundaryType::YIELD:
          s_upper_bound = std::fmin(s_upper_bound, s_upper);
          break;
        case STBoundary::BoundaryType::FOLLOW:
          // TODO(Hongyi): unify follow buffer on decision side
          s_upper_bound = std::fmin(s_upper_bound, s_upper - 8.0);
          break;
        case STBoundary::BoundaryType::OVERTAKE:
          s_lower_bound = std::fmax(s_lower_bound, s_lower);
          break;
        default:
          break;
      }
    }
    if (s_lower_bound > s_upper_bound) {
      std::string msg("s_lower_bound larger than s_upper_bound on STGraph!");
      AERROR << msg;
      speed_data->clear();
      return Status(ErrorCode::PLANNING_ERROR, msg);
    }
    s_bounds.emplace_back(s_lower_bound, s_upper_bound);
  }
  piecewise_jerk_problem.SetZeroOrderBounds(std::move(s_bounds));

  // Update SpeedBoundary and ref_s
  std::vector<double> x_ref;
  std::vector<double> penalty_dx;
  std::vector<std::pair<double, double>> s_dot_bounds;
  const SpeedLimit& speed_limit = st_graph_data.speed_limit();
  for (int i = 0; i < num_of_knots; ++i) {
    double curr_t = i * delta_t;
    // get path_s
    SpeedPoint sp;
    reference_speed_data.EvaluateByTime(curr_t, &sp);
    const double path_s = sp.s();
    x_ref.emplace_back(path_s);
    // get curvature
    PathPoint path_point;
    path_data.GetPathPointWithPathS(path_s, &path_point);
    penalty_dx.emplace_back(std::fabs(path_point.kappa()) *
                            piecewise_jerk_speed_config.kappa_penalty_weight());
    // get v_upper_bound
    const double v_lower_bound = 0.0;
    double v_upper_bound = FLAGS_planning_upper_speed_limit;
    v_upper_bound = speed_limit.GetSpeedLimitByS(path_s);
    s_dot_bounds.emplace_back(v_lower_bound, std::fmax(v_upper_bound, 0.0));
  }
  piecewise_jerk_problem.SetZeroOrderReference(x_ref);
  piecewise_jerk_problem.SetFirstOrderPenalty(penalty_dx);
  piecewise_jerk_problem.SetFirstOrderBounds(s_dot_bounds);

  // Solve the problem
  if (!piecewise_jerk_problem.Optimize()) {
    std::string msg("Piecewise jerk speed optimizer failed!");
    AERROR << msg;
    speed_data->clear();
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  // Extract output
  const std::vector<double>& s = piecewise_jerk_problem.x();
  const std::vector<double>& ds = piecewise_jerk_problem.x_derivative();
  const std::vector<double>& dds = piecewise_jerk_problem.x_second_order_derivative();
  for (int i = 0; i < num_of_knots; ++i) {
    ADEBUG << "For t[" << i * delta_t << "], s = " << s[i] << ", v = " << ds[i]
           << ", a = " << dds[i];
  }
  speed_data->clear();
  speed_data->AppendSpeedPoint(s[0], 0.0, ds[0], dds[0], 0.0);
  for (int i = 1; i < num_of_knots; ++i) {
    // Avoid the very last points when already stopped
    if (ds[i] <= 0.0) {
      break;
    }
    speed_data->AppendSpeedPoint(s[i], delta_t * i, ds[i], dds[i],
                                 (dds[i] - dds[i - 1]) / delta_t);
  }
  RecordDebugInfo(*speed_data, st_graph_data.mutable_st_graph_debug());
  return Status::OK();
}

}  // namespace planning
}  // namespace apollo
