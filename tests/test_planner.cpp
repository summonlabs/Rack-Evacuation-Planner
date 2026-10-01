// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Planning semantics: enumeration completeness, eligibility, deterministic
// ordering, dependency handling, conservation, and the exact typed reasons.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <rep/rep.hpp>

#include "testkit.hpp"

#include "planner_fixture.hpp"

using namespace repfix;


// ---------------------------------------------------------------------------
// Coverage: establishing the obligation set
// ---------------------------------------------------------------------------

REP_TEST(planner, complete_single_workload) {
  auto outcome = plan_scenario(standard().text());
  REP_REQUIRE(outcome.ok());
  REP_REQUIRE(outcome.value().committed());
  const rep::Plan& plan = outcome.value().plan;
  REP_CHECK(plan.status == rep::PlanStatus::Complete);
  REP_CHECK(plan.verdict() == rep::SafetyVerdict::Safe);
  REP_CHECK_EQ(plan.assignments.size(), std::size_t{1});
  REP_CHECK(plan.residuals.empty());
  REP_CHECK(plan.out_of_scope.empty());
  if (plan.assignments.size() == 1) {
    const rep::Assignment& assignment = plan.assignments.front();
    REP_CHECK_EQ(assignment.obligation.str(), std::string("w1"));
    REP_CHECK_EQ(assignment.candidate.str(), std::string("c1"));
    REP_CHECK_EQ(assignment.destination.to_text(), std::string("rack_slot:rack-B"));
    REP_CHECK_EQ(assignment.order_index, std::uint32_t{0});
    REP_CHECK_EQ(assignment.wave, std::uint32_t{0});
  }
  REP_CHECK_EQ(plan.wave_count(), std::uint32_t{1});
  REP_REQUIRE(outcome.value().plan.verify().ok());
}

REP_TEST(planner, empty_enumeration_proves_safe) {
  Scenario scenario = standard();
  scenario.set_body("rack_composition", "rack rack-A\nrevision 7\noccupants -\n");
  scenario.set_body("enumeration", "rack rack-A\nrevision 7\ncomplete true\nenumerated -\n");
  scenario.set_body("obligation_catalog", "");
  scenario.remove("obligation_catalog");
  scenario.remove("asi_workload_state");
  scenario.remove("candidate_offers");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_REQUIRE(outcome.value().committed());
  REP_CHECK(outcome.value().plan.status == rep::PlanStatus::EmptySafe);
  REP_CHECK(outcome.value().plan.verdict() == rep::SafetyVerdict::Safe);
  REP_CHECK(outcome.value().plan.assignments.empty());
  REP_CHECK(outcome.value().plan.residuals.empty());
}

REP_TEST(planner, incomplete_enumeration_is_indeterminate) {
  Scenario scenario = standard();
  scenario.set_body("enumeration", "rack rack-A\nrevision 7\ncomplete false\nenumerated w1\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_REQUIRE(outcome.value().committed());
  REP_CHECK(outcome.value().plan.status == rep::PlanStatus::Indeterminate);
  REP_CHECK(outcome.value().plan.verdict() == rep::SafetyVerdict::Indeterminate);
  REP_CHECK_EQ(outcome.value().plan.indeterminacy_reasons.size(), std::size_t{1});
  if (!outcome.value().plan.indeterminacy_reasons.empty()) {
    REP_CHECK(outcome.value().plan.indeterminacy_reasons.front() ==
              rep::IndeterminacyReason::EnumerationIncomplete);
  }
  // An indeterminate plan asserts no per-obligation outcome at all.
  REP_CHECK(outcome.value().plan.assignments.empty());
  REP_CHECK(outcome.value().plan.residuals.empty());
  REP_CHECK(outcome.value().plan.out_of_scope.empty());
}

REP_TEST(planner, enumeration_set_mismatch_is_indeterminate) {
  Scenario scenario = standard();
  scenario.set_body("enumeration", "rack rack-A\nrevision 7\ncomplete true\nenumerated -\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_REQUIRE(outcome.value().committed());
  REP_CHECK(outcome.value().plan.status == rep::PlanStatus::Indeterminate);
  REP_REQUIRE(!outcome.value().plan.indeterminacy_reasons.empty());
  REP_CHECK(outcome.value().plan.indeterminacy_reasons.front() ==
            rep::IndeterminacyReason::EnumerationSetMismatch);
}

REP_TEST(planner, missing_composition_is_indeterminate) {
  Scenario scenario = standard();
  scenario.remove("rack_composition");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().plan.status == rep::PlanStatus::Indeterminate);
  REP_REQUIRE(!outcome.value().plan.indeterminacy_reasons.empty());
  REP_CHECK(outcome.value().plan.indeterminacy_reasons.front() ==
            rep::IndeterminacyReason::RackCompositionMissing);
}

REP_TEST(planner, missing_enumeration_is_indeterminate) {
  Scenario scenario = standard();
  scenario.remove("enumeration");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().plan.status == rep::PlanStatus::Indeterminate);
  REP_REQUIRE(!outcome.value().plan.indeterminacy_reasons.empty());
  REP_CHECK(outcome.value().plan.indeterminacy_reasons.front() ==
            rep::IndeterminacyReason::EnumerationMissing);
}

REP_TEST(planner, composition_revision_mismatch_is_indeterminate) {
  Scenario scenario = standard();
  scenario.revision = "8";
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().plan.status == rep::PlanStatus::Indeterminate);
  REP_REQUIRE(!outcome.value().plan.indeterminacy_reasons.empty());
  REP_CHECK(outcome.value().plan.indeterminacy_reasons.front() ==
            rep::IndeterminacyReason::RackCompositionRevisionMismatch);
}

REP_TEST(planner, enumeration_revision_mismatch_is_indeterminate) {
  Scenario scenario = standard();
  scenario.set_body("enumeration",
                    "rack rack-A\nrevision 6\ncomplete true\nenumerated w1\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().plan.status == rep::PlanStatus::Indeterminate);
  REP_REQUIRE(!outcome.value().plan.indeterminacy_reasons.empty());
  REP_CHECK(outcome.value().plan.indeterminacy_reasons.front() ==
            rep::IndeterminacyReason::EnumerationRevisionMismatch);
}

REP_TEST(planner, undefined_obligation_is_indeterminate) {
  Scenario scenario = standard();
  scenario.set_body("obligation_catalog", "");
  scenario.remove("obligation_catalog");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().plan.status == rep::PlanStatus::Indeterminate);
  REP_REQUIRE(!outcome.value().plan.indeterminacy_reasons.empty());
  REP_CHECK(outcome.value().plan.indeterminacy_reasons.front() ==
            rep::IndeterminacyReason::ObligationCatalogMissing);

  // A catalog that defines a different obligation leaves the occupant w1
  // undefined: the enumeration is complete, the definition is not.
  Scenario partial = standard();
  partial.set_body("obligation_catalog",
                   "obligation id=w9 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                   "protected=false depends=-\n");
  auto second = plan_scenario(partial.text());
  REP_REQUIRE(second.ok());
  REP_CHECK(second.value().plan.status == rep::PlanStatus::Indeterminate);
  REP_REQUIRE(!second.value().plan.indeterminacy_reasons.empty());
  REP_CHECK(second.value().plan.indeterminacy_reasons.front() ==
            rep::IndeterminacyReason::ObligationUndefined);
}

REP_TEST(planner, obligation_definition_for_another_rack_is_indeterminate) {
  Scenario scenario = standard();
  scenario.set_body("obligation_catalog",
                    "obligation id=w1 kind=workload rack=rack-Z demand=cpu_millicores:1000 "
                    "protected=false depends=-\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().plan.status == rep::PlanStatus::Indeterminate);
  REP_REQUIRE(!outcome.value().plan.indeterminacy_reasons.empty());
  REP_CHECK(outcome.value().plan.indeterminacy_reasons.front() ==
            rep::IndeterminacyReason::ObligationSourceRackMismatch);
}

REP_TEST(planner, unbound_evidence_never_satisfies_a_requirement) {
  // The bundle carries a complete-looking composition and enumeration, but the
  // request binds a different stream for both, so the streams are absent.
  Scenario scenario = standard();
  for (Block& block : scenario.blocks) {
    if (block.kind == "rack_composition" || block.kind == "enumeration") {
      block.unbound = true;
    }
  }
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().plan.status == rep::PlanStatus::Indeterminate);
  REP_REQUIRE(!outcome.value().plan.indeterminacy_reasons.empty());
  REP_CHECK(outcome.value().plan.indeterminacy_reasons.front() ==
            rep::IndeterminacyReason::RackCompositionMissing);
}

REP_TEST(planner, out_of_scope_kinds_are_recorded_not_assigned) {
  Scenario scenario = standard();
  scenario.kinds = "storage_replica";
  scenario.set_body("obligation_catalog",
                    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=false depends=-\n");
  scenario.remove("asi_workload_state");
  scenario.remove("candidate_offers");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_REQUIRE(outcome.value().committed());
  const rep::Plan& plan = outcome.value().plan;
  REP_CHECK(plan.status == rep::PlanStatus::EmptySafe);
  REP_CHECK_EQ(plan.out_of_scope.size(), std::size_t{1});
  REP_CHECK(plan.assignments.empty());
  if (!plan.out_of_scope.empty()) {
    REP_CHECK(plan.out_of_scope.front().reason == rep::ScopeExclusionReason::KindNotRequested);
    REP_CHECK(plan.out_of_scope.front().kind == rep::ObligationKind::Workload);
  }
}

// ---------------------------------------------------------------------------
// Candidate eligibility
// ---------------------------------------------------------------------------

REP_TEST(planner, no_candidate_is_residual) {
  Scenario scenario = standard();
  scenario.set_body("candidate_offers", "");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_REQUIRE(outcome.value().committed());
  const rep::Plan& plan = outcome.value().plan;
  REP_CHECK(plan.status == rep::PlanStatus::Partial);
  REP_CHECK(plan.verdict() == rep::SafetyVerdict::NotProven);
  const rep::Residual* residual = find_residual(plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(residual->reason == rep::ResidualReason::NoCandidate);
  REP_CHECK(residual->rejected.empty());
}

REP_TEST(planner, action_incompatible_with_obligation_kind_is_rejected) {
  Scenario scenario = standard();
  scenario.set_body("candidate_offers",
                    "candidate id=c1 obligation=w1 action=detach "
                    "destination=fabric_endpoint:fab-1 authority=agent-scheduler generation=4 "
                    "epoch=1 provision=cpu_millicores:1000 cost=10 window=false\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::ObligationKindNotAllowed));
  REP_CHECK(residual->reason == rep::ResidualReason::PolicyDenied);
}

REP_TEST(planner, action_incompatible_with_destination_kind_is_rejected) {
  Scenario scenario = standard();
  scenario.set_body("candidate_offers",
                    "candidate id=c1 obligation=w1 action=live_migrate "
                    "destination=fabric_endpoint:fab-1 authority=agent-scheduler generation=4 "
                    "epoch=1 provision=cpu_millicores:1000 cost=10 window=false\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::ActionDestinationMismatch));
}

REP_TEST(planner, destination_equal_to_source_rack_is_rejected) {
  Scenario scenario = standard();
  scenario.set_body("candidate_offers",
                    "candidate id=c1 obligation=w1 action=live_migrate "
                    "destination=rack_slot:rack-A authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=10 window=false\n");
  scenario.set_body("capacity",
                    "destination destination=rack_slot:rack-A available=cpu_millicores:8000\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::SelfDestination));
}

REP_TEST(planner, capacity_shortfall_is_residual_not_an_assignment) {
  Scenario scenario = standard();
  scenario.set_body("capacity",
                    "destination destination=rack_slot:rack-B available=cpu_millicores:999\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Plan& plan = outcome.value().plan;
  REP_CHECK(plan.status == rep::PlanStatus::Partial);
  const rep::Residual* residual = find_residual(plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(residual->reason == rep::ResidualReason::CapacityExhausted);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::CapacityInsufficient));
}

REP_TEST(planner, destination_without_capacity_evidence_is_unknown) {
  Scenario scenario = standard();
  scenario.set_body("capacity",
                    "destination destination=rack_slot:rack-C available=cpu_millicores:8000\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(residual->reason == rep::ResidualReason::CapacityExhausted);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::DestinationUnknown));
}

REP_TEST(planner, provision_that_does_not_cover_demand_is_rejected) {
  Scenario scenario = standard();
  scenario.set_body("candidate_offers",
                    "candidate id=c1 obligation=w1 action=live_migrate "
                    "destination=rack_slot:rack-B authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:999 cost=10 window=false\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::CapacityClaimMismatch));
}

REP_TEST(planner, candidate_from_an_unbound_authority_is_rejected) {
  Scenario scenario = standard();
  scenario.set_body("candidate_offers",
                    "candidate id=c1 obligation=w1 action=live_migrate "
                    "destination=rack_slot:rack-B authority=rogue-authority generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=10 window=false\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::AuthorityMismatch));
}

REP_TEST(planner, candidate_at_the_wrong_generation_is_stale) {
  Scenario scenario = standard();
  scenario.set_body("candidate_offers",
                    "candidate id=c1 obligation=w1 action=live_migrate "
                    "destination=rack_slot:rack-B authority=agent-scheduler generation=3 epoch=1 "
                    "provision=cpu_millicores:1000 cost=10 window=false\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::StaleGeneration));
}

REP_TEST(planner, cheaper_candidate_is_selected) {
  Scenario scenario = standard();
  scenario.set_body("candidate_offers",
                    "candidate id=c9 obligation=w1 action=live_migrate "
                    "destination=rack_slot:rack-C authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=50 window=false\n"
                    "candidate id=c2 obligation=w1 action=live_migrate "
                    "destination=rack_slot:rack-B authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=5 window=false\n");
  scenario.set_body("capacity",
                    "destination destination=rack_slot:rack-B available=cpu_millicores:8000\n"
                    "destination destination=rack_slot:rack-C available=cpu_millicores:8000\n");
  scenario.set_body("failure_domain",
                    "member kind=rack rack=rack-A domain=fd-1\n"
                    "member kind=destination destination=rack_slot:rack-B domain=fd-2\n"
                    "member kind=destination destination=rack_slot:rack-C domain=fd-3\n"
                    "member kind=obligation obligation=w1 domain=fd-1\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_REQUIRE(outcome.value().plan.status == rep::PlanStatus::Complete);
  REP_CHECK_EQ(outcome.value().plan.assignments.front().candidate.str(), std::string("c2"));
}

REP_TEST(planner, duplicate_action_destination_keeps_the_first_candidate) {
  Scenario scenario = standard();
  scenario.set_body("candidate_offers",
                    "candidate id=c1 obligation=w1 action=live_migrate "
                    "destination=rack_slot:rack-B authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=10 window=false\n"
                    "candidate id=c2 obligation=w1 action=live_migrate "
                    "destination=rack_slot:rack-B authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=1 window=false\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_REQUIRE(outcome.value().plan.status == rep::PlanStatus::Complete);
  REP_CHECK_EQ(outcome.value().plan.assignments.front().candidate.str(), std::string("c1"));
}

// ---------------------------------------------------------------------------
// Adjacent state, protection and policy
// ---------------------------------------------------------------------------

REP_TEST(planner, protected_obligation_needs_an_explicit_rule) {
  Scenario scenario = standard();
  scenario.set_body("obligation_catalog",
                    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=true depends=-\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(residual->reason == rep::ResidualReason::ProtectedObligation);

  Scenario allowed = standard();
  allowed.set_body("obligation_catalog",
                   "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                   "protected=true depends=-\n");
  Block* policy = allowed.find("placement_policy");
  if (policy == nullptr) {
    allowed.blocks.push_back(make_block("placement_policy", "placement-policy", "policy", "4", "1",
                                        "rule kind=allow_protected_move obligation=workload "
                                        "action=live_migrate destination=rack_slot domain=-\n"));
  }
  auto second = plan_scenario(allowed.text());
  REP_REQUIRE(second.ok());
  REP_CHECK(second.value().plan.status == rep::PlanStatus::Complete);
}

REP_TEST(planner, non_migratable_and_unknown_capability_are_distinct) {
  Scenario blocked = standard();
  blocked.set_body("asi_workload_state",
                   "record obligation=w1 lifecycle=running migration=not_migratable "
                   "attachment=local_state\n");
  auto first = plan_scenario(blocked.text());
  REP_REQUIRE(first.ok());
  const rep::Residual* residual = find_residual(first.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(residual->reason == rep::ResidualReason::NonMigratable);

  Scenario unknown = standard();
  unknown.set_body("asi_workload_state",
                   "record obligation=w1 lifecycle=running migration=unknown "
                   "attachment=unknown\n");
  auto second = plan_scenario(unknown.text());
  REP_REQUIRE(second.ok());
  const rep::Residual* other = find_residual(second.value().plan, "w1");
  REP_REQUIRE(other != nullptr);
  REP_CHECK(other->reason == rep::ResidualReason::StateUnknown);
}

REP_TEST(planner, absent_workload_state_stream_is_missing_evidence) {
  Scenario scenario = standard();
  scenario.remove("asi_workload_state");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(residual->reason == rep::ResidualReason::EvidenceMissing);
}

REP_TEST(planner, absent_record_for_a_known_obligation_is_missing_evidence) {
  Scenario scenario = standard();
  scenario.set_body("asi_workload_state",
                    "record obligation=w9 lifecycle=running migration=live_allowed "
                    "attachment=stateless\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(residual->reason == rep::ResidualReason::EvidenceMissing);
}

REP_TEST(planner, fabric_obligation_path_rules_are_enforced) {
  Scenario scenario = standard();
  scenario.kinds = "network_path";
  scenario.set_body("rack_composition", "rack rack-A\nrevision 7\noccupants n1\n");
  scenario.set_body("enumeration", "rack rack-A\nrevision 7\ncomplete true\nenumerated n1\n");
  scenario.set_body("obligation_catalog",
                    "obligation id=n1 kind=network_path rack=rack-A demand=network_kib:100 "
                    "protected=false depends=-\n");
  scenario.remove("asi_workload_state");
  scenario.set_body("candidate_offers",
                    "candidate id=c1 obligation=n1 action=rebind destination=fabric_endpoint:fab-1 "
                    "authority=agent-scheduler generation=4 epoch=1 provision=network_kib:100 "
                    "cost=10 window=false\n");
  scenario.set_body("capacity",
                    "destination destination=fabric_endpoint:fab-1 available=network_kib:1000\n");
  scenario.set_body("failure_domain",
                    "member kind=rack rack=rack-A domain=fd-1\n"
                    "member kind=destination destination=fabric_endpoint:fab-1 domain=fd-2\n"
                    "member kind=obligation obligation=n1 domain=fd-1\n");
  scenario.set_body("dfi_obligation",
                    "record obligation=n1 kind=path_attachment path_migration=true "
                    "endpoints=fabric_endpoint:fab-1 "
                    "mapping_digest=0000000000000000000000000000000000000000000000000000000000000000\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().plan.status == rep::PlanStatus::Complete);

  Scenario denied = scenario;
  denied.set_body("dfi_obligation",
                  "record obligation=n1 kind=path_attachment path_migration=true "
                  "endpoints=fabric_endpoint:fab-9 "
                  "mapping_digest=0000000000000000000000000000000000000000000000000000000000000000\n");
  auto second = plan_scenario(denied.text());
  REP_REQUIRE(second.ok());
  const rep::Residual* residual = find_residual(second.value().plan, "n1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::DestinationNotPermitted));

  Scenario no_migration = scenario;
  no_migration.set_body("dfi_obligation",
                        "record obligation=n1 kind=path_attachment path_migration=false "
                        "endpoints=- "
                        "mapping_digest=0000000000000000000000000000000000000000000000000000000000000000\n");
  auto third = plan_scenario(no_migration.text());
  REP_REQUIRE(third.ok());
  const rep::Residual* other = find_residual(third.value().plan, "n1");
  REP_REQUIRE(other != nullptr);
  REP_CHECK(other->reason == rep::ResidualReason::NonMigratable);
}

REP_TEST(planner, deny_rule_blocks_an_otherwise_eligible_candidate) {
  Scenario scenario = standard();
  scenario.blocks.push_back(make_block(
      "placement_policy", "placement-policy", "policy", "4", "1",
      "rule kind=deny_action obligation=workload action=live_migrate destination=rack_slot "
      "domain=-\n"));
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(residual->reason == rep::ResidualReason::PolicyDenied);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::PolicyDenied));
}

REP_TEST(planner, allow_list_for_a_kind_is_closed) {
  Scenario scenario = standard();
  scenario.blocks.push_back(make_block(
      "placement_policy", "placement-policy", "policy", "4", "1",
      "rule kind=allow_action obligation=workload action=cold_migrate destination=rack_slot "
      "domain=-\n"));
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::PolicyDenied));
}

REP_TEST(planner, domain_spread_requirement_rejects_the_source_domain) {
  Scenario scenario = standard();
  scenario.blocks.push_back(make_block(
      "placement_policy", "placement-policy", "policy", "4", "1",
      "rule kind=require_domain_spread obligation=workload action=live_migrate "
      "destination=rack_slot domain=-\n"));
  scenario.set_body("failure_domain",
                    "member kind=rack rack=rack-A domain=fd-1\n"
                    "member kind=destination destination=rack_slot:rack-B domain=fd-1\n"
                    "member kind=obligation obligation=w1 domain=fd-1\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::SameFailureDomain));
  REP_CHECK(residual->reason == rep::ResidualReason::FailureDomainConflict);
}

REP_TEST(planner, domain_spread_with_unknown_destination_domain_is_refused) {
  Scenario scenario = standard();
  scenario.blocks.push_back(make_block(
      "placement_policy", "placement-policy", "policy", "4", "1",
      "rule kind=require_domain_spread obligation=workload action=live_migrate "
      "destination=rack_slot domain=-\n"));
  scenario.set_body("failure_domain",
                    "member kind=rack rack=rack-A domain=fd-1\n"
                    "member kind=obligation obligation=w1 domain=fd-1\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::FailureDomainUnknown));
}

REP_TEST(planner, blocking_incident_stops_evacuation) {
  Scenario scenario = standard();
  scenario.blocks.push_back(make_block(
      "maintenance", "maintenance-authority", "maintenance", "4", "1",
      "incident domain=fd-2 severity=critical blocks=true\n"));
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(residual->reason == rep::ResidualReason::IncidentActive);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::IncidentActive));

  Scenario advisory = standard();
  advisory.blocks.push_back(make_block(
      "maintenance", "maintenance-authority", "maintenance", "4", "1",
      "incident domain=fd-2 severity=critical blocks=false\n"));
  auto second = plan_scenario(advisory.text());
  REP_REQUIRE(second.ok());
  REP_CHECK(second.value().plan.status == rep::PlanStatus::Complete);
}

REP_TEST(planner, maintenance_windows_open_and_close_actions) {
  Scenario required = standard();
  required.set_body("candidate_offers",
                    "candidate id=c1 obligation=w1 action=live_migrate "
                    "destination=rack_slot:rack-B authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=10 window=true\n");
  auto no_window = plan_scenario(required.text());
  REP_REQUIRE(no_window.ok());
  const rep::Residual* residual = find_residual(no_window.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(residual->reason == rep::ResidualReason::MaintenanceBlocked);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::MaintenanceWindowRequired));

  Scenario open_window = required;
  open_window.blocks.push_back(make_block(
      "maintenance", "maintenance-authority", "maintenance", "4", "1",
      "window domain=- destination=rack_slot:rack-B action=live_migrate start=500 end=1500 "
      "permits=true\n"));
  auto allowed = plan_scenario(open_window.text());
  REP_REQUIRE(allowed.ok());
  REP_CHECK(allowed.value().plan.status == rep::PlanStatus::Complete);

  Scenario closed_window = required;
  closed_window.blocks.push_back(make_block(
      "maintenance", "maintenance-authority", "maintenance", "4", "1",
      "window domain=- destination=rack_slot:rack-B action=live_migrate start=500 end=1500 "
      "permits=false\n"));
  auto frozen = plan_scenario(closed_window.text());
  REP_REQUIRE(frozen.ok());
  const rep::Residual* other = find_residual(frozen.value().plan, "w1");
  REP_REQUIRE(other != nullptr);
  REP_CHECK(has_rejection(*other, rep::RejectionReason::MaintenanceWindowClosed));
}

REP_TEST(planner, window_outside_the_evaluation_instant_is_not_open) {
  Scenario scenario = standard();
  scenario.set_body("candidate_offers",
                    "candidate id=c1 obligation=w1 action=live_migrate "
                    "destination=rack_slot:rack-B authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=10 window=true\n");
  scenario.blocks.push_back(make_block(
      "maintenance", "maintenance-authority", "maintenance", "4", "1",
      "window domain=- destination=rack_slot:rack-B action=live_migrate start=5000 end=6000 "
      "permits=true\n"));
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Residual* residual = find_residual(outcome.value().plan, "w1");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(has_rejection(*residual, rep::RejectionReason::MaintenanceWindowRequired));
}

// ---------------------------------------------------------------------------
// Dependency ordering
// ---------------------------------------------------------------------------

REP_TEST(planner, dependencies_order_assignments_into_waves) {
  Scenario scenario = standard();
  scenario.set_body("rack_composition", "rack rack-A\nrevision 7\noccupants w1,w2\n");
  scenario.set_body("enumeration", "rack rack-A\nrevision 7\ncomplete true\nenumerated w1,w2\n");
  scenario.set_body("obligation_catalog",
                    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=false depends=-\n"
                    "obligation id=w2 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=false depends=w1\n");
  scenario.set_body("asi_workload_state",
                    "record obligation=w1 lifecycle=running migration=live_allowed "
                    "attachment=stateless\n"
                    "record obligation=w2 lifecycle=running migration=live_allowed "
                    "attachment=stateless\n");
  scenario.set_body("candidate_offers",
                    "candidate id=c1 obligation=w1 action=live_migrate "
                    "destination=rack_slot:rack-B authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=10 window=false\n"
                    "candidate id=c2 obligation=w2 action=live_migrate "
                    "destination=rack_slot:rack-C authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=10 window=false\n");
  scenario.set_body("capacity",
                    "destination destination=rack_slot:rack-B available=cpu_millicores:8000\n"
                    "destination destination=rack_slot:rack-C available=cpu_millicores:8000\n");
  scenario.set_body("failure_domain",
                    "member kind=rack rack=rack-A domain=fd-1\n"
                    "member kind=destination destination=rack_slot:rack-B domain=fd-2\n"
                    "member kind=destination destination=rack_slot:rack-C domain=fd-3\n"
                    "member kind=obligation obligation=w1 domain=fd-1\n"
                    "member kind=obligation obligation=w2 domain=fd-1\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_REQUIRE(outcome.value().plan.status == rep::PlanStatus::Complete);
  const rep::Plan& plan = outcome.value().plan;
  REP_REQUIRE(plan.assignments.size() == 2);
  REP_CHECK_EQ(plan.assignments[0].obligation.str(), std::string("w1"));
  REP_CHECK_EQ(plan.assignments[0].wave, std::uint32_t{0});
  REP_CHECK_EQ(plan.assignments[1].obligation.str(), std::string("w2"));
  REP_CHECK_EQ(plan.assignments[1].wave, std::uint32_t{1});
  REP_CHECK_EQ(plan.wave_count(), std::uint32_t{2});
}

REP_TEST(planner, dependency_on_an_out_of_scope_obligation_is_ignored) {
  Scenario scenario = standard();
  scenario.kinds = "workload";
  scenario.set_body("rack_composition", "rack rack-A\nrevision 7\noccupants w1,n1\n");
  scenario.set_body("enumeration", "rack rack-A\nrevision 7\ncomplete true\nenumerated w1,n1\n");
  scenario.set_body("obligation_catalog",
                    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=false depends=n1\n"
                    "obligation id=n1 kind=network_path rack=rack-A demand=network_kib:100 "
                    "protected=false depends=-\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_REQUIRE(outcome.value().plan.status == rep::PlanStatus::Complete);
  REP_CHECK_EQ(outcome.value().plan.assignments.front().wave, std::uint32_t{0});
  REP_CHECK_EQ(outcome.value().plan.out_of_scope.size(), std::size_t{1});
}

REP_TEST(planner, dependency_cycle_makes_every_member_residual) {
  Scenario scenario = standard();
  scenario.set_body("rack_composition", "rack rack-A\nrevision 7\noccupants w1,w2\n");
  scenario.set_body("enumeration", "rack rack-A\nrevision 7\ncomplete true\nenumerated w1,w2\n");
  scenario.set_body("obligation_catalog",
                    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=false depends=w2\n"
                    "obligation id=w2 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=false depends=w1\n");
  scenario.set_body("asi_workload_state",
                    "record obligation=w1 lifecycle=running migration=live_allowed "
                    "attachment=stateless\n"
                    "record obligation=w2 lifecycle=running migration=live_allowed "
                    "attachment=stateless\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Plan& plan = outcome.value().plan;
  REP_CHECK(plan.status == rep::PlanStatus::Partial);
  REP_CHECK(plan.assignments.empty());
  REP_REQUIRE(plan.residuals.size() == 2);
  REP_CHECK(plan.residuals[0].reason == rep::ResidualReason::DependencyCycle);
  REP_CHECK(plan.residuals[1].reason == rep::ResidualReason::DependencyCycle);
}

REP_TEST(planner, dependent_of_a_residual_obligation_is_unsatisfied) {
  Scenario scenario = standard();
  scenario.set_body("rack_composition", "rack rack-A\nrevision 7\noccupants w1,w2\n");
  scenario.set_body("enumeration", "rack rack-A\nrevision 7\ncomplete true\nenumerated w1,w2\n");
  scenario.set_body("obligation_catalog",
                    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=false depends=-\n"
                    "obligation id=w2 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=false depends=w1\n");
  // w1 has no candidate, so w2 cannot be ordered after a completed w1.
  scenario.set_body("candidate_offers",
                    "candidate id=c2 obligation=w2 action=live_migrate "
                    "destination=rack_slot:rack-B authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=10 window=false\n");
  scenario.set_body("asi_workload_state",
                    "record obligation=w1 lifecycle=running migration=live_allowed "
                    "attachment=stateless\n"
                    "record obligation=w2 lifecycle=running migration=live_allowed "
                    "attachment=stateless\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Plan& plan = outcome.value().plan;
  REP_REQUIRE(plan.residuals.size() == 2);
  const rep::Residual* first = find_residual(plan, "w1");
  const rep::Residual* second = find_residual(plan, "w2");
  REP_REQUIRE(first != nullptr);
  REP_REQUIRE(second != nullptr);
  REP_CHECK(first->reason == rep::ResidualReason::NoCandidate);
  REP_CHECK(second->reason == rep::ResidualReason::DependencyUnsatisfied);
  REP_CHECK(plan.assignments.empty());
}

// ---------------------------------------------------------------------------
// Capacity accounting and conservation
// ---------------------------------------------------------------------------

REP_TEST(planner, capacity_is_consumed_greedily_in_order) {
  Scenario scenario = standard();
  scenario.set_body("rack_composition", "rack rack-A\nrevision 7\noccupants w1,w2\n");
  scenario.set_body("enumeration", "rack rack-A\nrevision 7\ncomplete true\nenumerated w1,w2\n");
  scenario.set_body("obligation_catalog",
                    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=false depends=-\n"
                    "obligation id=w2 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=false depends=-\n");
  scenario.set_body("asi_workload_state",
                    "record obligation=w1 lifecycle=running migration=live_allowed "
                    "attachment=stateless\n"
                    "record obligation=w2 lifecycle=running migration=live_allowed "
                    "attachment=stateless\n");
  scenario.set_body("candidate_offers",
                    "candidate id=c1 obligation=w1 action=live_migrate "
                    "destination=rack_slot:rack-B authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=10 window=false\n"
                    "candidate id=c2 obligation=w2 action=live_migrate "
                    "destination=rack_slot:rack-B authority=agent-scheduler generation=4 epoch=1 "
                    "provision=cpu_millicores:1000 cost=10 window=false\n");
  scenario.set_body("capacity",
                    "destination destination=rack_slot:rack-B available=cpu_millicores:1500\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Plan& plan = outcome.value().plan;
  REP_CHECK(plan.status == rep::PlanStatus::Partial);
  REP_REQUIRE(plan.assignments.size() == 1);
  REP_CHECK_EQ(plan.assignments.front().obligation.str(), std::string("w1"));
  const rep::Residual* residual = find_residual(plan, "w2");
  REP_REQUIRE(residual != nullptr);
  REP_CHECK(residual->reason == rep::ResidualReason::CapacityExhausted);
}

REP_TEST(planner, conservation_holds_over_every_outcome_list) {
  Scenario scenario = standard();
  scenario.set_body("rack_composition", "rack rack-A\nrevision 7\noccupants w1,w2,n1\n");
  scenario.set_body("enumeration",
                    "rack rack-A\nrevision 7\ncomplete true\nenumerated w1,w2,n1\n");
  scenario.set_body("obligation_catalog",
                    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=false depends=-\n"
                    "obligation id=w2 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=true depends=-\n"
                    "obligation id=n1 kind=network_path rack=rack-A demand=network_kib:100 "
                    "protected=false depends=-\n");
  scenario.set_body("asi_workload_state",
                    "record obligation=w1 lifecycle=running migration=live_allowed "
                    "attachment=stateless\n"
                    "record obligation=w2 lifecycle=running migration=live_allowed "
                    "attachment=stateless\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  const rep::Plan& plan = outcome.value().plan;
  REP_CHECK_EQ(plan.obligation_count(), std::size_t{3});
  REP_CHECK_EQ(plan.assignments.size(), std::size_t{1});
  REP_CHECK_EQ(plan.residuals.size(), std::size_t{1});
  REP_CHECK_EQ(plan.out_of_scope.size(), std::size_t{1});

  // The independent auditor recomputes conservation from the request.
  auto document = rep::parse_scenario(scenario.text());
  REP_REQUIRE(document.ok());
  const std::vector<rep::AuditFinding> findings = rep::audit_plan(plan, document.value().request);
  REP_CHECK_MSG(findings.empty(), findings.empty() ? "" : findings.front().code + ": " +
                                                               findings.front().detail);
}

REP_TEST(planner, identical_input_produces_an_identical_plan_digest) {
  const std::string text = standard().text();
  auto first = plan_scenario(text);
  auto second = plan_scenario(text);
  REP_REQUIRE(first.ok());
  REP_REQUIRE(second.ok());
  REP_CHECK(first.value().plan.plan_digest == second.value().plan.plan_digest);
  REP_CHECK(first.value().plan.to_text() == second.value().plan.to_text());
}

REP_TEST(planner, conflicting_bound_capacity_streams_are_refused) {
  Scenario scenario = standard();
  scenario.blocks.push_back(make_block(
      "capacity", "other-capacity", "capacity-2", "4", "1",
      "destination destination=rack_slot:rack-B available=cpu_millicores:1\n"));
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().kind == rep::OutcomeKind::RejectedInput);
  REP_CHECK(outcome.value().error.code == rep::ErrorCode::DuplicateIdentity);
  REP_CHECK(!outcome.value().has_plan);
}

REP_TEST(planner, unbound_records_are_carried_but_never_used) {
  // An offers record that no source directive binds sits in the bundle.  It
  // must not become evidence, so the plan still resolves from the bound
  // stream alone, and the unbound stream never appears in the plan bindings.
  Scenario scenario = standard();
  scenario.add_unbound("candidate_offers",
                       "candidate id=c1 obligation=w1 action=live_migrate "
                       "destination=rack_slot:rack-Z authority=other-scheduler generation=9 "
                       "epoch=9 provision=cpu_millicores:1000 cost=1 window=false\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  REP_REQUIRE(outcome.value().committed());
  REP_CHECK(outcome.value().plan.status == rep::PlanStatus::Complete);
  REP_CHECK_EQ(outcome.value().plan.assignments.front().candidate.str(), std::string("c1"));
  REP_CHECK_EQ(outcome.value().plan.assignments.front().destination.to_text(),
               std::string("rack_slot:rack-B"));
  REP_CHECK_EQ(outcome.value().plan.bindings.size(), std::size_t{7});
  for (const rep::PlanBinding& binding : outcome.value().plan.bindings) {
    REP_CHECK(binding.stream.stream.str() != "offers-unbound");
  }
}

REP_TEST(planner, audit_detects_a_tampered_assignment) {
  auto document = rep::parse_scenario(standard().text());
  REP_REQUIRE(document.ok());
  auto outcome = plan_scenario(standard().text());
  REP_REQUIRE(outcome.ok());
  rep::Plan tampered = outcome.value().plan;
  REP_REQUIRE(tampered.assignments.size() == 1);
  tampered.assignments.front().destination = rep::DestinationRef::parse("rack_slot:rack-Z").value();
  // The content digest no longer matches, and the auditor reports integrity.
  const std::vector<rep::AuditFinding> findings = rep::audit_plan(tampered, document.value().request);
  REP_REQUIRE(!findings.empty());
  REP_CHECK_EQ(findings.front().code, std::string("plan_integrity"));
}

REP_TEST_MAIN()
