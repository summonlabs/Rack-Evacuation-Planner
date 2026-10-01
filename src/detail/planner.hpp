// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal planning entry point. Not installed: callers go through
// PlanEngine, which owns identity, sequence, and the control epoch.

#ifndef REP_SRC_DETAIL_PLANNER_HPP
#define REP_SRC_DETAIL_PLANNER_HPP

#include <cstddef>

#include "rep/plan.hpp"
#include "rep/request.hpp"
#include "rep/status.hpp"
#include "rep/types.hpp"

namespace rep::detail {

// Identity the engine assigns to the plan being sealed.  The planner never
// invents an identity: it receives one and stamps it onto the artifact.
struct PlanSealContext {
  PlannerId planner;
  PlanId plan_id;
  Sequence sequence;
  Epoch epoch;
  LineageId lineage;
  PlanRevision revision;
};

struct PlanLimits {
  std::size_t max_obligations{100000};
  std::size_t max_candidates{100000};
};

// Plans one evacuation.
//
// Returns an Error only when the request itself must be refused: bound
// evidence that contradicts other bound evidence, or evidence whose integrity
// does not hold.  Everything else is reported inside the plan: a missing or
// incomplete obligation set makes the plan indeterminate, and an obligation
// that cannot be assigned becomes a typed residual.
[[nodiscard]] Result<Plan> plan_evacuation(const PlanRequest& request,
                                           const PlanSealContext& context,
                                           const PlanLimits& limits);

} // namespace rep::detail

#endif // REP_SRC_DETAIL_PLANNER_HPP
