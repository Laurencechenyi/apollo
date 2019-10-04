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
 * @file
 **/
#include "modules/planning/tasks/deciders/path_reuse_decider/path_reuse_decider.h"

#include <algorithm>
#include <string>
#include <vector>

#include "modules/planning/common/planning_context.h"

namespace apollo {
namespace planning {
// #define ADEBUG AINFO

using apollo::common::Status;
using apollo::common::math::Polygon2d;
using apollo::common::math::Vec2d;

int PathReuseDecider::reusable_path_counter_ = 0;
int PathReuseDecider::total_path_counter_ = 0;

PathReuseDecider::PathReuseDecider(const TaskConfig& config)
    : Decider(config) {}

Status PathReuseDecider::Process(Frame* const frame,
                                 ReferenceLineInfo* const reference_line_info) {
  if (!Decider::config_.path_reuse_decider_config().reuse_path()) {
    return Status::OK();
  }
  // Sanity checks.
  CHECK_NOTNULL(frame);
  CHECK_NOTNULL(reference_line_info);

  // check front static blocking obstacle
  auto* mutable_path_reuse_decider_status = PlanningContext::Instance()
                                                ->mutable_planning_status()
                                                ->mutable_path_reuse_decider();
  auto* mutable_path_decider_status = PlanningContext::Instance()
                                          ->mutable_planning_status()
                                          ->mutable_path_decider();
  constexpr int kWaitCycle = -2;  // wait 2 cycle
  ADEBUG << "reuse or not: "
         << mutable_path_reuse_decider_status->reused_path();
  // T -> F
  if (mutable_path_reuse_decider_status->reused_path()) {
    ADEBUG << "reused path";
    if (CheckPathReusable(frame, reference_line_info)) {
      ADEBUG << "no collision";
      ++reusable_path_counter_;  // count reusable path
      if (!TrimHistoryPath(frame, reference_line_info))
        AERROR << "Failed to trim reused path";
    } else {
      // disable reuse path
      ADEBUG << "collision!!!";
      mutable_path_reuse_decider_status->set_reused_path(false);
    }
  } else {
    // F -> T
    ADEBUG
        << "counter: "
        << mutable_path_decider_status->front_static_obstacle_cycle_counter();
    // far from blocking obstacle or no blocking obstacle for a while
    if (mutable_path_decider_status->front_static_obstacle_cycle_counter() <
            kWaitCycle ||
        IsIgnoredBlockingObstacle(reference_line_info)) {
      // enable reuse path
      mutable_path_reuse_decider_status->set_reused_path(true);
    }
  }
  ++total_path_counter_;
  ADEBUG << "reusable_path_counter_" << reusable_path_counter_;
  ADEBUG << "total_path_counter_" << total_path_counter_;
  return Status::OK();
}

bool PathReuseDecider::CheckPathReusable(
    Frame* const frame, ReferenceLineInfo* const reference_line_info) {
  // TODO(SHU) kinds of criteria for path reuse
  //   if (!IsSameStopObstacles(frame, reference_line_info))
  //     ADEBUG << "not same stop obstacle";
  // reuse path when when collision free and same blocking obstacle
  //   return (IsCollisionFree(reference_line_info) &&
  //           IsSameStopObstacles(frame, reference_line_info));
  // reuse path when the blocking obstacle is same
  //   return IsSameStopObstacles(frame, reference_line_info);
  // reuse path when all obstacles are same
  //   return IsSameObstacles(reference_line_info);
  ADEBUG << "Check Collision";
  return IsCollisionFree(reference_line_info);
}

bool PathReuseDecider::IsIgnoredBlockingObstacle(
    ReferenceLineInfo* const reference_line_info) {
  constexpr double kSDistBuffer = 30.0;  // meter
  constexpr int kTimeBuffer = 3;         // second
  // vehicle speed
  double adc_speed =
      common::VehicleStateProvider::Instance()->linear_velocity();
  double final_s_buffer = std::max(kSDistBuffer, kTimeBuffer * adc_speed);
  // current vehicle s position
  common::SLPoint adc_position_sl;
  GetADCSLPoint(reference_line_info, &adc_position_sl);
  // blocking obstacle start s
  double blocking_obstacle_start_s;
  if (GetBlockingObstacleS(reference_line_info, &blocking_obstacle_start_s) &&
      // distance to blocking obstacle
      (blocking_obstacle_start_s - adc_position_sl.s() > final_s_buffer)) {
    ADEBUG << "blocking obstacle distance: "
           << blocking_obstacle_start_s - adc_position_sl.s();
    return true;
  } else {
    return false;
  }
}

bool PathReuseDecider::GetBlockingObstacleS(
    ReferenceLineInfo* const reference_line_info, double* blocking_obstacle_s) {
  auto* mutable_path_decider_status = PlanningContext::Instance()
                                          ->mutable_planning_status()
                                          ->mutable_path_decider();
  // get blocking obstacle ID (front_static_obstacle_id)
  const std::string& blocking_obstacle_ID =
      mutable_path_decider_status->front_static_obstacle_id();
  const IndexedList<std::string, Obstacle>& indexed_obstacles =
      reference_line_info->path_decision()->obstacles();
  const auto* blocking_obstacle = indexed_obstacles.Find(blocking_obstacle_ID);

  if (blocking_obstacle == nullptr) {
    return false;
  }
  const auto& obstacle_sl = blocking_obstacle->PerceptionSLBoundary();
  *blocking_obstacle_s = obstacle_sl.start_s();
  ADEBUG << "blocking obstacle distance: " << obstacle_sl.start_s();
  ADEBUG << "blocking obstacle distance: " << blocking_obstacle_s;
  return true;
}

void PathReuseDecider::GetADCSLPoint(
    ReferenceLineInfo* const reference_line_info,
    common::SLPoint* adc_position_sl) {
  const auto& reference_line = reference_line_info->reference_line();
  common::math::Vec2d adc_position = {
      common::VehicleStateProvider::Instance()->x(),
      common::VehicleStateProvider::Instance()->y()};
  reference_line.XYToSL(adc_position, adc_position_sl);
}

bool PathReuseDecider::IsSameStopObstacles(
    Frame* const frame, ReferenceLineInfo* const reference_line_info) {
  // sanity check
  if (history_->GetLastFrame() == nullptr) return false;

  const std::vector<const HistoryObjectDecision*> history_objects_decisions =
      history_->GetLastFrame()->GetStopObjectDecisions();
  const auto& reference_line = reference_line_info->reference_line();
  std::vector<double> history_stop_positions;
  std::vector<double> current_stop_positions;

  GetCurrentStopObstacleS(reference_line_info, &current_stop_positions);
  GetHistoryStopSPosition(reference_line_info, history_objects_decisions,
                          &history_stop_positions);

  // get current vehicle s
  common::math::Vec2d adc_position = {
      common::VehicleStateProvider::Instance()->x(),
      common::VehicleStateProvider::Instance()->y()};
  common::SLPoint adc_position_sl;

  reference_line.XYToSL(adc_position, &adc_position_sl);

  ADEBUG << "ADC_s:" << adc_position_sl.s();

  double nearest_history_stop_s = FLAGS_default_front_clear_distance;
  double nearest_current_stop_s = FLAGS_default_front_clear_distance;

  for (auto history_stop_position : history_stop_positions) {
    ADEBUG << "current_stop_position " << history_stop_position
           << "adc_position_sl.s()" << adc_position_sl.s();
    if (history_stop_position < adc_position_sl.s()) {
      continue;
    } else {
      // find nearest history stop
      nearest_history_stop_s = history_stop_position;
      break;
    }
  }

  for (auto current_stop_position : current_stop_positions) {
    ADEBUG << "current_stop_position " << current_stop_position
           << "adc_position_sl.s()" << adc_position_sl.s();
    if (current_stop_position < adc_position_sl.s()) {
      continue;
    } else {
      // find nearest current stop
      nearest_current_stop_s = current_stop_position;
      break;
    }
  }
  return SameStopS(nearest_history_stop_s, nearest_current_stop_s);
}

// compare history stop position vs current stop position
bool PathReuseDecider::SameStopS(const double history_stop_s,
                                 const double current_stop_s) {
  const double KNegative = 0.1;  // (meter) closer
  const double kPositive = 0.5;  // (meter) further
  ADEBUG << "current_stop_s" << current_stop_s;
  ADEBUG << "history_stop_s" << history_stop_s;
  if ((current_stop_s >= history_stop_s &&
       current_stop_s - history_stop_s <= kPositive) ||
      (current_stop_s <= history_stop_s &&
       history_stop_s - current_stop_s <= KNegative))
    return true;
  return false;
}

// get current stop positions
void PathReuseDecider::GetCurrentStopPositions(
    Frame* frame,
    std::vector<const common::PointENU*>* current_stop_positions) {
  auto obstacles = frame->obstacles();
  for (auto obstacle : obstacles) {
    const std::vector<ObjectDecisionType>& current_decisions =
        obstacle->decisions();
    for (auto current_decision : current_decisions) {
      if (current_decision.has_stop())
        current_stop_positions->emplace_back(
            &current_decision.stop().stop_point());
    }
  }
  // sort
  std::sort(current_stop_positions->begin(), current_stop_positions->end(),
            [](const common::PointENU* lhs, const common::PointENU* rhs) {
              return (lhs->x() < rhs->x() ||
                      (lhs->x() == rhs->x() && lhs->y() < rhs->y()));
            });
}

// get current stop obstacle position in s-direction
void PathReuseDecider::GetCurrentStopObstacleS(
    ReferenceLineInfo* const reference_line_info,
    std::vector<double>* current_stop_obstacle) {
  // get all obstacles
  for (auto obstacle :
       reference_line_info->path_decision()->obstacles().Items()) {
    ADEBUG << "current obstacle: "
           << obstacle->PerceptionSLBoundary().start_s();
    if (obstacle->IsLaneBlocking())
      current_stop_obstacle->emplace_back(
          obstacle->PerceptionSLBoundary().start_s());
  }

  // sort w.r.t s
  std::sort(current_stop_obstacle->begin(), current_stop_obstacle->end());
}

// get history stop positions at current reference line
void PathReuseDecider::GetHistoryStopSPosition(
    ReferenceLineInfo* const reference_line_info,
    const std::vector<const HistoryObjectDecision*>& history_objects_decisions,
    std::vector<double>* history_stop_positions) {
  const auto& reference_line = reference_line_info->reference_line();

  for (auto history_object_decision : history_objects_decisions) {
    const std::vector<const ObjectDecisionType*> decisions =
        history_object_decision->GetObjectDecision();

    for (const ObjectDecisionType* decision : decisions) {
      if (decision->has_stop()) {
        common::math::Vec2d stop_position({decision->stop().stop_point().x(),
                                           decision->stop().stop_point().y()});
        common::SLPoint stop_position_sl;

        reference_line.XYToSL(stop_position, &stop_position_sl);
        history_stop_positions->emplace_back(stop_position_sl.s() -
                                             decision->stop().distance_s());

        ADEBUG << "stop_position_x: " << decision->stop().stop_point().x();
        ADEBUG << "stop_position_y: " << decision->stop().stop_point().y();
        ADEBUG << "stop_distance_s: " << decision->stop().distance_s();
        ADEBUG << "stop_distance_s: " << stop_position_sl.s();
        ADEBUG << "adjusted_stop_distance_s: "
               << stop_position_sl.s() - decision->stop().distance_s();
      }
    }
  }
  // sort w.r.t s
  std::sort(history_stop_positions->begin(), history_stop_positions->end());
}

// compare obstacles
bool PathReuseDecider::IsSameObstacles(
    ReferenceLineInfo* const reference_line_info) {
  const auto& history_frame = FrameHistory::Instance()->Latest();
  if (!history_frame) return false;

  const auto& history_reference_line_info =
      history_frame->reference_line_info().front();
  const IndexedList<std::string, Obstacle>& history_obstacles =
      history_reference_line_info.path_decision().obstacles();
  const ReferenceLine& history_reference_line =
      history_reference_line_info.reference_line();
  const ReferenceLine& current_reference_line =
      reference_line_info->reference_line();

  if (reference_line_info->path_decision()->obstacles().Items().size() !=
      history_obstacles.Items().size())
    return false;

  for (auto obstacle :
       reference_line_info->path_decision()->obstacles().Items()) {
    const std::string& obstacle_id = obstacle->Id();
    // same obstacle id
    auto history_obstacle = history_obstacles.Find(obstacle_id);

    if (!history_obstacle ||
        (obstacle->IsStatic() != history_obstacle->IsStatic()) ||
        (IsBlockingDrivingPathObstacle(current_reference_line, obstacle) !=
         IsBlockingDrivingPathObstacle(history_reference_line,
                                       history_obstacle)))
      return false;
  }
  return true;
}

bool PathReuseDecider::IsCollisionFree(
    ReferenceLineInfo* const reference_line_info) {
  constexpr double kMinObstacleArea = 1e-4;
  const double kSBuffer = 0.5;
  constexpr int kNumExtraTailBoundPoint = 20;
  constexpr double kPathBoundsDeciderResolution = 0.5;
  // current vehicle status
  common::math::Vec2d adc_position = {
      common::VehicleStateProvider::Instance()->x(),
      common::VehicleStateProvider::Instance()->y()};
  common::SLPoint adc_position_sl;
  const auto& reference_line = reference_line_info->reference_line();
  reference_line.XYToSL(adc_position, &adc_position_sl);

  // current obstacles
  std::vector<Polygon2d> obstacle_polygons;
  for (auto obstacle :
       reference_line_info->path_decision()->obstacles().Items()) {
    // filtered all non-static objects and virtual obstacle
    if (!obstacle->IsStatic() || obstacle->IsVirtual()) {
      if (!obstacle->IsStatic()) ADEBUG << "SPOT a dynamic obstacle";
      if (obstacle->IsVirtual()) ADEBUG << "SPOT a virtual obstacle";
      continue;
    }
    const auto& obstacle_sl = obstacle->PerceptionSLBoundary();
    // Ignore obstacles behind ADC
    if (obstacle_sl.end_s() < adc_position_sl.s() - kSBuffer) continue;
    // Ignore too small obstacles.
    if ((obstacle_sl.end_s() - obstacle_sl.start_s()) *
            (obstacle_sl.end_l() - obstacle_sl.start_l()) <
        kMinObstacleArea)
      continue;
    obstacle_polygons.push_back(
        Polygon2d({Vec2d(obstacle_sl.start_s(), obstacle_sl.start_l()),
                   Vec2d(obstacle_sl.start_s(), obstacle_sl.end_l()),
                   Vec2d(obstacle_sl.end_s(), obstacle_sl.end_l()),
                   Vec2d(obstacle_sl.end_s(), obstacle_sl.start_l())}));
  }
  if (obstacle_polygons.empty()) return true;

  const auto& history_frame = FrameHistory::Instance()->Latest();
  if (!history_frame) return false;
  const DiscretizedPath& history_path =
      history_frame->current_frame_planned_path();
  // path end point
  common::SLPoint path_end_position_sl;
  common::math::Vec2d path_end_position = {history_path.back().x(),
                                           history_path.back().y()};
  reference_line.XYToSL(path_end_position, &path_end_position_sl);
  for (size_t i = 0; i < history_path.size(); ++i) {
    common::SLPoint path_position_sl;
    common::math::Vec2d path_position = {history_path[i].x(),
                                         history_path[i].y()};
    reference_line.XYToSL(path_position, &path_position_sl);
    if (path_end_position_sl.s() - path_position_sl.s() <
        kNumExtraTailBoundPoint * kPathBoundsDeciderResolution) {
      break;
    }
    if (path_position_sl.s() < adc_position_sl.s() - kSBuffer) continue;
    const auto& vehicle_box =
        common::VehicleConfigHelper::Instance()->GetBoundingBox(
            history_path[i]);
    std::vector<Vec2d> ABCDpoints = vehicle_box.GetAllCorners();
    for (const auto& corner_point : ABCDpoints) {
      // For each corner point, project it onto reference_line
      common::SLPoint curr_point_sl;
      if (!reference_line.XYToSL(corner_point, &curr_point_sl)) {
        AERROR << "Failed to get the projection from point onto "
                  "reference_line";
        return false;
      }
      auto curr_point = Vec2d(curr_point_sl.s(), curr_point_sl.l());
      // Check if it's in any polygon of other static obstacles.
      for (const auto& obstacle_polygon : obstacle_polygons) {
        if (obstacle_polygon.IsPointIn(curr_point)) {
          ADEBUG << "to end point:"
                 << path_end_position_sl.s() - path_position_sl.s();
          ADEBUG << "collision:" << curr_point.x() << ", " << curr_point.y();
          Vec2d xy_point;
          reference_line.SLToXY(curr_point_sl, &xy_point);
          ADEBUG << "collision:" << xy_point.x() << ", " << xy_point.y();
          return false;
        }
      }
    }
  }
  return true;
}

bool PathReuseDecider::TrimHistoryPath(
    Frame* const frame, ReferenceLineInfo* const reference_line_info) {
  const auto& history_frame = FrameHistory::Instance()->Latest();
  if (!history_frame) return false;
  const DiscretizedPath& history_path =
      history_frame->current_frame_planned_path();
  DiscretizedPath trimmed_path;
  // current vehicle status
  common::math::Vec2d adc_position = {
      common::VehicleStateProvider::Instance()->x(),
      common::VehicleStateProvider::Instance()->y()};
  common::SLPoint adc_position_sl;
  const auto& reference_line = reference_line_info->reference_line();
  reference_line.XYToSL(adc_position, &adc_position_sl);
  double path_start_s = 0.0;
  size_t path_start_index = 0;

  for (size_t i = 0; i < history_path.size(); ++i) {
    common::SLPoint path_position_sl;
    common::math::Vec2d path_position = {history_path[i].x(),
                                         history_path[i].y()};
    reference_line.XYToSL(path_position, &path_position_sl);
    if (path_position_sl.s() < adc_position_sl.s()) {
      path_start_s = path_position_sl.s();
      ++path_start_index;
    } else {
      double updated_s = history_path[i].s() - path_start_s;
      trimmed_path.emplace_back(history_path[i]);
      trimmed_path.back().set_s(updated_s);
    }
  }
  trimmed_path.insert(trimmed_path.begin(), history_path[path_start_index]);
  frame->set_current_frame_planned_path(trimmed_path);
  return true;
}

}  // namespace planning
}  // namespace apollo