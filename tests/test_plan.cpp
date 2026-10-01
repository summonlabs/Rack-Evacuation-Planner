// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The plan artifact: sealing, self-consistency, staleness fencing, verdicts,
// the canonical text report, and the independent auditor.

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rep/rep.hpp>

#include "planner_fixture.hpp"
#include "testkit.hpp"

using namespace repfix;

namespace {

// "single" plans a complete evacuation, "multi" a partial one that also has a
// residual and an out-of-scope obligation.
[[nodiscard]] rep::Result<rep::Plan> sealed_plan(std::string_view flavour = "single") {
  Scenario scenario = standard();
  if (flavour == "multi") {
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
  }
  auto outcome = plan_scenario(scenario.text());
  if (!outcome.ok()) {
    return outcome.error();
  }
  if (!outcome.value().has_plan) {
    return make_error(rep::ErrorCode::Internal, "scenario did not commit a plan");
  }
  return outcome.value().plan;
}

[[nodiscard]] rep::Result<rep::EvidenceBundle> bundle_for(const std::string& text) {
  auto document = rep::parse_scenario(text);
  if (!document.ok()) {
    return document.error();
  }
  return document.value().request.evidence;
}

[[nodiscard]] bool has_finding(const std::vector<rep::AuditFinding>& findings,
                               std::string_view code) {
  return std::any_of(findings.begin(), findings.end(),
                     [code](const rep::AuditFinding& finding) { return finding.code == code; });
}

} // namespace

REP_TEST(plan, seal_produces_a_verifiable_digest) {
  auto plan = sealed_plan();
  REP_REQUIRE(plan.ok());
  REP_REQUIRE(plan.value().status == rep::PlanStatus::Complete);
  REP_CHECK(plan.value().verify().ok());
  REP_CHECK(plan.value().content_digest() == plan.value().plan_digest);
  const rep::Plan copy = plan.value();
  REP_CHECK(copy == plan.value());
  rep::Plan changed = plan.value();
  changed.status = rep::PlanStatus::Partial;
  REP_CHECK(!(changed == plan.value()));
  REP_CHECK(changed.verify().error().code == rep::ErrorCode::DigestMismatch);
}

REP_TEST(plan, every_content_field_participates_in_the_digest) {
  auto plan = sealed_plan();
  REP_REQUIRE(plan.ok());
  const rep::Digest original = plan.value().plan_digest;

  rep::Plan renamed = plan.value();
  renamed.id = rep::PlanId::parse("planner-01-000000000099").value();
  REP_CHECK(!(renamed.content_digest() == original));

  rep::Plan revisioned = plan.value();
  revisioned.revision = rep::PlanRevision{7};
  REP_CHECK(!(revisioned.content_digest() == original));

  rep::Plan resequenced = plan.value();
  resequenced.sequence = rep::Sequence{99};
  REP_CHECK(!(resequenced.content_digest() == original));

  rep::Plan rekeyed = plan.value();
  rekeyed.idempotency_key = rep::IdempotencyKey::parse("other-key").value();
  REP_CHECK(!(rekeyed.content_digest() == original));

  rep::Plan rebound = plan.value();
  rebound.bindings.clear();
  REP_CHECK(!(rebound.content_digest() == original));

  rep::Plan rescoped = plan.value();
  rescoped.evacuate_kinds = {rep::ObligationKind::NetworkPath};
  REP_CHECK(!(rescoped.content_digest() == original));

  rep::Plan reassigned = plan.value();
  reassigned.assignments.front().wave = 4;
  REP_CHECK(!(reassigned.content_digest() == original));

  // The plan digest is excluded from the content it covers, by construction.
  rep::Plan digest_only = plan.value();
  digest_only.plan_digest = rep::Digest{};
  REP_CHECK(digest_only.content_digest() == original);
}

REP_TEST(plan, verify_refuses_structural_inconsistencies) {
  auto base = sealed_plan();
  REP_REQUIRE(base.ok());
  REP_REQUIRE(base.value().status == rep::PlanStatus::Complete);

  rep::Plan sparse = base.value();
  sparse.assignments.front().order_index = 3;
  REP_CHECK(rep::Plan::seal(sparse).error().code == rep::ErrorCode::InvalidArgument);

  rep::Plan duplicated = base.value();
  rep::Assignment clone = duplicated.assignments.front();
  clone.order_index = static_cast<std::uint32_t>(duplicated.assignments.size());
  duplicated.assignments.push_back(clone);
  REP_CHECK(rep::Plan::seal(duplicated).error().code == rep::ErrorCode::DuplicateIdentity);

  rep::Plan overlapping = base.value();
  rep::Residual clash;
  clash.obligation = base.value().assignments.front().obligation;
  clash.kind = base.value().assignments.front().kind;
  clash.reason = rep::ResidualReason::NoCandidate;
  overlapping.residuals.push_back(clash);
  overlapping.status = rep::PlanStatus::Partial;
  REP_CHECK(rep::Plan::seal(overlapping).error().code == rep::ErrorCode::DuplicateIdentity);

  rep::Plan partial_without_residual = base.value();
  partial_without_residual.status = rep::PlanStatus::Partial;
  REP_CHECK(rep::Plan::seal(partial_without_residual).error().code == rep::ErrorCode::InvalidArgument);

  rep::Plan complete_with_residual = base.value();
  rep::Residual other;
  other.obligation = rep::ObligationId::parse("zz").value();
  other.kind = rep::ObligationKind::Workload;
  other.reason = rep::ResidualReason::NoCandidate;
  complete_with_residual.residuals.push_back(other);
  REP_CHECK(rep::Plan::seal(complete_with_residual).error().code == rep::ErrorCode::InvalidArgument);

  rep::Plan empty_safe_with_assignment = base.value();
  empty_safe_with_assignment.status = rep::PlanStatus::EmptySafe;
  REP_CHECK(rep::Plan::seal(empty_safe_with_assignment).error().code ==
            rep::ErrorCode::InvalidArgument);

  rep::Plan indeterminate_with_assignment = base.value();
  indeterminate_with_assignment.status = rep::PlanStatus::Indeterminate;
  indeterminate_with_assignment.indeterminacy_reasons = {rep::IndeterminacyReason::EnumerationMissing};
  REP_CHECK(rep::Plan::seal(indeterminate_with_assignment).error().code ==
            rep::ErrorCode::InvalidArgument);

  rep::Plan indeterminate_without_reason = base.value();
  indeterminate_without_reason.status = rep::PlanStatus::Indeterminate;
  indeterminate_without_reason.assignments.clear();
  indeterminate_without_reason.residuals.clear();
  indeterminate_without_reason.out_of_scope.clear();
  REP_CHECK(rep::Plan::seal(indeterminate_without_reason).error().code ==
            rep::ErrorCode::InvalidArgument);

  rep::Plan unsorted_bindings = base.value();
  REP_REQUIRE(unsorted_bindings.bindings.size() >= 2);
  std::swap(unsorted_bindings.bindings.front(), unsorted_bindings.bindings.back());
  REP_CHECK(rep::Plan::seal(unsorted_bindings).error().code == rep::ErrorCode::InvalidArgument);

  rep::Plan no_kinds = base.value();
  no_kinds.evacuate_kinds.clear();
  REP_CHECK(rep::Plan::seal(no_kinds).error().code == rep::ErrorCode::InvalidArgument);

  rep::Plan no_binding = base.value();
  no_binding.bindings.clear();
  REP_CHECK(rep::Plan::seal(no_binding).ok());
}

REP_TEST(plan, staleness_is_empty_against_the_consumed_evidence) {
  const std::string text = standard().text();
  auto plan = sealed_plan();
  REP_REQUIRE(plan.ok());
  auto bundle = bundle_for(text);
  REP_REQUIRE(bundle.ok());
  bool missing = true;
  const std::vector<rep::StalenessFinding> findings = plan.value().staleness(bundle.value(), missing);
  REP_CHECK(findings.empty());
  REP_CHECK(!missing);
  REP_CHECK(plan.value().verdict(findings) == rep::SafetyVerdict::Safe);
}

REP_TEST(plan, a_generation_move_makes_the_plan_stale) {
  auto plan = sealed_plan();
  REP_REQUIRE(plan.ok());
  Scenario republished = standard();
  for (Block& block : republished.blocks) {
    if (block.kind == "rack_composition") {
      block.generation = "8";
    }
  }
  auto bundle = bundle_for(republished.text());
  REP_REQUIRE(bundle.ok());
  bool missing = false;
  const std::vector<rep::StalenessFinding> findings =
      plan.value().staleness(bundle.value(), missing);
  REP_REQUIRE(findings.size() == 1);
  REP_CHECK(findings.front().kind == rep::StalenessKind::GenerationMoved);
  REP_CHECK_EQ(findings.front().plan_generation.value(), std::uint64_t{7});
  REP_CHECK_EQ(findings.front().current_generation.value(), std::uint64_t{8});
  REP_CHECK(!missing);
  REP_CHECK(plan.value().verdict(findings) == rep::SafetyVerdict::Stale);
}

REP_TEST(plan, an_epoch_move_and_a_digest_move_are_reported) {
  auto plan = sealed_plan();
  REP_REQUIRE(plan.ok());

  Scenario epoch_moved = standard();
  for (Block& block : epoch_moved.blocks) {
    if (block.kind == "capacity") {
      block.epoch = "9";
    }
  }
  auto bundle = bundle_for(epoch_moved.text());
  REP_REQUIRE(bundle.ok());
  const std::vector<rep::StalenessFinding> epoch_findings = plan.value().staleness(bundle.value());
  // The destination-capacity payload embeds its own generation and epoch, so
  // moving the stream epoch also moves the payload digest.  Both must show.
  REP_REQUIRE(epoch_findings.size() == 2);
  REP_CHECK(epoch_findings[0].kind == rep::StalenessKind::EpochChanged);
  REP_CHECK(epoch_findings[1].kind == rep::StalenessKind::DigestChanged);
  for (const rep::StalenessFinding& finding : epoch_findings) {
    REP_CHECK(finding.stream.stream.str() == "capacity");
  }

  Scenario digest_moved = standard();
  digest_moved.set_body("capacity",
                        "destination destination=rack_slot:rack-B available=cpu_millicores:9000\n");
  auto other = bundle_for(digest_moved.text());
  REP_REQUIRE(other.ok());
  const std::vector<rep::StalenessFinding> digest_findings = plan.value().staleness(other.value());
  REP_REQUIRE(digest_findings.size() == 1);
  REP_CHECK(digest_findings.front().kind == rep::StalenessKind::DigestChanged);
}

REP_TEST(plan, a_missing_stream_is_reported_as_missing) {
  auto plan = sealed_plan();
  REP_REQUIRE(plan.ok());
  Scenario trimmed = standard();
  trimmed.remove("capacity");
  auto bundle = bundle_for(trimmed.text());
  REP_REQUIRE(bundle.ok());
  bool missing = false;
  const std::vector<rep::StalenessFinding> findings = plan.value().staleness(bundle.value(), missing);
  REP_REQUIRE(findings.size() == 1);
  REP_CHECK(findings.front().kind == rep::StalenessKind::StreamMissing);
  REP_CHECK(missing);
}

REP_TEST(plan, verdict_without_findings_follows_the_status) {
  auto complete = sealed_plan();
  REP_REQUIRE(complete.ok());
  REP_CHECK(complete.value().verdict() == rep::SafetyVerdict::Safe);

  Scenario partial_scenario = standard();
  partial_scenario.set_body("candidate_offers", "");
  auto partial = plan_scenario(partial_scenario.text());
  REP_REQUIRE(partial.ok());
  REP_CHECK(partial.value().plan.status == rep::PlanStatus::Partial);
  REP_CHECK(partial.value().plan.verdict() == rep::SafetyVerdict::NotProven);

  Scenario indeterminate_scenario = standard();
  indeterminate_scenario.set_body("enumeration",
                                  "rack rack-A\nrevision 7\ncomplete false\nenumerated w1\n");
  auto indeterminate = plan_scenario(indeterminate_scenario.text());
  REP_REQUIRE(indeterminate.ok());
  REP_CHECK(indeterminate.value().plan.status == rep::PlanStatus::Indeterminate);
  REP_CHECK(indeterminate.value().plan.verdict() == rep::SafetyVerdict::Indeterminate);
}

REP_TEST(plan, text_report_is_stable_and_complete) {
  auto plan = sealed_plan("multi");
  REP_REQUIRE(plan.ok());
  REP_REQUIRE(plan.value().status == rep::PlanStatus::Partial);
  const std::string text = plan.value().to_text();
  REP_CHECK(text == plan.value().to_text());
  REP_CHECK(!text.empty());
  REP_CHECK_EQ(text.back(), '\n');
  REP_CHECK(text.rfind("plan id=", 0) == 0);
  REP_CHECK(text.find("\nsource rack=rack-A ") != std::string::npos);
  REP_CHECK(text.find("status=partial") != std::string::npos);
  REP_CHECK(text.find("status_verdict=not_proven") != std::string::npos);
  REP_CHECK(text.find("plan_digest=" + plan.value().plan_digest.to_hex()) != std::string::npos);
  REP_CHECK(text.find("binding kind=") != std::string::npos);
  REP_CHECK(text.find("out_of_scope obligation=n1 kind=network_path "
                      "reason=kind_not_requested") != std::string::npos);
  REP_CHECK(text.find("residual obligation=w2 kind=workload reason=protected_obligation") !=
            std::string::npos);
  REP_CHECK(text.find("assignment order=0 obligation=w1") != std::string::npos);

  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string line = text.substr(start, end - start);
    REP_CHECK(!line.empty());
    REP_CHECK(line.back() != ' ');
    REP_CHECK(line.find('\t') == std::string::npos);
    start = end + 1;
  }
}

REP_TEST(plan, counters_describe_the_artifact) {
  auto plan = sealed_plan("multi");
  REP_REQUIRE(plan.ok());
  REP_CHECK_EQ(plan.value().obligation_count(),
               plan.value().assignments.size() + plan.value().residuals.size() +
                   plan.value().out_of_scope.size());
  REP_CHECK_EQ(plan.value().obligation_count(), std::size_t{3});
  REP_CHECK_EQ(plan.value().wave_count(), std::uint32_t{1});

  Scenario empty_scenario = standard();
  empty_scenario.set_body("rack_composition", "rack rack-A\nrevision 7\noccupants -\n");
  empty_scenario.set_body("enumeration", "rack rack-A\nrevision 7\ncomplete true\nenumerated -\n");
  empty_scenario.remove("obligation_catalog");
  empty_scenario.remove("asi_workload_state");
  empty_scenario.remove("candidate_offers");
  auto empty = plan_scenario(empty_scenario.text());
  REP_REQUIRE(empty.ok());
  REP_CHECK_EQ(empty.value().plan.wave_count(), std::uint32_t{0});
  REP_CHECK_EQ(empty.value().plan.obligation_count(), std::size_t{0});
}

REP_TEST(plan, audit_accepts_a_faithful_plan_and_finds_a_mismatched_request) {
  const std::string text = standard().text();
  auto plan = sealed_plan();
  REP_REQUIRE(plan.ok());
  auto document = rep::parse_scenario(text);
  REP_REQUIRE(document.ok());
  REP_CHECK(rep::audit_plan(plan.value(), document.value().request).empty());

  Scenario other = standard();
  other.key = "req-2";
  auto other_document = rep::parse_scenario(other.text());
  REP_REQUIRE(other_document.ok());
  const std::vector<rep::AuditFinding> findings =
      rep::audit_plan(plan.value(), other_document.value().request);
  REP_REQUIRE(!findings.empty());
  REP_CHECK(has_finding(findings, "request_digest_mismatch"));
}

REP_TEST(plan, audit_reports_integrity_first_and_semantics_after_resealing) {
  auto document = rep::parse_scenario(standard().text());
  REP_REQUIRE(document.ok());
  auto plan = sealed_plan();
  REP_REQUIRE(plan.ok());

  // Tampering after sealing is caught by the digest before anything else.
  rep::Plan tampered = plan.value();
  tampered.assignments.front().destination = rep::DestinationRef::parse("rack_slot:rack-Z").value();
  const std::vector<rep::AuditFinding> integrity = rep::audit_plan(tampered, document.value().request);
  REP_REQUIRE(!integrity.empty());
  REP_CHECK_EQ(integrity.front().code, std::string("plan_integrity"));

  // A correctly sealed plan can still be wrong, and the auditor says how.
  auto resealed = rep::Plan::seal(tampered);
  REP_REQUIRE(resealed.ok());
  const std::vector<rep::AuditFinding> semantic =
      rep::audit_plan(resealed.value(), document.value().request);
  REP_CHECK(!semantic.empty());
  REP_CHECK(has_finding(semantic, "assignment_candidate_mismatch"));
}

REP_TEST(plan, audit_reports_a_scope_that_does_not_match_the_request) {
  Scenario scenario = standard();
  scenario.set_body("rack_composition", "rack rack-A\nrevision 7\noccupants w1,n1\n");
  scenario.set_body("enumeration", "rack rack-A\nrevision 7\ncomplete true\nenumerated w1,n1\n");
  scenario.set_body("obligation_catalog",
                    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                    "protected=false depends=-\n"
                    "obligation id=n1 kind=network_path rack=rack-A demand=network_kib:100 "
                    "protected=false depends=-\n");
  auto outcome = plan_scenario(scenario.text());
  REP_REQUIRE(outcome.ok());
  auto document = rep::parse_scenario(scenario.text());
  REP_REQUIRE(document.ok());
  REP_CHECK(rep::audit_plan(outcome.value().plan, document.value().request).empty());

  // The same plan audited against a request that also evacuates network paths
  // no longer conserves: n1 is missing an outcome.
  auto wider = rep::IsolationRequest::make(document.value().request.isolation.rack,
                                           rep::IsolationKind::Depower, rep::Generation{7},
                                           {rep::ObligationKind::Workload,
                                            rep::ObligationKind::NetworkPath});
  REP_REQUIRE(wider.ok());
  auto wider_request = rep::PlanRequest::make(
      document.value().request.idempotency_key, document.value().request.requested_by,
      document.value().request.expected_epoch, document.value().request.evaluation_time,
      wider.value(), document.value().request.lineage,
      document.value().request.evidence_sources, document.value().request.evidence);
  REP_REQUIRE(wider_request.ok());
  const std::vector<rep::AuditFinding> findings =
      rep::audit_plan(outcome.value().plan, wider_request.value());
  REP_CHECK(has_finding(findings, "request_binding_mismatch"));
  REP_CHECK(has_finding(findings, "request_digest_mismatch"));
}

REP_TEST(plan, staleness_findings_are_sorted) {
  auto plan = sealed_plan();
  REP_REQUIRE(plan.ok());
  Scenario moved = standard();
  for (Block& block : moved.blocks) {
    block.generation = "9";
    block.epoch = "9";
  }
  auto bundle = bundle_for(moved.text());
  REP_REQUIRE(bundle.ok());
  const std::vector<rep::StalenessFinding> findings = plan.value().staleness(bundle.value());
  // Every bound stream moved, so every binding produces at least one finding.
  REP_CHECK(findings.size() >= plan.value().bindings.size());
  for (std::size_t index = 1; index < findings.size(); ++index) {
    REP_CHECK(!(findings[index] < findings[index - 1]));
  }
  REP_CHECK(std::is_sorted(findings.begin(), findings.end()));
}

REP_TEST_MAIN()
