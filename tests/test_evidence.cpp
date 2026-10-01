// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Tests for the evidence envelope: kind/payload agreement, payload digest
// enforcement, per-candidate declaration digests, canonical bundle ordering,
// keyed lookup, and the record limit.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "rep/candidate.hpp"
#include "rep/canonical.hpp"
#include "rep/digest.hpp"
#include "rep/evidence.hpp"
#include "rep/obligation.hpp"
#include "rep/policy.hpp"
#include "rep/status.hpp"
#include "rep/types.hpp"

#include "testkit.hpp"

namespace {

[[nodiscard]] rep::StreamRef stream(std::string_view authority, std::string_view name) {
  return rep::StreamRef{rep::AuthorityId(std::string(authority)),
                        rep::StreamId(std::string(name))};
}

[[nodiscard]] rep::EvidenceSource source(rep::EvidenceKind kind, const rep::StreamRef& ref) {
  return rep::EvidenceSource{kind, ref};
}

const rep::StreamRef kAuthorityA = stream("a-authority", "composition");
const rep::StreamRef kAuthorityB = stream("b-authority", "composition");

[[nodiscard]] rep::Candidate candidate(std::string_view id) {
  const auto made = rep::Candidate::make(
      rep::CandidateId(std::string(id)), rep::ObligationId("ob-1"), rep::ActionKind::LiveMigrate,
      rep::DestinationRef{rep::DestinationKind::RackSlot, rep::DestinationId("rack-2")},
      rep::AuthorityId("candidate-authority"), rep::Generation(4), rep::Epoch(2),
      rep::ResourceVector(), 3, false);
  REP_REQUIRE(made.ok());
  return made.value();
}

[[nodiscard]] rep::RackCompositionPayload composition_payload(std::uint64_t revision) {
  const auto made = rep::RackCompositionPayload::make(rep::RackId("rack-a"),
                                                      rep::Generation(revision),
                                                      {rep::ObligationId("ob-1")});
  REP_REQUIRE(made.ok());
  return made.value();
}

[[nodiscard]] rep::EnumerationPayload enumeration_payload(bool complete) {
  const auto made = rep::EnumerationPayload::make(
      rep::RackId("rack-a"), rep::Generation(1), complete, {rep::ObligationId("ob-1")});
  REP_REQUIRE(made.ok());
  return made.value();
}

[[nodiscard]] rep::ObligationCatalogPayload catalog_payload(bool filled) {
  std::vector<rep::Obligation> obligations;
  if (filled) {
    const auto obligation =
        rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::Workload,
                              rep::RackId("rack-a"), rep::ResourceVector(), false, {});
    REP_REQUIRE(obligation.ok());
    obligations.push_back(obligation.value());
  }
  const auto made = rep::ObligationCatalogPayload::make(std::move(obligations));
  REP_REQUIRE(made.ok());
  return made.value();
}

[[nodiscard]] rep::CapacityPayload capacity_payload(bool filled) {
  std::vector<rep::DestinationCapacity> destinations;
  if (filled) {
    rep::DestinationCapacity entry;
    entry.destination =
        rep::DestinationRef{rep::DestinationKind::RackSlot, rep::DestinationId("rack-1")};
    entry.generation = rep::Generation(1);
    entry.epoch = rep::Epoch(1);
    destinations.push_back(entry);
  }
  const auto made = rep::CapacityPayload::make(std::move(destinations));
  REP_REQUIRE(made.ok());
  return made.value();
}

[[nodiscard]] rep::PlacementPolicyPayload policy_payload(bool filled) {
  std::vector<rep::PolicyRule> rules;
  if (filled) {
    rules.push_back(rep::PolicyRule{});
  }
  const auto made = rep::PlacementPolicyPayload::make(std::move(rules));
  REP_REQUIRE(made.ok());
  return made.value();
}

[[nodiscard]] rep::MaintenancePayload maintenance_payload(bool filled) {
  std::vector<rep::MaintenanceWindow> windows;
  if (filled) {
    rep::MaintenanceWindow entry;
    entry.starts_at = rep::UnixNanos(0);
    entry.ends_at = rep::UnixNanos(10);
    windows.push_back(entry);
  }
  const auto made = rep::MaintenancePayload::make(std::move(windows), {});
  REP_REQUIRE(made.ok());
  return made.value();
}

[[nodiscard]] rep::FailureDomainTopology failure_domain_payload(bool filled) {
  std::vector<rep::DomainMember> members;
  if (filled) {
    rep::DomainMember entry;
    entry.domain = rep::FailureDomainId("domain-1");
    entry.is_rack = true;
    entry.rack = rep::RackId("rack-a");
    members.push_back(entry);
  }
  const auto made = rep::FailureDomainTopology::make(std::move(members));
  REP_REQUIRE(made.ok());
  return made.value();
}

[[nodiscard]] rep::AsiWorkloadPayload asi_payload(bool filled) {
  std::vector<rep::WorkloadStateRecord> records;
  if (filled) {
    rep::WorkloadStateRecord entry;
    entry.obligation = rep::ObligationId("ob-1");
    records.push_back(entry);
  }
  const auto made = rep::AsiWorkloadPayload::make(std::move(records));
  REP_REQUIRE(made.ok());
  return made.value();
}

[[nodiscard]] rep::DfiObligationPayload dfi_payload(bool filled) {
  std::vector<rep::FabricObligationRecord> records;
  if (filled) {
    rep::FabricObligationRecord entry;
    entry.obligation = rep::ObligationId("ob-1");
    records.push_back(entry);
  }
  const auto made = rep::DfiObligationPayload::make(std::move(records));
  REP_REQUIRE(made.ok());
  return made.value();
}

[[nodiscard]] rep::CandidateOffersPayload offers_payload(bool filled) {
  std::vector<rep::Candidate> candidates;
  if (filled) {
    candidates.push_back(candidate("cand-1"));
  }
  const auto made = rep::CandidateOffersPayload::make(std::move(candidates));
  REP_REQUIRE(made.ok());
  return made.value();
}

struct KindPayload {
  rep::EvidenceKind kind;
  rep::EvidencePayload payload;
};

[[nodiscard]] std::vector<KindPayload> kind_payload_table() {
  return {
      {rep::EvidenceKind::RackComposition, composition_payload(1)},
      {rep::EvidenceKind::Enumeration, enumeration_payload(true)},
      {rep::EvidenceKind::ObligationCatalog, catalog_payload(true)},
      {rep::EvidenceKind::Capacity, capacity_payload(true)},
      {rep::EvidenceKind::PlacementPolicy, policy_payload(true)},
      {rep::EvidenceKind::Maintenance, maintenance_payload(true)},
      {rep::EvidenceKind::FailureDomain, failure_domain_payload(true)},
      {rep::EvidenceKind::AsiWorkloadState, asi_payload(true)},
      {rep::EvidenceKind::DfiObligation, dfi_payload(true)},
      {rep::EvidenceKind::CandidateOffers, offers_payload(true)},
  };
}

[[nodiscard]] rep::EvidenceRecord record_of(rep::EvidenceKind kind, const rep::StreamRef& source,
                                            const rep::EvidencePayload& payload,
                                            std::uint64_t generation = 1,
                                            std::uint64_t epoch = 1) {
  rep::EvidenceRecord record;
  record.kind = kind;
  record.stamp.source = source;
  record.stamp.generation = rep::Generation(generation);
  record.stamp.epoch = rep::Epoch(epoch);
  record.stamp.content_digest = rep::evidence_payload_digest(kind, payload);
  record.payload = payload;
  return record;
}

// Recomputes the payload digest from the payload's own encode() under the tag
// that evidence_payload_domain names, independently of the dispatch inside
// evidence_payload_digest.
[[nodiscard]] rep::Digest payload_digest(rep::EvidenceKind kind,
                                         const rep::EvidencePayload& payload) {
  rep::CanonicalWriter writer(rep::evidence_payload_domain(kind));
  std::visit([&writer](const auto& value) { value.encode(writer); }, payload);
  return writer.finish();
}

struct ChangeCase {
  rep::EvidenceKind kind;
  rep::EvidencePayload before;
  rep::EvidencePayload after;
};

}  // namespace

REP_TEST(Evidence, PayloadDomainIsDistinctPerKind) {
  // Each kind maps to its own documented domain tag.
  REP_CHECK_EQ(rep::evidence_payload_domain(rep::EvidenceKind::RackComposition),
               rep::domains::kRackComposition);
  REP_CHECK_EQ(rep::evidence_payload_domain(rep::EvidenceKind::Enumeration),
               rep::domains::kEnumeration);
  REP_CHECK_EQ(rep::evidence_payload_domain(rep::EvidenceKind::ObligationCatalog),
               rep::domains::kObligationCatalog);
  REP_CHECK_EQ(rep::evidence_payload_domain(rep::EvidenceKind::Capacity), rep::domains::kCapacity);
  REP_CHECK_EQ(rep::evidence_payload_domain(rep::EvidenceKind::PlacementPolicy),
               rep::domains::kPlacementPolicy);
  REP_CHECK_EQ(rep::evidence_payload_domain(rep::EvidenceKind::Maintenance),
               rep::domains::kMaintenance);
  REP_CHECK_EQ(rep::evidence_payload_domain(rep::EvidenceKind::FailureDomain),
               rep::domains::kFailureDomain);
  REP_CHECK_EQ(rep::evidence_payload_domain(rep::EvidenceKind::AsiWorkloadState),
               rep::domains::kAsiWorkload);
  REP_CHECK_EQ(rep::evidence_payload_domain(rep::EvidenceKind::DfiObligation),
               rep::domains::kDfiObligation);
  REP_CHECK_EQ(rep::evidence_payload_domain(rep::EvidenceKind::CandidateOffers),
               rep::domains::kCandidateOffers);

  // No two kinds share a domain, so a digest can never be replayed from one
  // stream kind into another.
  const std::vector<KindPayload> table = kind_payload_table();
  REP_REQUIRE(table.size() == 10);
  for (std::size_t i = 0; i < table.size(); ++i) {
    for (std::size_t j = i + 1; j < table.size(); ++j) {
      REP_CHECK_MSG(rep::evidence_payload_domain(table[i].kind) !=
                        rep::evidence_payload_domain(table[j].kind),
                    "two evidence kinds share a payload domain");
    }
  }

  // The payload digest is the digest of the payload's encoding under that
  // kind's domain, for every kind.
  for (const KindPayload& entry : table) {
    REP_CHECK_EQ(rep::evidence_payload_digest(entry.kind, entry.payload),
                 payload_digest(entry.kind, entry.payload));
    REP_CHECK(!rep::evidence_payload_digest(entry.kind, entry.payload).is_zero());
  }
}

REP_TEST(Evidence, PayloadDigestChangesWithThePayload) {
  const ChangeCase cases[] = {
      {rep::EvidenceKind::RackComposition, composition_payload(1), composition_payload(2)},
      {rep::EvidenceKind::Enumeration, enumeration_payload(true), enumeration_payload(false)},
      {rep::EvidenceKind::ObligationCatalog, catalog_payload(false), catalog_payload(true)},
      {rep::EvidenceKind::Capacity, capacity_payload(false), capacity_payload(true)},
      {rep::EvidenceKind::PlacementPolicy, policy_payload(false), policy_payload(true)},
      {rep::EvidenceKind::Maintenance, maintenance_payload(false), maintenance_payload(true)},
      {rep::EvidenceKind::FailureDomain, failure_domain_payload(false),
       failure_domain_payload(true)},
      {rep::EvidenceKind::AsiWorkloadState, asi_payload(false), asi_payload(true)},
      {rep::EvidenceKind::DfiObligation, dfi_payload(false), dfi_payload(true)},
      {rep::EvidenceKind::CandidateOffers, offers_payload(false), offers_payload(true)},
  };
  for (const ChangeCase& entry : cases) {
    const std::string what = std::string("kind ") + std::string(rep::to_string(entry.kind));
    REP_CHECK_MSG(rep::evidence_payload_digest(entry.kind, entry.before) !=
                      rep::evidence_payload_digest(entry.kind, entry.after),
                  what);
  }

  // The same payload under two different kinds never produces the same
  // digest, even though the payload bytes are identical.
  const rep::EvidencePayload shared = composition_payload(1);
  REP_CHECK_NE(rep::evidence_payload_digest(rep::EvidenceKind::RackComposition, shared),
               rep::evidence_payload_digest(rep::EvidenceKind::Capacity, shared));

  // An out of range kind falls back to the envelope domain rather than
  // silently reusing a real kind's tag.
  const auto unknown_kind = static_cast<rep::EvidenceKind>(60000);
  REP_CHECK_EQ(rep::evidence_payload_domain(unknown_kind), rep::domains::kEvidenceEnvelope);
  REP_CHECK_EQ(rep::evidence_payload_digest(unknown_kind, shared),
               payload_digest(unknown_kind, shared));
}

REP_TEST(Evidence, KindMustMatchPayload) {
  const std::vector<KindPayload> table = kind_payload_table();
  REP_REQUIRE(table.size() == 10);
  // Every kind carrying the next kind's payload is refused before anything
  // else is examined.
  for (std::size_t i = 0; i < table.size(); ++i) {
    const KindPayload& entry = table[i];
    const KindPayload& other = table[(i + 1) % table.size()];
    rep::EvidenceRecord record;
    record.kind = entry.kind;
    record.stamp.source = kAuthorityA;
    record.stamp.generation = rep::Generation(1);
    record.stamp.epoch = rep::Epoch(1);
    // A digest that is correct for the payload under the record's own kind:
    // only the kind/payload agreement can reject this record.
    record.stamp.content_digest = rep::evidence_payload_digest(entry.kind, other.payload);
    record.payload = other.payload;

    const auto refused = rep::EvidenceBundle::make({record});
    REP_REQUIRE(!refused.ok());
    REP_CHECK_EQ(refused.error().code, rep::ErrorCode::InvalidArgument);
    REP_CHECK_EQ(refused.error().field, std::string("evidence"));
    REP_CHECK_MSG(refused.error().message.find("does not match its payload") != std::string::npos,
                  refused.error().message);
  }

  // The concrete example from the contract: Capacity carrying a rack
  // composition.
  rep::EvidenceRecord wrong;
  wrong.kind = rep::EvidenceKind::Capacity;
  wrong.stamp.source = kAuthorityA;
  wrong.stamp.content_digest = rep::evidence_payload_digest(rep::EvidenceKind::Capacity,
                                                            composition_payload(1));
  wrong.payload = composition_payload(1);
  const auto refused = rep::EvidenceBundle::make({wrong});
  REP_REQUIRE(!refused.ok());
  REP_CHECK_EQ(refused.error().code, rep::ErrorCode::InvalidArgument);
  REP_CHECK_MSG(refused.error().message.find("capacity") != std::string::npos,
                refused.error().message);

  // An unmapped kind matches no payload at all.
  rep::EvidenceRecord unmapped;
  unmapped.kind = static_cast<rep::EvidenceKind>(60000);
  unmapped.stamp.source = kAuthorityA;
  unmapped.payload = composition_payload(1);
  const auto unmapped_refused = rep::EvidenceBundle::make({unmapped});
  REP_REQUIRE(!unmapped_refused.ok());
  REP_CHECK_EQ(unmapped_refused.error().code, rep::ErrorCode::InvalidArgument);
}

REP_TEST(Evidence, PayloadDigestIsEnforced) {
  const rep::EvidencePayload payload = enumeration_payload(true);
  const rep::EvidenceRecord good = record_of(rep::EvidenceKind::Enumeration, kAuthorityA, payload);

  const auto accepted = rep::EvidenceBundle::make({good});
  REP_REQUIRE(accepted.ok());
  REP_CHECK_EQ(accepted.value().size(), std::size_t{1});
  REP_CHECK_EQ(accepted.value().records()[0].stamp.content_digest,
               rep::evidence_payload_digest(rep::EvidenceKind::Enumeration, payload));

  // A record whose asserted digest is not the payload's digest is refused.
  rep::EvidenceRecord flipped = good;
  flipped.stamp.content_digest = rep::sha256_of("not the payload");
  REP_CHECK_NE(flipped.stamp.content_digest,
               rep::evidence_payload_digest(flipped.kind, flipped.payload));
  const auto refused = rep::EvidenceBundle::make({flipped});
  REP_REQUIRE(!refused.ok());
  REP_CHECK_EQ(refused.error().code, rep::ErrorCode::DigestMismatch);
  REP_CHECK_EQ(refused.error().field, std::string("evidence"));
  REP_CHECK_MSG(refused.error().message.find("a-authority/composition") != std::string::npos,
                refused.error().message);

  // One changed byte inside the payload, with the stamp left alone, is
  // detected: the record no longer describes the bytes it claims.
  rep::EvidenceRecord mutated = good;
  auto* enumeration = std::get_if<rep::EnumerationPayload>(&mutated.payload);
  REP_REQUIRE(enumeration != nullptr);
  enumeration->rack = rep::RackId("rack-b");
  const auto detected = rep::EvidenceBundle::make({mutated});
  REP_REQUIRE(!detected.ok());
  REP_CHECK_EQ(detected.error().code, rep::ErrorCode::DigestMismatch);

  // Flipping the completeness flag is also a different payload.
  rep::EvidenceRecord flipped_flag = good;
  auto* flag = std::get_if<rep::EnumerationPayload>(&flipped_flag.payload);
  REP_REQUIRE(flag != nullptr);
  flag->complete = false;
  const auto flag_detected = rep::EvidenceBundle::make({flipped_flag});
  REP_REQUIRE(!flag_detected.ok());
  REP_CHECK_EQ(flag_detected.error().code, rep::ErrorCode::DigestMismatch);

  // Recomputing the stamp after the change makes the record valid again, which
  // shows the mismatch came from the digest and not from the payload content.
  rep::EvidenceRecord repaired = mutated;
  repaired.stamp.content_digest = rep::evidence_payload_digest(repaired.kind, repaired.payload);
  REP_CHECK_NE(repaired.stamp.content_digest, good.stamp.content_digest);
  const auto repaired_bundle = rep::EvidenceBundle::make({repaired});
  REP_REQUIRE(repaired_bundle.ok());
  REP_CHECK_EQ(repaired_bundle.value().size(), std::size_t{1});
}

REP_TEST(Evidence, CandidateDeclarationDigestIsEnforced) {
  const rep::CandidateOffersPayload offers = offers_payload(true);
  const rep::EvidenceRecord good =
      record_of(rep::EvidenceKind::CandidateOffers, kAuthorityA, offers);
  const auto accepted = rep::EvidenceBundle::make({good});
  REP_REQUIRE(accepted.ok());

  // The candidate's own digest matches the content it declares.
  REP_REQUIRE(offers.candidates.size() == 1);
  REP_CHECK_EQ(offers.candidates[0].evidence_digest, offers.candidates[0].content_digest());

  // Now alter only the candidate's declared digest and repair the payload
  // digest, so the per-candidate check is the only thing that can refuse it.
  rep::CandidateOffersPayload tampered = offers;
  tampered.candidates[0].evidence_digest = rep::sha256_of("not the candidate content");
  REP_CHECK_NE(tampered.candidates[0].evidence_digest,
               tampered.candidates[0].content_digest());
  const rep::EvidenceRecord tampered_record =
      record_of(rep::EvidenceKind::CandidateOffers, kAuthorityA, tampered);
  REP_REQUIRE(tampered_record.stamp.content_digest ==
              rep::evidence_payload_digest(rep::EvidenceKind::CandidateOffers, tampered));

  const auto refused = rep::EvidenceBundle::make({tampered_record});
  REP_REQUIRE(!refused.ok());
  REP_CHECK_EQ(refused.error().code, rep::ErrorCode::DigestMismatch);
  REP_CHECK_EQ(refused.error().field, std::string("candidate"));
  REP_CHECK_MSG(refused.error().message.find("cand-1") != std::string::npos,
                refused.error().message);
  REP_CHECK_MSG(refused.error().message.find("declaration") != std::string::npos,
                refused.error().message);

  // Changing a content field of the candidate while keeping the digest is
  // equally detected.
  rep::CandidateOffersPayload content_changed = offers;
  content_changed.candidates[0].estimated_cost += 1;
  REP_CHECK_NE(content_changed.candidates[0].evidence_digest,
               content_changed.candidates[0].content_digest());
  const auto content_refused = rep::EvidenceBundle::make(
      {record_of(rep::EvidenceKind::CandidateOffers, kAuthorityA, content_changed)});
  REP_REQUIRE(!content_refused.ok());
  REP_CHECK_EQ(content_refused.error().code, rep::ErrorCode::DigestMismatch);
  REP_CHECK_EQ(content_refused.error().field, std::string("candidate"));

  // A second, well formed candidate in the same offer set is accepted, so the
  // check is per record rather than a blanket refusal.
  std::vector<rep::Candidate> two = offers.candidates;
  two.push_back(candidate("cand-2"));
  const auto both = rep::CandidateOffersPayload::make(std::move(two));
  REP_REQUIRE(both.ok());
  const auto both_accepted = rep::EvidenceBundle::make(
      {record_of(rep::EvidenceKind::CandidateOffers, kAuthorityA, both.value())});
  REP_REQUIRE(both_accepted.ok());
}

REP_TEST(Evidence, BundleIsSortedByKindAndSource) {
  const rep::EvidenceRecord capacity_record =
      record_of(rep::EvidenceKind::Capacity, kAuthorityA, capacity_payload(true), 1);
  const rep::EvidenceRecord a_composition =
      record_of(rep::EvidenceKind::RackComposition, kAuthorityB, composition_payload(1), 2);
  const rep::EvidenceRecord another_stream =
      record_of(rep::EvidenceKind::RackComposition, stream("a-authority", "enumeration"),
                composition_payload(1), 3);
  const rep::EvidenceRecord first_composition =
      record_of(rep::EvidenceKind::RackComposition, kAuthorityA, composition_payload(1), 4);

  const auto bundle = rep::EvidenceBundle::make(
      {capacity_record, a_composition, another_stream, first_composition});
  REP_REQUIRE(bundle.ok());
  REP_REQUIRE(bundle.value().size() == 4);
  // Kind code first, then the source: a-authority/composition,
  // a-authority/enumeration, b-authority/composition, then capacity.
  REP_CHECK(bundle.value().records()[0].stamp.source == kAuthorityA);
  REP_CHECK_EQ(bundle.value().records()[0].stamp.generation, rep::Generation(4));
  REP_CHECK_EQ(bundle.value().records()[1].stamp.source.stream.str(), std::string("enumeration"));
  REP_CHECK_EQ(bundle.value().records()[1].stamp.generation, rep::Generation(3));
  REP_CHECK(bundle.value().records()[2].stamp.source == kAuthorityB);
  REP_CHECK_EQ(bundle.value().records()[2].stamp.generation, rep::Generation(2));
  REP_CHECK(bundle.value().records()[3].kind == rep::EvidenceKind::Capacity);
  REP_CHECK_EQ(bundle.value().records()[3].stamp.generation, rep::Generation(1));

  // The input order does not matter: the same records in any order make the
  // same bundle, and therefore the same digest.
  const auto shuffled = rep::EvidenceBundle::make(
      {first_composition, capacity_record, another_stream, a_composition});
  REP_REQUIRE(shuffled.ok());
  REP_CHECK(shuffled.value() == bundle.value());
  REP_CHECK_EQ(rep::digest_of(rep::domains::kEvidenceEnvelope, shuffled.value()),
               rep::digest_of(rep::domains::kEvidenceEnvelope, bundle.value()));

  // The bundle encodes as a sequence: a u64 count followed by the records.
  rep::CanonicalWriter writer(rep::domains::kEvidenceEnvelope);
  bundle.value().encode(writer);
  REP_CHECK_EQ(writer.to_hex().substr(0, 16), std::string("0400000000000000"));

  const auto empty = rep::EvidenceBundle::make({});
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().empty());
  REP_CHECK_EQ(empty.value().size(), std::size_t{0});
  REP_CHECK(empty.value().records().empty());
  REP_CHECK_NE(rep::digest_of(rep::domains::kEvidenceEnvelope, empty.value()),
               rep::digest_of(rep::domains::kEvidenceEnvelope, bundle.value()));
}

REP_TEST(Evidence, BundleRefusesDuplicateKindAndSource) {
  const rep::EvidenceRecord first =
      record_of(rep::EvidenceKind::RackComposition, kAuthorityA, composition_payload(1));
  // Same kind, same source, different payload: a contradiction, not two
  // versions.
  const rep::EvidenceRecord second =
      record_of(rep::EvidenceKind::RackComposition, kAuthorityA, composition_payload(2));
  REP_CHECK(first.payload != second.payload);

  const auto refused = rep::EvidenceBundle::make({first, second});
  REP_REQUIRE(!refused.ok());
  REP_CHECK_EQ(refused.error().code, rep::ErrorCode::AlreadyExists);
  REP_CHECK_EQ(refused.error().field, std::string("evidence"));
  REP_CHECK_MSG(refused.error().message.find("a-authority/composition") != std::string::npos,
                refused.error().message);

  // The same payload twice is also a repeated stream.
  const auto same_twice = rep::EvidenceBundle::make({first, first});
  REP_REQUIRE(!same_twice.ok());
  REP_CHECK_EQ(same_twice.error().code, rep::ErrorCode::AlreadyExists);

  // The same source under two different kinds is not a duplicate.
  const rep::EvidenceRecord other_kind =
      record_of(rep::EvidenceKind::Enumeration, kAuthorityA, enumeration_payload(true));
  const auto both = rep::EvidenceBundle::make({first, other_kind});
  REP_REQUIRE(both.ok());
  REP_CHECK_EQ(both.value().size(), std::size_t{2});

  // Nor is the same kind from two different authorities.
  const rep::EvidenceRecord other_authority =
      record_of(rep::EvidenceKind::RackComposition, kAuthorityB, composition_payload(1));
  const auto two_authorities = rep::EvidenceBundle::make({first, other_authority});
  REP_REQUIRE(two_authorities.ok());
  REP_CHECK_EQ(two_authorities.value().size(), std::size_t{2});
}

REP_TEST(Evidence, BundleFindAndFindAll) {
  const rep::EvidenceRecord first =
      record_of(rep::EvidenceKind::RackComposition, kAuthorityA, composition_payload(1), 11);
  const rep::EvidenceRecord second =
      record_of(rep::EvidenceKind::RackComposition, stream("a-authority", "enumeration"),
                composition_payload(1), 12);
  const rep::EvidenceRecord third =
      record_of(rep::EvidenceKind::Enumeration, kAuthorityB, enumeration_payload(true), 13);
  const rep::EvidenceRecord fourth =
      record_of(rep::EvidenceKind::Capacity, kAuthorityA, capacity_payload(true), 14);

  const auto bundle = rep::EvidenceBundle::make({third, first, fourth, second});
  REP_REQUIRE(bundle.ok());

  const rep::EvidenceRecord* found =
      bundle.value().find(rep::EvidenceKind::RackComposition, kAuthorityA);
  REP_REQUIRE(found != nullptr);
  REP_CHECK_EQ(found->stamp.generation, rep::Generation(11));
  REP_CHECK(found->kind == rep::EvidenceKind::RackComposition);

  // Present kind, absent source.
  REP_CHECK(bundle.value().find(rep::EvidenceKind::RackComposition,
                                stream("a-authority", "missing")) == nullptr);
  // Present source, absent kind.
  REP_CHECK(bundle.value().find(rep::EvidenceKind::DfiObligation, kAuthorityA) == nullptr);
  // A source that matches a different kind's record is still absent.
  REP_CHECK(bundle.value().find(rep::EvidenceKind::Capacity, kAuthorityB) == nullptr);
  REP_CHECK(bundle.value().find(rep::EvidenceKind::Maintenance, kAuthorityA) == nullptr);

  const std::vector<const rep::EvidenceRecord*> compositions =
      bundle.value().find_all(rep::EvidenceKind::RackComposition);
  REP_REQUIRE(compositions.size() == 2);
  REP_CHECK_EQ(compositions[0]->stamp.generation, rep::Generation(11));
  REP_CHECK_EQ(compositions[1]->stamp.generation, rep::Generation(12));

  const std::vector<const rep::EvidenceRecord*> capacities =
      bundle.value().find_all(rep::EvidenceKind::Capacity);
  REP_REQUIRE(capacities.size() == 1);
  REP_CHECK_EQ(capacities[0]->stamp.generation, rep::Generation(14));

  REP_CHECK(bundle.value().find_all(rep::EvidenceKind::Maintenance).empty());
  REP_CHECK(bundle.value().find_all(rep::EvidenceKind::DfiObligation).empty());

  const auto empty = rep::EvidenceBundle::make({});
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().find(rep::EvidenceKind::Capacity, kAuthorityA) == nullptr);
  REP_CHECK(empty.value().find_all(rep::EvidenceKind::Capacity).empty());
}

REP_TEST(Evidence, BundleRecordLimit) {
  const rep::EvidenceRecord record =
      record_of(rep::EvidenceKind::RackComposition, kAuthorityA, composition_payload(1));

  // One record over the limit is refused, and the limit is checked before any
  // other validation.
  const auto over = rep::EvidenceBundle::make({record, record, record}, 2);
  REP_REQUIRE(!over.ok());
  REP_CHECK_EQ(over.error().code, rep::ErrorCode::LimitExceeded);
  REP_CHECK_EQ(over.error().field, std::string("evidence"));

  // At the limit, the duplicate records are what refuse the bundle, which
  // shows the limit itself was satisfied.
  const auto at_limit = rep::EvidenceBundle::make({record, record, record}, 3);
  REP_REQUIRE(!at_limit.ok());
  REP_CHECK_EQ(at_limit.error().code, rep::ErrorCode::AlreadyExists);

  // An empty bundle is within a zero limit.
  const auto empty = rep::EvidenceBundle::make({}, 0);
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().empty());

  // The default limit is 4096 records: one more is refused by the limit, and
  // raising the limit lets the same input reach the duplicate check instead.
  std::vector<rep::EvidenceRecord> many(4097, record);
  const auto default_limit = rep::EvidenceBundle::make(many);
  REP_REQUIRE(!default_limit.ok());
  REP_CHECK_EQ(default_limit.error().code, rep::ErrorCode::LimitExceeded);
  REP_CHECK_MSG(default_limit.error().message.find("4096") != std::string::npos,
                default_limit.error().message);

  const auto raised_limit = rep::EvidenceBundle::make(many, 4097);
  REP_REQUIRE(!raised_limit.ok());
  REP_CHECK_EQ(raised_limit.error().code, rep::ErrorCode::AlreadyExists);

  // 4096 distinct streams fit exactly.
  std::vector<rep::EvidenceRecord> distinct;
  distinct.reserve(4096);
  for (std::uint64_t index = 0; index < 4096; ++index) {
    rep::EvidenceRecord item = record;
    item.stamp.source.stream = rep::StreamId("stream-" + std::to_string(index));
    item.stamp.generation = rep::Generation(index);
    distinct.push_back(item);
  }
  const auto full = rep::EvidenceBundle::make(distinct);
  REP_REQUIRE(full.ok());
  REP_CHECK_EQ(full.value().size(), std::size_t{4096});
  REP_CHECK(full.value().find(rep::EvidenceKind::RackComposition,
                              stream("a-authority", "stream-4095")) != nullptr);
  REP_CHECK(full.value().find(rep::EvidenceKind::RackComposition,
                              stream("a-authority", "stream-4096")) == nullptr);
}

REP_TEST(Evidence, StampEqualityCoversEveryField) {
  const rep::EvidenceStamp stamp{kAuthorityA, rep::Generation(3), rep::Epoch(2),
                                 rep::sha256_of("payload")};
  const rep::EvidenceStamp same{kAuthorityA, rep::Generation(3), rep::Epoch(2),
                                rep::sha256_of("payload")};
  REP_CHECK(stamp == same);
  REP_CHECK(!(stamp != same));

  rep::EvidenceStamp other = stamp;
  other.generation = rep::Generation(4);
  REP_CHECK(stamp != other);
  REP_CHECK_EQ(stamp.content_digest, other.content_digest);

  other = stamp;
  other.epoch = rep::Epoch(3);
  REP_CHECK(stamp != other);
  REP_CHECK_EQ(stamp.generation, other.generation);

  other = stamp;
  other.content_digest = rep::sha256_of("another payload");
  REP_CHECK(stamp != other);
  REP_CHECK_EQ(stamp.epoch, other.epoch);

  other = stamp;
  other.source.stream = rep::StreamId("enumeration");
  REP_CHECK(stamp != other);

  other = stamp;
  other.source.authority = rep::AuthorityId("b-authority");
  REP_CHECK(stamp != other);

  // The stamp sorts by its fields in declaration order, so a bundle of records
  // that differ only in generation still orders deterministically.
  const rep::EvidenceStamp later{kAuthorityA, rep::Generation(9), rep::Epoch(2),
                                 rep::sha256_of("payload")};
  REP_CHECK(stamp != later);
  const rep::EvidenceSource capacity_a = source(rep::EvidenceKind::Capacity, kAuthorityA);
  REP_CHECK(capacity_a == source(rep::EvidenceKind::Capacity, kAuthorityA));
  REP_CHECK(capacity_a != source(rep::EvidenceKind::Maintenance, kAuthorityA));
  REP_CHECK(capacity_a != source(rep::EvidenceKind::Capacity, kAuthorityB));
  REP_CHECK(capacity_a < source(rep::EvidenceKind::Maintenance, kAuthorityA));
  REP_CHECK(capacity_a < source(rep::EvidenceKind::Capacity, kAuthorityB));
}

REP_TEST(Evidence, RecordEncodingCoversKindStampAndPayload) {
  const rep::EvidenceRecord record =
      record_of(rep::EvidenceKind::Enumeration, kAuthorityA, enumeration_payload(true), 5, 6);

  rep::CanonicalWriter writer(rep::domains::kEvidenceEnvelope);
  record.encode(writer);
  // u16 kind, the stream reference, generation, epoch, the payload digest,
  // then the payload itself.
  const std::string hex = writer.to_hex();
  REP_CHECK_EQ(hex.substr(0, 4), std::string("0200"));
  REP_CHECK_MSG(hex.find(record.stamp.content_digest.to_hex()) != std::string::npos, hex);

  // Every stamp field is inside the encoding: changing the generation, the
  // epoch, the source, or the payload digest changes the bytes.
  rep::EvidenceRecord changed = record;
  changed.stamp.generation = rep::Generation(7);
  rep::CanonicalWriter generation_writer(rep::domains::kEvidenceEnvelope);
  changed.encode(generation_writer);
  REP_CHECK_NE(generation_writer.to_hex(), hex);

  changed = record;
  changed.stamp.epoch = rep::Epoch(9);
  rep::CanonicalWriter epoch_writer(rep::domains::kEvidenceEnvelope);
  changed.encode(epoch_writer);
  REP_CHECK_NE(epoch_writer.to_hex(), hex);

  changed = record;
  changed.stamp.source.authority = rep::AuthorityId("b-authority");
  rep::CanonicalWriter source_writer(rep::domains::kEvidenceEnvelope);
  changed.encode(source_writer);
  REP_CHECK_NE(source_writer.to_hex(), hex);

  changed = record;
  changed.payload = enumeration_payload(false);
  rep::CanonicalWriter payload_writer(rep::domains::kEvidenceEnvelope);
  changed.encode(payload_writer);
  REP_CHECK_NE(payload_writer.to_hex(), hex);

  // A record with the same content encodes identically.
  const rep::EvidenceRecord same =
      record_of(rep::EvidenceKind::Enumeration, kAuthorityA, enumeration_payload(true), 5, 6);
  REP_CHECK(same == record);
  rep::CanonicalWriter same_writer(rep::domains::kEvidenceEnvelope);
  same.encode(same_writer);
  REP_CHECK_EQ(same_writer.to_hex(), hex);
}

REP_TEST_MAIN()
