// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// plan_walkthrough: a self-contained example, usable as a smoke test.
//
// It builds a minimal depower request by hand, plans it in a non-durable
// in-memory engine, prints the canonical plan text, then republishes the
// rack-composition stream one generation later and shows that the first plan
// is fenced as stale.  The exit code is zero only when every step behaved as
// the comments below describe.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rep/rep.hpp"

namespace {

// The scenario: one reservation held in rack A is released to rack B.
constexpr std::string_view kRack = "rack-a";
constexpr std::string_view kRequestedBy = "walkthrough-authority";
constexpr std::string_view kRackAuthority = "rack-authority";
constexpr std::string_view kCatalogAuthority = "catalog-authority";
constexpr std::string_view kCapacityAuthority = "capacity-authority";
constexpr std::string_view kPolicyAuthority = "policy-authority";
constexpr std::string_view kDomainAuthority = "failure-domain-authority";
constexpr std::string_view kMigrationAuthority = "migration-authority";
constexpr std::string_view kCompositionStream = "rack-a-composition";
constexpr std::string_view kEnumerationStream = "rack-a-enumeration";
constexpr std::string_view kCatalogStream = "rack-a-obligations";
constexpr std::string_view kCapacityStream = "destination-capacity";
constexpr std::string_view kPolicyStream = "placement-policy";
constexpr std::string_view kDomainStream = "failure-domains";
constexpr std::string_view kOffersStream = "candidate-offers";
constexpr std::uint64_t kCompositionRevision = 7;
constexpr std::uint64_t kStreamGeneration = 1;
constexpr std::uint64_t kAuthorityEpoch = 5;
// 2025-01-01T00:00:00Z.  The example reads no clock: this fixed instant is
// both the request's evaluation time and the engine's epoch claim.
constexpr std::int64_t kEvaluationTimeNanos = 1735689600000000000;

// Every library call below returns a Result.  This example is a smoke test, so
// a failure is reported with its typed error and stops the process; a real
// caller decides for itself what a refused input means.
template <class T>
[[nodiscard]] T expect(rep::Result<T> result, std::string_view what) {
  if (!result.ok()) {
    std::cerr << "walkthrough: " << what << ": " << result.error().to_string() << "\n";
    std::exit(1);
  }
  return result.take();
}

[[nodiscard]] rep::StreamRef stream_of(std::string_view authority, std::string_view id) {
  return rep::StreamRef{rep::AuthorityId(std::string(authority)), rep::StreamId(std::string(id))};
}

[[nodiscard]] rep::ResourceVector amount_of(rep::ResourceClass resource, std::uint64_t amount) {
  return expect(rep::ResourceVector::make({rep::ResourceAmount{resource, amount}}),
                "resource amount");
}

// One evidence record.  The payload digest is computed here, exactly as a
// publishing authority would, because EvidenceBundle::make verifies it.
[[nodiscard]] rep::EvidenceRecord make_record(rep::EvidenceKind kind, rep::StreamRef source,
                                              rep::Generation generation, rep::Epoch epoch,
                                              rep::EvidencePayload payload) {
  rep::EvidenceRecord record;
  record.kind = kind;
  record.stamp.source = std::move(source);
  record.stamp.generation = generation;
  record.stamp.epoch = epoch;
  record.stamp.content_digest = rep::evidence_payload_digest(kind, payload);
  record.payload = std::move(payload);
  return record;
}

// One complete request.  composition_generation is the generation of the
// rack-composition stream this request consumes, so the caller can publish a
// newer generation of that stream and watch the earlier plan go stale.
[[nodiscard]] rep::PlanRequest build_request(std::uint64_t composition_generation,
                                             std::string_view key) {
  const rep::Generation revision{kCompositionRevision};
  const rep::Generation stream_generation{composition_generation};
  const rep::Generation generation{kStreamGeneration};
  const rep::Epoch epoch{kAuthorityEpoch};

  const rep::RackId rack = expect(rep::RackId::parse(kRack), "rack id");
  const rep::ObligationId reservation = expect(rep::ObligationId::parse("res-1"), "res-1");
  const rep::ResourceVector demand = amount_of(rep::ResourceClass::RackUnits, 2);
  const rep::ResourceVector capacity = amount_of(rep::ResourceClass::RackUnits, 8);

  const rep::Obligation definition = expect(
      rep::Obligation::make(reservation, rep::ObligationKind::Reservation, rack, demand, false, {}),
      "obligation definition");
  const auto catalog = expect(rep::ObligationCatalogPayload::make({definition}), "catalog");
  const auto composition =
      expect(rep::RackCompositionPayload::make(rack, revision, {reservation}), "composition");
  const auto enumeration =
      expect(rep::EnumerationPayload::make(rack, revision, true, {reservation}), "enumeration");

  const rep::DestinationRef slot =
      expect(rep::DestinationRef::parse("rack_slot:rack-b-slot-1"), "slot");
  const auto destinations = expect(
      rep::CapacityPayload::make({rep::DestinationCapacity{slot, capacity, generation, epoch}}),
      "capacity");

  rep::PolicyRule allow_rule;
  allow_rule.kind = rep::PolicyRuleKind::AllowAction;
  allow_rule.obligation_kind = rep::ObligationKind::Reservation;
  allow_rule.action = rep::ActionKind::Release;
  allow_rule.destination_kind = rep::DestinationKind::RackSlot;
  rep::PolicyRule spread_rule = allow_rule;
  spread_rule.kind = rep::PolicyRuleKind::RequireDomainSpread;
  const auto policy =
      expect(rep::PlacementPolicyPayload::make({allow_rule, spread_rule}), "placement policy");

  const rep::FailureDomainId rack_domain =
      expect(rep::FailureDomainId::parse("domain-1"), "domain-1");
  const rep::FailureDomainId slot_domain =
      expect(rep::FailureDomainId::parse("domain-2"), "domain-2");
  rep::DomainMember rack_member;
  rack_member.domain = rack_domain;
  rack_member.rack = rack;
  rack_member.is_rack = true;
  rep::DomainMember slot_member;
  slot_member.domain = slot_domain;
  slot_member.destination = slot;
  const auto domains =
      expect(rep::FailureDomainTopology::make({rack_member, slot_member}), "failure domains");

  const auto offer = expect(
      rep::Candidate::make(expect(rep::CandidateId::parse("cand-1"), "cand-1"), reservation,
                           rep::ActionKind::Release, slot,
                           rep::AuthorityId(std::string(kMigrationAuthority)), generation, epoch,
                           demand, 10u, false),
      "candidate offer");
  const auto offers = expect(rep::CandidateOffersPayload::make({offer}), "candidate offers");

  const rep::StreamRef composition_source = stream_of(kRackAuthority, kCompositionStream);
  const rep::StreamRef enumeration_source = stream_of(kRackAuthority, kEnumerationStream);
  const rep::StreamRef catalog_source = stream_of(kCatalogAuthority, kCatalogStream);
  const rep::StreamRef capacity_source = stream_of(kCapacityAuthority, kCapacityStream);
  const rep::StreamRef policy_source = stream_of(kPolicyAuthority, kPolicyStream);
  const rep::StreamRef domain_source = stream_of(kDomainAuthority, kDomainStream);
  const rep::StreamRef offers_source = stream_of(kMigrationAuthority, kOffersStream);

  const std::vector<rep::EvidenceRecord> records = {
      make_record(rep::EvidenceKind::RackComposition, composition_source, stream_generation, epoch,
                  composition),
      make_record(rep::EvidenceKind::Enumeration, enumeration_source, generation, epoch,
                  enumeration),
      make_record(rep::EvidenceKind::ObligationCatalog, catalog_source, generation, epoch, catalog),
      make_record(rep::EvidenceKind::Capacity, capacity_source, generation, epoch, destinations),
      make_record(rep::EvidenceKind::PlacementPolicy, policy_source, generation, epoch, policy),
      make_record(rep::EvidenceKind::FailureDomain, domain_source, generation, epoch, domains),
      make_record(rep::EvidenceKind::CandidateOffers, offers_source, generation, epoch, offers),
  };
  const std::vector<rep::EvidenceSource> sources = {
      {rep::EvidenceKind::RackComposition, composition_source},
      {rep::EvidenceKind::Enumeration, enumeration_source},
      {rep::EvidenceKind::ObligationCatalog, catalog_source},
      {rep::EvidenceKind::Capacity, capacity_source},
      {rep::EvidenceKind::PlacementPolicy, policy_source},
      {rep::EvidenceKind::FailureDomain, domain_source},
      {rep::EvidenceKind::CandidateOffers, offers_source},
  };

  const auto bundle = expect(rep::EvidenceBundle::make(records), "evidence bundle");
  const auto isolation = expect(
      rep::IsolationRequest::make(rack, rep::IsolationKind::Depower, revision,
                                  {rep::ObligationKind::Reservation}),
      "isolation request");
  // An expected epoch of zero asserts nothing about the engine incarnation:
  // the engine claims its own epoch when it opens.
  return expect(rep::PlanRequest::make(expect(rep::IdempotencyKey::parse(key), "key"),
                                       expect(rep::AuthorityId::parse(kRequestedBy), "requester"),
                                       rep::Epoch{0}, rep::UnixNanos{kEvaluationTimeNanos},
                                       isolation, rep::LineageId{}, sources, bundle),
                "plan request");
}

}  // namespace

int main() {
  // 1. Build the request: the rack-composition stream at generation 1, with the
  //    seven evidence kinds a minimal depower scenario needs.
  rep::PlanRequest first_request = build_request(kStreamGeneration, "walkthrough-1");

  // 2. Open a non-durable in-memory engine (an empty store directory) and
  //    submit the request.
  rep::EngineOptions options;
  options.planner = expect(rep::PlannerId::parse("walkthrough-planner"), "planner id");
  options.wall_clock = []() { return rep::UnixNanos{kEvaluationTimeNanos}; };
  auto engine = rep::PlanEngine::open(std::move(options));
  if (!engine.ok()) {
    std::cerr << "walkthrough: engine open: " << engine.error().to_string() << "\n";
    return 1;
  }
  auto submitted = engine.value()->submit(first_request);
  if (!submitted.ok()) {
    std::cerr << "walkthrough: submit: " << submitted.error().to_string() << "\n";
    return 1;
  }
  const rep::PlanOutcome& outcome = submitted.value();
  if (!outcome.committed()) {
    std::cerr << "walkthrough: request rejected: " << outcome.error.to_string() << "\n";
    return 1;
  }
  const rep::Plan& first_plan = outcome.plan;

  // 3. Print the canonical plan text.
  std::cout << "planned " << first_plan.id.str()
            << " status=" << rep::to_string(first_plan.status)
            << " verdict=" << rep::to_string(first_plan.verdict()) << "\n\n";
  std::cout << first_plan.to_text();

  bool ok = true;
  if (first_plan.status != rep::PlanStatus::Complete ||
      first_plan.verdict() != rep::SafetyVerdict::Safe) {
    std::cerr << "walkthrough: expected a complete, safe plan\n";
    ok = false;
  }

  // 4. Publish the same rack-composition stream one generation later and build
  //    the same request again against that newer generation.  The earlier plan
  //    must now report staleness and a Stale verdict.
  const rep::PlanRequest second_request = build_request(kStreamGeneration + 1, "walkthrough-2");
  const std::vector<rep::StalenessFinding> findings =
      first_plan.staleness(second_request.evidence);
  const rep::SafetyVerdict verdict = first_plan.verdict(findings);
  std::cout << "\nstaleness findings: " << findings.size() << "\n";
  for (const rep::StalenessFinding& finding : findings) {
    std::cout << "  stream=" << finding.stream.to_text()
              << " kind=" << rep::to_string(finding.kind)
              << " plan-generation=" << finding.plan_generation.value()
              << " current-generation=" << finding.current_generation.value() << "\n";
  }
  std::cout << "verdict against the newer generation: " << rep::to_string(verdict) << "\n";

  // 5. Exactly one finding, on the moved stream, is what this example claims.
  if (verdict != rep::SafetyVerdict::Stale) {
    std::cerr << "walkthrough: expected a stale verdict\n";
    ok = false;
  }
  if (findings.size() != 1u) {
    std::cerr << "walkthrough: expected exactly one staleness finding\n";
    ok = false;
  } else {
    const rep::StalenessFinding& finding = findings.front();
    if (finding.kind != rep::StalenessKind::GenerationMoved ||
        finding.stream.authority.view() != kRackAuthority ||
        finding.stream.stream.view() != kCompositionStream ||
        finding.plan_generation != rep::Generation{kStreamGeneration} ||
        finding.current_generation != rep::Generation{kStreamGeneration + 1}) {
      std::cerr << "walkthrough: unexpected staleness finding\n";
      ok = false;
    }
  }
  std::cout << (ok ? "walkthrough: ok\n" : "walkthrough: failed\n");
  return ok ? 0 : 1;
}
