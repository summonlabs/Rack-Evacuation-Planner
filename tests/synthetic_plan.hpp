// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A minimal sealed plan, used where a test needs durable content without
// planning anything.  It is a real Plan: sealed, verified, and structurally
// valid.

#ifndef REP_TESTS_SYNTHETIC_PLAN_HPP
#define REP_TESTS_SYNTHETIC_PLAN_HPP

#include <string>
#include <utility>

#include <rep/rep.hpp>

namespace reptest {

[[nodiscard]] inline rep::Result<rep::Plan> synthetic_plan(std::string_view planner,
                                                           std::uint64_t sequence,
                                                           std::string_view lineage,
                                                           std::string_view key) {
  auto planner_id = rep::PlannerId::parse(planner);
  if (!planner_id.ok()) {
    return planner_id.error();
  }
  auto plan_id = rep::PlanId::parse(std::string(planner) + "-" + std::to_string(sequence));
  if (!plan_id.ok()) {
    return plan_id.error();
  }
  auto lineage_id = rep::LineageId::parse(lineage);
  if (!lineage_id.ok()) {
    return lineage_id.error();
  }
  auto idempotency = rep::IdempotencyKey::parse(key);
  if (!idempotency.ok()) {
    return idempotency.error();
  }
  auto rack = rep::RackId::parse("rack-alpha");
  if (!rack.ok()) {
    return rack.error();
  }

  rep::Plan plan;
  plan.id = plan_id.value();
  plan.planner = planner_id.value();
  plan.revision = rep::PlanRevision{1};
  plan.sequence = rep::Sequence{sequence};
  plan.epoch = rep::Epoch{1};
  plan.lineage = lineage_id.value();
  plan.idempotency_key = idempotency.value();
  auto isolation = rep::IsolationRequest::make(
      rack.value(), rep::IsolationKind::Depower, rep::Generation{7}, {rep::ObligationKind::Workload});
  if (!isolation.ok()) {
    return isolation.error();
  }
  plan.request_digest = isolation.value().digest();
  plan.source_rack = rack.value();
  plan.composition_revision = rep::Generation{7};
  plan.isolation = rep::IsolationKind::Depower;
  plan.evacuate_kinds = {rep::ObligationKind::Workload};
  plan.status = rep::PlanStatus::EmptySafe;
  return rep::Plan::seal(std::move(plan));
}

} // namespace reptest

#endif // REP_TESTS_SYNTHETIC_PLAN_HPP
