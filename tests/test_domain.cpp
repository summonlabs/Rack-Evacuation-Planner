// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Tests for the obligation definitions and for every evidence payload type
// that carries domain state: canonical ordering, refusal of contradictory
// input, keyed lookups, and the static action/destination compatibility
// tables.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rep/adjacent.hpp"
#include "rep/candidate.hpp"
#include "rep/canonical.hpp"
#include "rep/digest.hpp"
#include "rep/obligation.hpp"
#include "rep/policy.hpp"
#include "rep/types.hpp"

#include "testkit.hpp"

namespace {

[[nodiscard]] rep::Result<rep::Obligation> make_obligation(
    std::string_view id, rep::ObligationKind kind = rep::ObligationKind::Workload,
    std::string_view rack = "rack-a", bool protected_obligation = false,
    std::vector<rep::ObligationId> depends_on = {}) {
  return rep::Obligation::make(rep::ObligationId(std::string(id)), kind,
                               rep::RackId(std::string(rack)), rep::ResourceVector(),
                               protected_obligation, std::move(depends_on));
}

[[nodiscard]] rep::DestinationRef destination(rep::DestinationKind kind, std::string_view id) {
  return rep::DestinationRef{kind, rep::DestinationId(std::string(id))};
}

[[nodiscard]] rep::MaintenanceWindow window(rep::UnixNanos starts, rep::UnixNanos ends) {
  rep::MaintenanceWindow result;
  result.starts_at = starts;
  result.ends_at = ends;
  return result;
}

[[nodiscard]] rep::Candidate make_candidate(std::string_view id, std::string_view obligation,
                                            rep::ActionKind action,
                                            const rep::DestinationRef& target) {
  const auto candidate = rep::Candidate::make(
      rep::CandidateId(std::string(id)), rep::ObligationId(std::string(obligation)), action,
      target, rep::AuthorityId("authority-1"), rep::Generation(3), rep::Epoch(2),
      rep::ResourceVector(), 7, false);
  REP_REQUIRE(candidate.ok());
  return candidate.value();
}

constexpr rep::ObligationKind kObligations[] = {
    rep::ObligationKind::Workload,        rep::ObligationKind::StorageReplica,
    rep::ObligationKind::NetworkPath,     rep::ObligationKind::ServiceEndpoint,
    rep::ObligationKind::Reservation,     rep::ObligationKind::Appliance};

constexpr rep::ActionKind kActions[] = {
    rep::ActionKind::LiveMigrate, rep::ActionKind::ColdMigrate, rep::ActionKind::Quiesce,
    rep::ActionKind::Detach,      rep::ActionKind::Rebind,      rep::ActionKind::Release,
    rep::ActionKind::Depower};

constexpr rep::DestinationKind kDestinations[] = {
    rep::DestinationKind::RackSlot, rep::DestinationKind::FabricEndpoint,
    rep::DestinationKind::StorageTarget, rep::DestinationKind::ServiceEndpoint};

}  // namespace

REP_TEST(Domain, ObligationMakeRejectsInvalidInputs) {
  const auto empty_id = rep::Obligation::make(rep::ObligationId(), rep::ObligationKind::Workload,
                                              rep::RackId("rack-a"), rep::ResourceVector(), false, {});
  REP_REQUIRE(!empty_id.ok());
  REP_CHECK_EQ(empty_id.error().code, rep::ErrorCode::InvalidIdentifier);

  const auto empty_rack =
      rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::Workload,
                            rep::RackId(), rep::ResourceVector(), false, {});
  REP_REQUIRE(!empty_rack.ok());
  REP_CHECK_EQ(empty_rack.error().code, rep::ErrorCode::InvalidIdentifier);
  REP_CHECK_EQ(empty_rack.error().field, std::string("rack"));

  const auto self_dependency =
      rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::Workload,
                            rep::RackId("rack-a"), rep::ResourceVector(), false,
                            {rep::ObligationId("ob-1")});
  REP_REQUIRE(!self_dependency.ok());
  REP_CHECK_EQ(self_dependency.error().code, rep::ErrorCode::InvalidArgument);
  REP_CHECK_EQ(self_dependency.error().field, std::string("depends_on"));

  // A repeated dependency is a contradiction, not a repetition.
  const auto duplicate_dependency =
      rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::Workload,
                            rep::RackId("rack-a"), rep::ResourceVector(), false,
                            {rep::ObligationId("ob-0"), rep::ObligationId("ob-0")});
  REP_REQUIRE(!duplicate_dependency.ok());
  REP_CHECK_EQ(duplicate_dependency.error().code, rep::ErrorCode::DuplicateIdentity);

  // A dependency that appears once is accepted, including one that sorts
  // before and after the obligation's own id.
  const auto ok = make_obligation("ob-1", rep::ObligationKind::Workload, "rack-a", false,
                                  {rep::ObligationId("ob-0")});
  REP_REQUIRE(ok.ok());
  REP_CHECK_EQ(ok.value().depends_on.size(), std::size_t{1});
}

REP_TEST(Domain, ObligationDependenciesAreCanonical) {
  const auto obligation = make_obligation("ob-m", rep::ObligationKind::Workload, "rack-a", false,
                                          {rep::ObligationId("ob-z"), rep::ObligationId("ob-a"),
                                           rep::ObligationId("ob-b")});
  REP_REQUIRE(obligation.ok());
  const std::vector<rep::ObligationId>& dependencies = obligation.value().depends_on;
  REP_REQUIRE(dependencies.size() == 3);
  REP_CHECK_EQ(dependencies[0].str(), std::string("ob-a"));
  REP_CHECK_EQ(dependencies[1].str(), std::string("ob-b"));
  REP_CHECK_EQ(dependencies[2].str(), std::string("ob-z"));

  // The same set given in a different order produces the same bytes and the
  // same definition digest.
  const auto shuffled = make_obligation("ob-m", rep::ObligationKind::Workload, "rack-a", false,
                                        {rep::ObligationId("ob-b"), rep::ObligationId("ob-z"),
                                         rep::ObligationId("ob-a")});
  REP_REQUIRE(shuffled.ok());
  REP_CHECK(obligation.value() == shuffled.value());
  REP_CHECK_EQ(obligation.value().definition_digest(), shuffled.value().definition_digest());

  // A one element list and an empty list are both canonical.
  const auto single = make_obligation("ob-m", rep::ObligationKind::Workload, "rack-a", false,
                                      {rep::ObligationId("ob-a")});
  REP_REQUIRE(single.ok());
  REP_CHECK_EQ(single.value().depends_on.size(), std::size_t{1});
  const auto none = make_obligation("ob-m");
  REP_REQUIRE(none.ok());
  REP_CHECK(none.value().depends_on.empty());
  REP_CHECK_NE(single.value().definition_digest(), none.value().definition_digest());
}

REP_TEST(Domain, ObligationDefinitionDigestCoversEveryField) {
  const auto demand = rep::ResourceVector::make({{rep::ResourceClass::CpuMillicores, 4}});
  REP_REQUIRE(demand.ok());
  const auto base = rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::Workload,
                                          rep::RackId("rack-a"), demand.value(), false,
                                          {rep::ObligationId("ob-0")});
  REP_REQUIRE(base.ok());
  const rep::Digest base_digest = base.value().definition_digest();
  REP_CHECK(!base_digest.is_zero());

  const auto other_id = rep::Obligation::make(rep::ObligationId("ob-2"), rep::ObligationKind::Workload,
                                              rep::RackId("rack-a"), demand.value(), false,
                                              {rep::ObligationId("ob-0")});
  REP_REQUIRE(other_id.ok());
  REP_CHECK_NE(other_id.value().definition_digest(), base_digest);

  const auto other_kind =
      rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::StorageReplica,
                            rep::RackId("rack-a"), demand.value(), false,
                            {rep::ObligationId("ob-0")});
  REP_REQUIRE(other_kind.ok());
  REP_CHECK_NE(other_kind.value().definition_digest(), base_digest);

  const auto other_rack = rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::Workload,
                                                rep::RackId("rack-b"), demand.value(), false,
                                                {rep::ObligationId("ob-0")});
  REP_REQUIRE(other_rack.ok());
  REP_CHECK_NE(other_rack.value().definition_digest(), base_digest);

  const auto other_demand = rep::ResourceVector::make({{rep::ResourceClass::CpuMillicores, 5}});
  REP_REQUIRE(other_demand.ok());
  const auto demand_changed =
      rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::Workload,
                            rep::RackId("rack-a"), other_demand.value(), false,
                            {rep::ObligationId("ob-0")});
  REP_REQUIRE(demand_changed.ok());
  REP_CHECK_NE(demand_changed.value().definition_digest(), base_digest);

  const auto protected_changed =
      rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::Workload,
                            rep::RackId("rack-a"), demand.value(), true,
                            {rep::ObligationId("ob-0")});
  REP_REQUIRE(protected_changed.ok());
  REP_CHECK_NE(protected_changed.value().definition_digest(), base_digest);

  const auto dependency_changed =
      rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::Workload,
                            rep::RackId("rack-a"), demand.value(), false,
                            {rep::ObligationId("ob-9")});
  REP_REQUIRE(dependency_changed.ok());
  REP_CHECK_NE(dependency_changed.value().definition_digest(), base_digest);

  const auto dependency_removed =
      rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::Workload,
                            rep::RackId("rack-a"), demand.value(), false, {});
  REP_REQUIRE(dependency_removed.ok());
  REP_CHECK_NE(dependency_removed.value().definition_digest(), base_digest);

  // An unrelated domain produces a different digest for the same definition.
  const rep::Digest under_other_tag = rep::digest_of("rep.other.v1", base.value());
  REP_CHECK_NE(under_other_tag, base_digest);
}

REP_TEST(Domain, ObligationCatalogCanonicalAndFind) {
  const auto second = make_obligation("ob-2");
  const auto first = make_obligation("ob-1", rep::ObligationKind::Reservation, "rack-b", true);
  REP_REQUIRE(second.ok());
  REP_REQUIRE(first.ok());

  const auto catalog = rep::ObligationCatalogPayload::make({second.value(), first.value()});
  REP_REQUIRE(catalog.ok());
  REP_REQUIRE(catalog.value().obligations.size() == 2);
  REP_CHECK_EQ(catalog.value().obligations[0].id.str(), std::string("ob-1"));
  REP_CHECK_EQ(catalog.value().obligations[1].id.str(), std::string("ob-2"));

  // The order the definitions arrived in does not change the payload.
  const auto reordered = rep::ObligationCatalogPayload::make({first.value(), second.value()});
  REP_REQUIRE(reordered.ok());
  REP_CHECK(reordered.value() == catalog.value());
  REP_CHECK_EQ(rep::digest_of(rep::domains::kObligationCatalog, reordered.value()),
               rep::digest_of(rep::domains::kObligationCatalog, catalog.value()));

  // Two definitions of the same id are refused even when they differ: a
  // repeated id is a contradiction about what the obligation is.
  const auto conflicting =
      make_obligation("ob-1", rep::ObligationKind::Appliance, "rack-c", true);
  REP_REQUIRE(conflicting.ok());
  REP_CHECK(conflicting.value() != first.value());
  const auto duplicate =
      rep::ObligationCatalogPayload::make({first.value(), conflicting.value()});
  REP_REQUIRE(!duplicate.ok());
  REP_CHECK_EQ(duplicate.error().code, rep::ErrorCode::DuplicateIdentity);

  const auto same_twice = rep::ObligationCatalogPayload::make({first.value(), first.value()});
  REP_REQUIRE(!same_twice.ok());
  REP_CHECK_EQ(same_twice.error().code, rep::ErrorCode::DuplicateIdentity);

  // find() answers for present and absent ids.
  const rep::Obligation* found = catalog.value().find(rep::ObligationId("ob-1"));
  REP_REQUIRE(found != nullptr);
  REP_CHECK(found->kind == rep::ObligationKind::Reservation);
  REP_CHECK_EQ(found->source_rack.str(), std::string("rack-b"));
  REP_CHECK(found->protected_obligation);
  REP_CHECK(catalog.value().find(rep::ObligationId("ob-3")) == nullptr);
  REP_CHECK(catalog.value().find(rep::ObligationId()) == nullptr);

  const auto empty = rep::ObligationCatalogPayload::make({});
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().obligations.empty());
  REP_CHECK(empty.value().find(rep::ObligationId("ob-1")) == nullptr);
}

REP_TEST(Domain, RackCompositionCanonicalAndDigest) {
  const auto composition = rep::RackCompositionPayload::make(
      rep::RackId("rack-a"), rep::Generation(7),
      {rep::ObligationId("ob-2"), rep::ObligationId("ob-1")});
  REP_REQUIRE(composition.ok());
  REP_REQUIRE(composition.value().occupants.size() == 2);
  REP_CHECK_EQ(composition.value().occupants[0].str(), std::string("ob-1"));
  REP_CHECK_EQ(composition.value().occupants[1].str(), std::string("ob-2"));
  REP_CHECK_EQ(composition.value().rack.str(), std::string("rack-a"));
  REP_CHECK_EQ(composition.value().composition_revision, rep::Generation(7));

  const auto reordered = rep::RackCompositionPayload::make(
      rep::RackId("rack-a"), rep::Generation(7),
      {rep::ObligationId("ob-1"), rep::ObligationId("ob-2")});
  REP_REQUIRE(reordered.ok());
  REP_CHECK(reordered.value() == composition.value());
  REP_CHECK_EQ(rep::digest_of(rep::domains::kRackComposition, reordered.value()),
               rep::digest_of(rep::domains::kRackComposition, composition.value()));

  // A repeated occupant is refused.
  const auto duplicate = rep::RackCompositionPayload::make(
      rep::RackId("rack-a"), rep::Generation(7),
      {rep::ObligationId("ob-1"), rep::ObligationId("ob-1")});
  REP_REQUIRE(!duplicate.ok());
  REP_CHECK_EQ(duplicate.error().code, rep::ErrorCode::DuplicateIdentity);

  // An empty rack is not a rack.
  const auto no_rack = rep::RackCompositionPayload::make(rep::RackId(), rep::Generation(7), {});
  REP_REQUIRE(!no_rack.ok());
  REP_CHECK_EQ(no_rack.error().code, rep::ErrorCode::InvalidIdentifier);

  // The revision participates in the digest.
  const auto other_revision = rep::RackCompositionPayload::make(
      rep::RackId("rack-a"), rep::Generation(8),
      {rep::ObligationId("ob-1"), rep::ObligationId("ob-2")});
  REP_REQUIRE(other_revision.ok());
  REP_CHECK_NE(rep::digest_of(rep::domains::kRackComposition, other_revision.value()),
               rep::digest_of(rep::domains::kRackComposition, composition.value()));

  // An empty occupant set is a valid, and different, composition.
  const auto empty = rep::RackCompositionPayload::make(rep::RackId("rack-a"), rep::Generation(7), {});
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().occupants.empty());
  REP_CHECK_NE(rep::digest_of(rep::domains::kRackComposition, empty.value()),
               rep::digest_of(rep::domains::kRackComposition, composition.value()));
}

REP_TEST(Domain, EnumerationCanonicalAndDigest) {
  const auto enumeration = rep::EnumerationPayload::make(
      rep::RackId("rack-a"), rep::Generation(7), true,
      {rep::ObligationId("ob-2"), rep::ObligationId("ob-1")});
  REP_REQUIRE(enumeration.ok());
  REP_REQUIRE(enumeration.value().enumerated.size() == 2);
  REP_CHECK_EQ(enumeration.value().enumerated[0].str(), std::string("ob-1"));
  REP_CHECK_EQ(enumeration.value().enumerated[1].str(), std::string("ob-2"));
  REP_CHECK(enumeration.value().complete);

  const rep::Digest base = rep::digest_of(rep::domains::kEnumeration, enumeration.value());

  // Every one of rack, revision, complete, and the enumerated set participates
  // in the digest.
  const auto other_rack = rep::EnumerationPayload::make(
      rep::RackId("rack-b"), rep::Generation(7), true,
      {rep::ObligationId("ob-1"), rep::ObligationId("ob-2")});
  REP_REQUIRE(other_rack.ok());
  REP_CHECK_NE(rep::digest_of(rep::domains::kEnumeration, other_rack.value()), base);

  const auto other_revision = rep::EnumerationPayload::make(
      rep::RackId("rack-a"), rep::Generation(8), true,
      {rep::ObligationId("ob-1"), rep::ObligationId("ob-2")});
  REP_REQUIRE(other_revision.ok());
  REP_CHECK_NE(rep::digest_of(rep::domains::kEnumeration, other_revision.value()), base);

  const auto incomplete = rep::EnumerationPayload::make(
      rep::RackId("rack-a"), rep::Generation(7), false,
      {rep::ObligationId("ob-1"), rep::ObligationId("ob-2")});
  REP_REQUIRE(incomplete.ok());
  REP_CHECK(!incomplete.value().complete);
  REP_CHECK_NE(rep::digest_of(rep::domains::kEnumeration, incomplete.value()), base);

  const auto other_set = rep::EnumerationPayload::make(
      rep::RackId("rack-a"), rep::Generation(7), true,
      {rep::ObligationId("ob-1"), rep::ObligationId("ob-3")});
  REP_REQUIRE(other_set.ok());
  REP_CHECK_NE(rep::digest_of(rep::domains::kEnumeration, other_set.value()), base);

  const auto smaller_set = rep::EnumerationPayload::make(
      rep::RackId("rack-a"), rep::Generation(7), true, {rep::ObligationId("ob-1")});
  REP_REQUIRE(smaller_set.ok());
  REP_CHECK_NE(rep::digest_of(rep::domains::kEnumeration, smaller_set.value()), base);

  // Sorting is canonical and order independent.
  const auto reordered = rep::EnumerationPayload::make(
      rep::RackId("rack-a"), rep::Generation(7), true,
      {rep::ObligationId("ob-1"), rep::ObligationId("ob-2")});
  REP_REQUIRE(reordered.ok());
  REP_CHECK(reordered.value() == enumeration.value());
  REP_CHECK_EQ(rep::digest_of(rep::domains::kEnumeration, reordered.value()), base);

  const auto duplicate = rep::EnumerationPayload::make(
      rep::RackId("rack-a"), rep::Generation(7), true,
      {rep::ObligationId("ob-1"), rep::ObligationId("ob-1")});
  REP_REQUIRE(!duplicate.ok());
  REP_CHECK_EQ(duplicate.error().code, rep::ErrorCode::DuplicateIdentity);

  const auto no_rack = rep::EnumerationPayload::make(rep::RackId(), rep::Generation(7), true, {});
  REP_REQUIRE(!no_rack.ok());
  REP_CHECK_EQ(no_rack.error().code, rep::ErrorCode::InvalidIdentifier);
}

REP_TEST(Domain, CapacityPayloadCanonicalAndFind) {
  rep::DestinationCapacity later;
  later.destination = destination(rep::DestinationKind::FabricEndpoint, "fc-1");
  later.generation = rep::Generation(1);
  later.epoch = rep::Epoch(1);

  rep::DestinationCapacity slot_two;
  slot_two.destination = destination(rep::DestinationKind::RackSlot, "rack-2");
  slot_two.generation = rep::Generation(4);
  slot_two.epoch = rep::Epoch(2);

  rep::DestinationCapacity slot_one;
  slot_one.destination = destination(rep::DestinationKind::RackSlot, "rack-1");
  slot_one.generation = rep::Generation(9);
  slot_one.epoch = rep::Epoch(3);
  const auto available = rep::ResourceVector::make({{rep::ResourceClass::CpuMillicores, 64}});
  REP_REQUIRE(available.ok());
  slot_one.available = available.value();

  const auto payload = rep::CapacityPayload::make({later, slot_two, slot_one});
  REP_REQUIRE(payload.ok());
  REP_REQUIRE(payload.value().destinations.size() == 3);
  // Sorted by destination: kind code first, then the id text.
  REP_CHECK(payload.value().destinations[0].destination ==
            destination(rep::DestinationKind::RackSlot, "rack-1"));
  REP_CHECK(payload.value().destinations[1].destination ==
            destination(rep::DestinationKind::RackSlot, "rack-2"));
  REP_CHECK(payload.value().destinations[2].destination ==
            destination(rep::DestinationKind::FabricEndpoint, "fc-1"));

  const auto reordered = rep::CapacityPayload::make({slot_one, later, slot_two});
  REP_REQUIRE(reordered.ok());
  REP_CHECK(reordered.value() == payload.value());
  REP_CHECK_EQ(rep::digest_of(rep::domains::kCapacity, reordered.value()),
               rep::digest_of(rep::domains::kCapacity, payload.value()));

  // Two capacities for one destination contradict each other even when the
  // numbers differ.
  rep::DestinationCapacity conflicting = slot_one;
  conflicting.generation = rep::Generation(10);
  const auto duplicate = rep::CapacityPayload::make({slot_one, conflicting});
  REP_REQUIRE(!duplicate.ok());
  REP_CHECK_EQ(duplicate.error().code, rep::ErrorCode::DuplicateIdentity);

  const rep::DestinationCapacity* found =
      payload.value().find(destination(rep::DestinationKind::RackSlot, "rack-1"));
  REP_REQUIRE(found != nullptr);
  REP_CHECK_EQ(found->generation, rep::Generation(9));
  REP_CHECK_EQ(found->available.get(rep::ResourceClass::CpuMillicores), std::uint64_t{64});
  REP_CHECK(payload.value().find(destination(rep::DestinationKind::RackSlot, "rack-9")) == nullptr);
  // The same id under another kind is a different destination.
  REP_CHECK(payload.value().find(destination(rep::DestinationKind::FabricEndpoint, "rack-1")) ==
            nullptr);

  const auto empty = rep::CapacityPayload::make({});
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().destinations.empty());
  REP_CHECK(empty.value().find(destination(rep::DestinationKind::RackSlot, "rack-1")) == nullptr);
}

REP_TEST(Domain, AsiWorkloadPayloadCanonicalAndFind) {
  rep::WorkloadStateRecord second;
  second.obligation = rep::ObligationId("ob-2");
  second.lifecycle = rep::WorkloadLifecycle::Paused;
  second.migration = rep::MigrationCapability::ColdOnly;
  second.attachment = rep::StorageAttachment::LocalState;

  rep::WorkloadStateRecord first;
  first.obligation = rep::ObligationId("ob-1");
  first.lifecycle = rep::WorkloadLifecycle::Running;
  first.migration = rep::MigrationCapability::LiveAllowed;
  first.attachment = rep::StorageAttachment::SharedVolume;

  const auto payload = rep::AsiWorkloadPayload::make({second, first});
  REP_REQUIRE(payload.ok());
  REP_REQUIRE(payload.value().records.size() == 2);
  REP_CHECK_EQ(payload.value().records[0].obligation.str(), std::string("ob-1"));
  REP_CHECK_EQ(payload.value().records[1].obligation.str(), std::string("ob-2"));

  const auto reordered = rep::AsiWorkloadPayload::make({first, second});
  REP_REQUIRE(reordered.ok());
  REP_CHECK(reordered.value() == payload.value());

  rep::WorkloadStateRecord conflicting = first;
  conflicting.lifecycle = rep::WorkloadLifecycle::Failed;
  const auto duplicate = rep::AsiWorkloadPayload::make({first, conflicting});
  REP_REQUIRE(!duplicate.ok());
  REP_CHECK_EQ(duplicate.error().code, rep::ErrorCode::DuplicateIdentity);

  const rep::WorkloadStateRecord* found = payload.value().find(rep::ObligationId("ob-1"));
  REP_REQUIRE(found != nullptr);
  REP_CHECK(found->lifecycle == rep::WorkloadLifecycle::Running);
  REP_CHECK(found->migration == rep::MigrationCapability::LiveAllowed);
  REP_CHECK(found->attachment == rep::StorageAttachment::SharedVolume);
  REP_CHECK(payload.value().find(rep::ObligationId("ob-3")) == nullptr);

  const auto empty = rep::AsiWorkloadPayload::make({});
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().find(rep::ObligationId("ob-1")) == nullptr);
}

REP_TEST(Domain, DfiObligationPayloadCanonicalAndFind) {
  rep::FabricObligationRecord second;
  second.obligation = rep::ObligationId("ob-2");
  second.kind = rep::FabricObligationKind::ZoningEntry;
  second.path_migration_supported = true;
  second.permitted_endpoints = {destination(rep::DestinationKind::FabricEndpoint, "fc-2"),
                                destination(rep::DestinationKind::FabricEndpoint, "fc-1")};

  rep::FabricObligationRecord first;
  first.obligation = rep::ObligationId("ob-1");
  first.kind = rep::FabricObligationKind::PathAttachment;
  first.mapping_digest = rep::sha256_of("mapping");
  first.permitted_endpoints = {destination(rep::DestinationKind::FabricEndpoint, "fc-1")};

  const auto payload = rep::DfiObligationPayload::make({second, first});
  REP_REQUIRE(payload.ok());
  REP_REQUIRE(payload.value().records.size() == 2);
  REP_CHECK_EQ(payload.value().records[0].obligation.str(), std::string("ob-1"));
  REP_CHECK_EQ(payload.value().records[1].obligation.str(), std::string("ob-2"));

  const auto reordered = rep::DfiObligationPayload::make({first, second});
  REP_REQUIRE(reordered.ok());
  REP_CHECK(reordered.value() == payload.value());

  rep::FabricObligationRecord conflicting = first;
  conflicting.path_migration_supported = true;
  const auto duplicate = rep::DfiObligationPayload::make({first, conflicting});
  REP_REQUIRE(!duplicate.ok());
  REP_CHECK_EQ(duplicate.error().code, rep::ErrorCode::DuplicateIdentity);

  const rep::FabricObligationRecord* found = payload.value().find(rep::ObligationId("ob-1"));
  REP_REQUIRE(found != nullptr);
  REP_CHECK(found->kind == rep::FabricObligationKind::PathAttachment);
  REP_CHECK(!found->path_migration_supported);
  REP_CHECK_EQ(found->mapping_digest, rep::sha256_of("mapping"));
  REP_REQUIRE(found->permitted_endpoints.size() == 1);
  REP_CHECK(payload.value().find(rep::ObligationId("ob-9")) == nullptr);

  // The permitted endpoint set is part of the canonical form: the payload
  // factory sorts and dedupes it, so a record whose list arrives unsorted is
  // the same payload as the sorted one.  The planner searches this list, so
  // the normalisation is load bearing.
  const rep::FabricObligationRecord* second_record = payload.value().find(rep::ObligationId("ob-2"));
  REP_REQUIRE(second_record != nullptr);
  REP_REQUIRE(second_record->permitted_endpoints.size() == 2);
  REP_CHECK(second_record->permitted_endpoints[0] ==
            destination(rep::DestinationKind::FabricEndpoint, "fc-1"));
  REP_CHECK(second_record->permitted_endpoints[1] ==
            destination(rep::DestinationKind::FabricEndpoint, "fc-2"));

  rep::FabricObligationRecord duplicated = second;
  duplicated.permitted_endpoints.push_back(
      destination(rep::DestinationKind::FabricEndpoint, "fc-1"));
  const auto deduped = rep::DfiObligationPayload::make({duplicated});
  REP_REQUIRE(deduped.ok());
  REP_REQUIRE(deduped.value().records.size() == 1);
  REP_CHECK(deduped.value().records[0].permitted_endpoints.size() == 2);

  const auto empty = rep::DfiObligationPayload::make({});
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().find(rep::ObligationId("ob-1")) == nullptr);
}

REP_TEST(Domain, CandidateOffersCanonicalAndFind) {
  const rep::Candidate second = make_candidate("cand-2", "ob-2", rep::ActionKind::ColdMigrate,
                                               destination(rep::DestinationKind::RackSlot, "rack-2"));
  const rep::Candidate first = make_candidate("cand-1", "ob-1", rep::ActionKind::LiveMigrate,
                                              destination(rep::DestinationKind::RackSlot, "rack-1"));

  const auto payload = rep::CandidateOffersPayload::make({second, first});
  REP_REQUIRE(payload.ok());
  REP_REQUIRE(payload.value().candidates.size() == 2);
  REP_CHECK_EQ(payload.value().candidates[0].id.str(), std::string("cand-1"));
  REP_CHECK_EQ(payload.value().candidates[1].id.str(), std::string("cand-2"));

  const auto reordered = rep::CandidateOffersPayload::make({first, second});
  REP_REQUIRE(reordered.ok());
  REP_CHECK(reordered.value() == payload.value());

  const rep::Candidate conflicting = make_candidate(
      "cand-1", "ob-1", rep::ActionKind::Quiesce,
      destination(rep::DestinationKind::RackSlot, "rack-3"));
  REP_CHECK(conflicting != first);
  const auto duplicate = rep::CandidateOffersPayload::make({first, conflicting});
  REP_REQUIRE(!duplicate.ok());
  REP_CHECK_EQ(duplicate.error().code, rep::ErrorCode::DuplicateIdentity);

  const rep::Candidate* found = payload.value().find(rep::CandidateId("cand-1"));
  REP_REQUIRE(found != nullptr);
  REP_CHECK(found->action == rep::ActionKind::LiveMigrate);
  REP_CHECK(found->destination == destination(rep::DestinationKind::RackSlot, "rack-1"));
  REP_CHECK(payload.value().find(rep::CandidateId("cand-9")) == nullptr);

  const auto empty = rep::CandidateOffersPayload::make({});
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().find(rep::CandidateId("cand-1")) == nullptr);
}

REP_TEST(Domain, PlacementPolicySetSemantics) {
  rep::PolicyRule allow_workload;
  allow_workload.kind = rep::PolicyRuleKind::AllowAction;
  allow_workload.obligation_kind = rep::ObligationKind::Workload;
  allow_workload.action = rep::ActionKind::LiveMigrate;
  allow_workload.destination_kind = rep::DestinationKind::RackSlot;

  rep::PolicyRule deny_storage;
  deny_storage.kind = rep::PolicyRuleKind::DenyAction;
  deny_storage.obligation_kind = rep::ObligationKind::StorageReplica;
  deny_storage.action = rep::ActionKind::Detach;
  deny_storage.destination_kind = rep::DestinationKind::StorageTarget;

  // Policy rules are a set: an exact repeat is meaningless and is dropped.
  const auto payload =
      rep::PlacementPolicyPayload::make({deny_storage, allow_workload, allow_workload});
  REP_REQUIRE(payload.ok());
  REP_REQUIRE(payload.value().rules.size() == 2);
  // Sorted by kind first: AllowAction (1) before DenyAction (2).
  REP_CHECK(payload.value().rules[0].kind == rep::PolicyRuleKind::AllowAction);
  REP_CHECK(payload.value().rules[1].kind == rep::PolicyRuleKind::DenyAction);

  // Rules that differ only in the scoped domain are distinct and are ordered
  // by the domain text.
  rep::PolicyRule any_domain = allow_workload;
  rep::PolicyRule scoped = allow_workload;
  scoped.domain = rep::FailureDomainId("domain-1");
  rep::PolicyRule scoped_later = allow_workload;
  scoped_later.domain = rep::FailureDomainId("domain-2");

  const auto with_domains = rep::PlacementPolicyPayload::make({scoped_later, any_domain, scoped});
  REP_REQUIRE(with_domains.ok());
  REP_REQUIRE(with_domains.value().rules.size() == 3);
  REP_CHECK(with_domains.value().rules[0].domain.empty());
  REP_CHECK_EQ(with_domains.value().rules[1].domain.str(), std::string("domain-1"));
  REP_CHECK_EQ(with_domains.value().rules[2].domain.str(), std::string("domain-2"));

  // The same set in any input order produces the same payload and digest.
  const auto shuffled = rep::PlacementPolicyPayload::make({scoped, scoped_later, any_domain});
  REP_REQUIRE(shuffled.ok());
  REP_CHECK(shuffled.value() == with_domains.value());
  REP_CHECK_EQ(rep::digest_of(rep::domains::kPlacementPolicy, shuffled.value()),
               rep::digest_of(rep::domains::kPlacementPolicy, with_domains.value()));

  // An empty rule set is a policy decision, not an error.
  const auto empty = rep::PlacementPolicyPayload::make({});
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().rules.empty());
  REP_CHECK_NE(rep::digest_of(rep::domains::kPlacementPolicy, empty.value()),
               rep::digest_of(rep::domains::kPlacementPolicy, with_domains.value()));
}

REP_TEST(Domain, MaintenancePayloadWindowsAndIncidents) {
  const rep::MaintenanceWindow early = window(rep::UnixNanos(100), rep::UnixNanos(200));
  const rep::MaintenanceWindow late = window(rep::UnixNanos(300), rep::UnixNanos(400));

  rep::Incident advisory;
  advisory.domain = rep::FailureDomainId("domain-1");
  advisory.severity = rep::IncidentSeverity::Advisory;
  advisory.blocks_evacuation = false;

  rep::Incident critical;
  critical.domain = rep::FailureDomainId("domain-1");
  critical.severity = rep::IncidentSeverity::Critical;

  const auto payload = rep::MaintenancePayload::make({late, early}, {critical, advisory});
  REP_REQUIRE(payload.ok());
  REP_REQUIRE(payload.value().windows.size() == 2);
  // Windows are ordered by start time.
  REP_CHECK_EQ(payload.value().windows[0].starts_at, rep::UnixNanos(100));
  REP_CHECK_EQ(payload.value().windows[1].starts_at, rep::UnixNanos(300));
  REP_REQUIRE(payload.value().incidents.size() == 2);
  // Incidents are ordered by domain then severity.
  REP_CHECK(payload.value().incidents[0].severity == rep::IncidentSeverity::Advisory);
  REP_CHECK(payload.value().incidents[1].severity == rep::IncidentSeverity::Critical);

  // Windows and incidents are both sets: exact repeats are dropped.
  const auto deduped = rep::MaintenancePayload::make({early, early, late},
                                                     {critical, critical, advisory});
  REP_REQUIRE(deduped.ok());
  REP_CHECK(deduped.value() == payload.value());

  // Two windows on the same interval with different actions are distinct.
  rep::MaintenanceWindow detach_window = early;
  detach_window.action = rep::ActionKind::Detach;
  const auto with_action = rep::MaintenancePayload::make({early, detach_window}, {});
  REP_REQUIRE(with_action.ok());
  REP_REQUIRE(with_action.value().windows.size() == 2);
  REP_CHECK(with_action.value().windows[0].action == rep::ActionKind::LiveMigrate);
  REP_CHECK(with_action.value().windows[1].action == rep::ActionKind::Detach);

  // A window that does not end after it starts is refused, including an empty
  // window.
  const auto backwards =
      rep::MaintenancePayload::make({window(rep::UnixNanos(200), rep::UnixNanos(100))}, {});
  REP_REQUIRE(!backwards.ok());
  REP_CHECK_EQ(backwards.error().code, rep::ErrorCode::InvalidArgument);
  REP_CHECK_EQ(backwards.error().field, std::string("window"));

  const auto empty_window =
      rep::MaintenancePayload::make({window(rep::UnixNanos(100), rep::UnixNanos(100))}, {});
  REP_REQUIRE(!empty_window.ok());
  REP_CHECK_EQ(empty_window.error().code, rep::ErrorCode::InvalidArgument);

  // Negative instants are allowed: the check is "ends after starts", not
  // "starts after the epoch".
  const auto negative =
      rep::MaintenancePayload::make({window(rep::UnixNanos(-200), rep::UnixNanos(-100))}, {});
  REP_REQUIRE(negative.ok());
  REP_REQUIRE(negative.value().windows.size() == 1);

  const auto empty = rep::MaintenancePayload::make({}, {});
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().windows.empty());
  REP_CHECK(empty.value().incidents.empty());
  REP_CHECK_NE(rep::digest_of(rep::domains::kMaintenance, empty.value()),
               rep::digest_of(rep::domains::kMaintenance, payload.value()));
}

REP_TEST(Domain, MaintenanceWindowHalfOpenInterval) {
  const rep::MaintenanceWindow open = window(rep::UnixNanos(100), rep::UnixNanos(200));
  // [start, end): the start is inside, the end is not.
  REP_CHECK(!open.is_open_at(rep::UnixNanos(99)));
  REP_CHECK(open.is_open_at(rep::UnixNanos(100)));
  REP_CHECK(open.is_open_at(rep::UnixNanos(101)));
  REP_CHECK(open.is_open_at(rep::UnixNanos(199)));
  REP_CHECK(!open.is_open_at(rep::UnixNanos(200)));
  REP_CHECK(!open.is_open_at(rep::UnixNanos(201)));

  // Adjacent windows tile without overlapping or leaving a gap.
  const rep::MaintenanceWindow next = window(rep::UnixNanos(200), rep::UnixNanos(300));
  REP_CHECK(!open.is_open_at(rep::UnixNanos(200)));
  REP_CHECK(next.is_open_at(rep::UnixNanos(200)));
  REP_CHECK(!next.is_open_at(rep::UnixNanos(300)));

  // The same holds for negative instants.
  const rep::MaintenanceWindow negative = window(rep::UnixNanos(-200), rep::UnixNanos(-100));
  REP_CHECK(!negative.is_open_at(rep::UnixNanos(-201)));
  REP_CHECK(negative.is_open_at(rep::UnixNanos(-200)));
  REP_CHECK(negative.is_open_at(rep::UnixNanos(-101)));
  REP_CHECK(!negative.is_open_at(rep::UnixNanos(-100)));

  // permits_evacuation does not change the interval.
  rep::MaintenanceWindow freezing = open;
  freezing.permits_evacuation = true;
  REP_CHECK_EQ(freezing.is_open_at(rep::UnixNanos(150)), open.is_open_at(rep::UnixNanos(150)));
  REP_CHECK_EQ(freezing.is_open_at(rep::UnixNanos(200)), open.is_open_at(rep::UnixNanos(200)));
}

REP_TEST(Domain, MaintenanceWindowCoversDestination) {
  const rep::DestinationRef slot_one = destination(rep::DestinationKind::RackSlot, "rack-1");
  const rep::DestinationRef slot_two = destination(rep::DestinationKind::RackSlot, "rack-2");
  const rep::DestinationRef fabric = destination(rep::DestinationKind::FabricEndpoint, "fc-1");
  const std::optional<rep::FailureDomainId> domain_one = rep::FailureDomainId("domain-1");
  const std::optional<rep::FailureDomainId> domain_two = rep::FailureDomainId("domain-2");
  const std::optional<rep::FailureDomainId> no_domain;

  // Unscoped: an empty destination and an empty domain cover everything.
  const rep::MaintenanceWindow any = window(rep::UnixNanos(0), rep::UnixNanos(10));
  REP_CHECK(any.covers_destination(slot_one, domain_one));
  REP_CHECK(any.covers_destination(slot_two, domain_two));
  REP_CHECK(any.covers_destination(fabric, no_domain));
  REP_CHECK(any.covers_destination(rep::DestinationRef(), no_domain));

  // Destination scoped: only that destination, whatever its domain.
  rep::MaintenanceWindow on_slot_one = any;
  on_slot_one.destination = slot_one;
  REP_CHECK(on_slot_one.covers_destination(slot_one, domain_one));
  REP_CHECK(on_slot_one.covers_destination(slot_one, domain_two));
  REP_CHECK(on_slot_one.covers_destination(slot_one, no_domain));
  REP_CHECK(!on_slot_one.covers_destination(slot_two, domain_one));
  REP_CHECK(!on_slot_one.covers_destination(fabric, domain_one));
  // The kind is part of the destination identity.
  REP_CHECK(!on_slot_one.covers_destination(destination(rep::DestinationKind::FabricEndpoint, "rack-1"),
                                            domain_one));

  // Domain scoped: only a candidate proven to be in that domain.
  rep::MaintenanceWindow on_domain_one = any;
  on_domain_one.domain = rep::FailureDomainId("domain-1");
  REP_CHECK(on_domain_one.covers_destination(slot_one, domain_one));
  REP_CHECK(on_domain_one.covers_destination(slot_two, domain_one));
  REP_CHECK(!on_domain_one.covers_destination(slot_one, domain_two));
  // An unknown domain never satisfies a domain scope.
  REP_CHECK(!on_domain_one.covers_destination(slot_one, no_domain));

  // Both scopes: the candidate must match the destination and the domain.
  rep::MaintenanceWindow both = on_domain_one;
  both.destination = slot_one;
  REP_CHECK(both.covers_destination(slot_one, domain_one));
  REP_CHECK(!both.covers_destination(slot_two, domain_one));
  REP_CHECK(!both.covers_destination(slot_one, domain_two));
  REP_CHECK(!both.covers_destination(slot_one, no_domain));
  REP_CHECK(!both.covers_destination(fabric, domain_one));
}

REP_TEST(Domain, FailureDomainTopologyRefusals) {
  const auto rack_member = [](std::string_view rack, std::string_view domain) {
    rep::DomainMember member;
    member.domain = rep::FailureDomainId(std::string(domain));
    member.is_rack = true;
    member.rack = rep::RackId(std::string(rack));
    return member;
  };
  const auto destination_member = [](const rep::DestinationRef& target, std::string_view domain) {
    rep::DomainMember member;
    member.domain = rep::FailureDomainId(std::string(domain));
    member.destination = target;
    return member;
  };
  const auto obligation_member = [](std::string_view obligation, std::string_view domain) {
    rep::DomainMember member;
    member.domain = rep::FailureDomainId(std::string(domain));
    member.obligation = rep::ObligationId(std::string(obligation));
    return member;
  };

  // A member with no domain is refused: a subject is never "no domain".
  const auto empty_domain = rep::FailureDomainTopology::make(
      {destination_member(destination(rep::DestinationKind::RackSlot, "rack-1"), "")});
  REP_REQUIRE(!empty_domain.ok());
  REP_CHECK_EQ(empty_domain.error().code, rep::ErrorCode::InvalidArgument);

  // A member that names no subject is refused.
  rep::DomainMember no_subject;
  no_subject.domain = rep::FailureDomainId("domain-1");
  const auto subjectless = rep::FailureDomainTopology::make({no_subject});
  REP_REQUIRE(!subjectless.ok());
  REP_CHECK_EQ(subjectless.error().code, rep::ErrorCode::InvalidArgument);
  REP_CHECK_EQ(subjectless.error().field, std::string("member"));

  // A rack member with no rack is refused.
  const auto empty_rack = rep::FailureDomainTopology::make({rack_member("", "domain-1")});
  REP_REQUIRE(!empty_rack.ok());
  REP_CHECK_EQ(empty_rack.error().code, rep::ErrorCode::InvalidArgument);

  // A member that names two subjects is refused: one member, one subject.
  rep::DomainMember two_subjects = destination_member(
      destination(rep::DestinationKind::RackSlot, "rack-1"), "domain-1");
  two_subjects.obligation = rep::ObligationId("ob-1");
  const auto ambiguous = rep::FailureDomainTopology::make({two_subjects});
  REP_REQUIRE(!ambiguous.ok());
  REP_CHECK_EQ(ambiguous.error().code, rep::ErrorCode::InvalidArgument);
  REP_CHECK_EQ(ambiguous.error().field, std::string("member"));

  // One rack in two domains is a contradiction.
  const auto split_rack = rep::FailureDomainTopology::make(
      {rack_member("rack-1", "domain-1"), rack_member("rack-1", "domain-2")});
  REP_REQUIRE(!split_rack.ok());
  REP_CHECK_EQ(split_rack.error().code, rep::ErrorCode::PreconditionFailed);

  // One destination in two domains is a contradiction.
  const auto split_destination = rep::FailureDomainTopology::make(
      {destination_member(destination(rep::DestinationKind::RackSlot, "rack-1"), "domain-1"),
       destination_member(destination(rep::DestinationKind::RackSlot, "rack-1"), "domain-2")});
  REP_REQUIRE(!split_destination.ok());
  REP_CHECK_EQ(split_destination.error().code, rep::ErrorCode::PreconditionFailed);

  // One obligation in two domains is a contradiction.
  const auto split_obligation = rep::FailureDomainTopology::make(
      {obligation_member("ob-1", "domain-1"), obligation_member("ob-1", "domain-2")});
  REP_REQUIRE(!split_obligation.ok());
  REP_CHECK_EQ(split_obligation.error().code, rep::ErrorCode::PreconditionFailed);

  // A member repeated exactly is a set repeat and is dropped, not refused.
  const auto repeated = rep::FailureDomainTopology::make(
      {rack_member("rack-1", "domain-1"), rack_member("rack-1", "domain-1")});
  REP_REQUIRE(repeated.ok());
  REP_CHECK_EQ(repeated.value().members.size(), std::size_t{1});

  // A rack and a destination that share a name are different subjects and may
  // live in different domains.
  const auto mixed = rep::FailureDomainTopology::make(
      {rack_member("rack-1", "domain-1"),
       destination_member(destination(rep::DestinationKind::RackSlot, "rack-1"), "domain-2")});
  REP_REQUIRE(mixed.ok());
  REP_CHECK_EQ(mixed.value().members.size(), std::size_t{2});

  const auto empty = rep::FailureDomainTopology::make({});
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().members.empty());
}

REP_TEST(Domain, FailureDomainTopologyLookups) {
  rep::DomainMember rack_member;
  rack_member.domain = rep::FailureDomainId("domain-racks");
  rack_member.is_rack = true;
  rack_member.rack = rep::RackId("rack-a");

  rep::DomainMember destination_member;
  destination_member.domain = rep::FailureDomainId("domain-fabric");
  destination_member.destination = destination(rep::DestinationKind::FabricEndpoint, "fc-1");

  rep::DomainMember obligation_member;
  obligation_member.domain = rep::FailureDomainId("domain-workloads");
  obligation_member.obligation = rep::ObligationId("ob-1");

  const auto topology =
      rep::FailureDomainTopology::make({obligation_member, destination_member, rack_member});
  REP_REQUIRE(topology.ok());

  // Canonical order: destination and obligation members (is_rack false) sort
  // before rack members, and the subject fields decide inside each group.  The
  // obligation member carries the default destination, which sorts before the
  // fabric endpoint.
  REP_REQUIRE(topology.value().members.size() == 3);
  REP_CHECK(!topology.value().members[0].is_rack);
  REP_CHECK(topology.value().members[0].destination.id.empty());
  REP_CHECK_EQ(topology.value().members[0].obligation.str(), std::string("ob-1"));
  REP_CHECK(topology.value().members[1].destination ==
            destination(rep::DestinationKind::FabricEndpoint, "fc-1"));
  REP_CHECK(topology.value().members[1].obligation.empty());
  REP_CHECK(topology.value().members[2].is_rack);
  REP_CHECK_EQ(topology.value().members[2].rack.str(), std::string("rack-a"));

  const auto rack = topology.value().domain_of_rack(rep::RackId("rack-a"));
  REP_REQUIRE(rack.has_value());
  REP_CHECK_EQ(rack->str(), std::string("domain-racks"));
  REP_CHECK(!topology.value().domain_of_rack(rep::RackId("rack-b")).has_value());
  REP_CHECK(!topology.value().domain_of_rack(rep::RackId()).has_value());

  const auto found_destination = topology.value().domain_of_destination(
      destination(rep::DestinationKind::FabricEndpoint, "fc-1"));
  REP_REQUIRE(found_destination.has_value());
  REP_CHECK_EQ(found_destination->str(), std::string("domain-fabric"));
  // The same id under another kind is unknown.
  REP_CHECK(!topology.value()
                 .domain_of_destination(destination(rep::DestinationKind::RackSlot, "fc-1"))
                 .has_value());
  REP_CHECK(!topology.value().domain_of_destination(rep::DestinationRef()).has_value());

  const auto found_obligation = topology.value().domain_of_obligation(rep::ObligationId("ob-1"));
  REP_REQUIRE(found_obligation.has_value());
  REP_CHECK_EQ(found_obligation->str(), std::string("domain-workloads"));
  REP_CHECK(!topology.value().domain_of_obligation(rep::ObligationId("ob-2")).has_value());
  // An absent subject is unknown, never a default domain.
  REP_CHECK(!topology.value().domain_of_obligation(rep::ObligationId()).has_value());

  const auto empty = rep::FailureDomainTopology::make({});
  REP_REQUIRE(empty.ok());
  REP_CHECK(!empty.value().domain_of_rack(rep::RackId("rack-a")).has_value());
  REP_CHECK(!empty.value()
                 .domain_of_destination(destination(rep::DestinationKind::RackSlot, "rack-a"))
                 .has_value());
  REP_CHECK(!empty.value().domain_of_obligation(rep::ObligationId("ob-1")).has_value());
}

REP_TEST(Domain, CandidateContentDigest) {
  const rep::DestinationRef target = destination(rep::DestinationKind::RackSlot, "rack-2");
  const auto provision = rep::ResourceVector::make(
      {{rep::ResourceClass::CpuMillicores, 8}, {rep::ResourceClass::MemoryBytes, 4096}});
  REP_REQUIRE(provision.ok());

  const auto made = rep::Candidate::make(rep::CandidateId("cand-1"), rep::ObligationId("ob-1"),
                                         rep::ActionKind::LiveMigrate, target,
                                         rep::AuthorityId("authority-1"), rep::Generation(5),
                                         rep::Epoch(2), provision.value(), 11, true);
  REP_REQUIRE(made.ok());

  // make() computes the declaration digest over the candidate's own content.
  REP_CHECK_EQ(made.value().evidence_digest, made.value().content_digest());
  REP_CHECK(!made.value().content_digest().is_zero());

  // The content digest is the digest of encode_content() under the candidate
  // domain, and encode() appends the declared digest afterwards.
  rep::CanonicalWriter content(rep::domains::kCandidate);
  made.value().encode_content(content);
  REP_CHECK_EQ(content.finish(), made.value().content_digest());
  rep::CanonicalWriter whole(rep::domains::kCandidate);
  made.value().encode(whole);
  REP_CHECK_EQ(whole.size(), content.size() + 32);
  REP_CHECK_EQ(whole.to_hex().substr(0, content.to_hex().size()), content.to_hex());
  REP_CHECK_EQ(whole.to_hex().substr(content.to_hex().size()),
               made.value().evidence_digest.to_hex());

  // Every field participates in the digest.
  const auto other_id = rep::Candidate::make(
      rep::CandidateId("cand-2"), rep::ObligationId("ob-1"), rep::ActionKind::LiveMigrate, target,
      rep::AuthorityId("authority-1"), rep::Generation(5), rep::Epoch(2), provision.value(), 11,
      true);
  REP_REQUIRE(other_id.ok());
  REP_CHECK_NE(other_id.value().content_digest(), made.value().content_digest());

  const auto other_obligation = rep::Candidate::make(
      rep::CandidateId("cand-1"), rep::ObligationId("ob-2"), rep::ActionKind::LiveMigrate, target,
      rep::AuthorityId("authority-1"), rep::Generation(5), rep::Epoch(2), provision.value(), 11,
      true);
  REP_REQUIRE(other_obligation.ok());
  REP_CHECK_NE(other_obligation.value().content_digest(), made.value().content_digest());

  const auto other_action = rep::Candidate::make(
      rep::CandidateId("cand-1"), rep::ObligationId("ob-1"), rep::ActionKind::ColdMigrate, target,
      rep::AuthorityId("authority-1"), rep::Generation(5), rep::Epoch(2), provision.value(), 11,
      true);
  REP_REQUIRE(other_action.ok());
  REP_CHECK_NE(other_action.value().content_digest(), made.value().content_digest());

  const auto other_destination = rep::Candidate::make(
      rep::CandidateId("cand-1"), rep::ObligationId("ob-1"), rep::ActionKind::LiveMigrate,
      destination(rep::DestinationKind::RackSlot, "rack-3"), rep::AuthorityId("authority-1"),
      rep::Generation(5), rep::Epoch(2), provision.value(), 11, true);
  REP_REQUIRE(other_destination.ok());
  REP_CHECK_NE(other_destination.value().content_digest(), made.value().content_digest());

  const auto other_authority = rep::Candidate::make(
      rep::CandidateId("cand-1"), rep::ObligationId("ob-1"), rep::ActionKind::LiveMigrate, target,
      rep::AuthorityId("authority-2"), rep::Generation(5), rep::Epoch(2), provision.value(), 11,
      true);
  REP_REQUIRE(other_authority.ok());
  REP_CHECK_NE(other_authority.value().content_digest(), made.value().content_digest());

  const auto other_generation = rep::Candidate::make(
      rep::CandidateId("cand-1"), rep::ObligationId("ob-1"), rep::ActionKind::LiveMigrate, target,
      rep::AuthorityId("authority-1"), rep::Generation(6), rep::Epoch(2), provision.value(), 11,
      true);
  REP_REQUIRE(other_generation.ok());
  REP_CHECK_NE(other_generation.value().content_digest(), made.value().content_digest());

  const auto other_epoch = rep::Candidate::make(
      rep::CandidateId("cand-1"), rep::ObligationId("ob-1"), rep::ActionKind::LiveMigrate, target,
      rep::AuthorityId("authority-1"), rep::Generation(5), rep::Epoch(3), provision.value(), 11,
      true);
  REP_REQUIRE(other_epoch.ok());
  REP_CHECK_NE(other_epoch.value().content_digest(), made.value().content_digest());

  const auto other_provision = rep::ResourceVector::make({{rep::ResourceClass::CpuMillicores, 9}});
  REP_REQUIRE(other_provision.ok());
  const auto provision_changed = rep::Candidate::make(
      rep::CandidateId("cand-1"), rep::ObligationId("ob-1"), rep::ActionKind::LiveMigrate, target,
      rep::AuthorityId("authority-1"), rep::Generation(5), rep::Epoch(2), other_provision.value(),
      11, true);
  REP_REQUIRE(provision_changed.ok());
  REP_CHECK_NE(provision_changed.value().content_digest(), made.value().content_digest());

  const auto cost_changed = rep::Candidate::make(
      rep::CandidateId("cand-1"), rep::ObligationId("ob-1"), rep::ActionKind::LiveMigrate, target,
      rep::AuthorityId("authority-1"), rep::Generation(5), rep::Epoch(2), provision.value(), 12,
      true);
  REP_REQUIRE(cost_changed.ok());
  REP_CHECK_NE(cost_changed.value().content_digest(), made.value().content_digest());

  const auto window_changed = rep::Candidate::make(
      rep::CandidateId("cand-1"), rep::ObligationId("ob-1"), rep::ActionKind::LiveMigrate, target,
      rep::AuthorityId("authority-1"), rep::Generation(5), rep::Epoch(2), provision.value(), 11,
      false);
  REP_REQUIRE(window_changed.ok());
  REP_CHECK_NE(window_changed.value().content_digest(), made.value().content_digest());

  // The same fields always produce the same digest and the same candidate.
  const auto repeated = rep::Candidate::make(
      rep::CandidateId("cand-1"), rep::ObligationId("ob-1"), rep::ActionKind::LiveMigrate, target,
      rep::AuthorityId("authority-1"), rep::Generation(5), rep::Epoch(2), provision.value(), 11,
      true);
  REP_REQUIRE(repeated.ok());
  REP_CHECK(repeated.value() == made.value());
  REP_CHECK_EQ(repeated.value().evidence_digest, made.value().evidence_digest);

  // A candidate whose declared digest was altered no longer matches its own
  // content, which is exactly what the evidence bundle verifies.
  rep::Candidate tampered = made.value();
  tampered.evidence_digest = rep::sha256_of("not the content");
  REP_CHECK_NE(tampered.evidence_digest, tampered.content_digest());
}

REP_TEST(Domain, CandidateMakeRejectsInvalidInputs) {
  const rep::DestinationRef target = destination(rep::DestinationKind::RackSlot, "rack-2");
  const auto make = [](std::string_view id, std::string_view obligation,
                      std::string_view authority, const rep::DestinationRef& direct_target) {
    return rep::Candidate::make(rep::CandidateId(std::string(id)),
                                rep::ObligationId(std::string(obligation)),
                                rep::ActionKind::LiveMigrate, direct_target,
                                rep::AuthorityId(std::string(authority)), rep::Generation(1),
                                rep::Epoch(1), rep::ResourceVector(), 0, false);
  };

  const auto empty_id = make("", "ob-1", "authority-1", target);
  REP_REQUIRE(!empty_id.ok());
  REP_CHECK_EQ(empty_id.error().code, rep::ErrorCode::InvalidIdentifier);
  REP_CHECK_EQ(empty_id.error().field, std::string("id"));

  const auto empty_obligation = make("cand-1", "", "authority-1", target);
  REP_REQUIRE(!empty_obligation.ok());
  REP_CHECK_EQ(empty_obligation.error().code, rep::ErrorCode::InvalidIdentifier);
  REP_CHECK_EQ(empty_obligation.error().field, std::string("obligation"));

  const auto empty_authority = make("cand-1", "ob-1", "", target);
  REP_REQUIRE(!empty_authority.ok());
  REP_CHECK_EQ(empty_authority.error().code, rep::ErrorCode::InvalidIdentifier);
  REP_CHECK_EQ(empty_authority.error().field, std::string("authority"));

  const auto empty_destination = make("cand-1", "ob-1", "authority-1", rep::DestinationRef());
  REP_REQUIRE(!empty_destination.ok());
  REP_CHECK_EQ(empty_destination.error().code, rep::ErrorCode::InvalidIdentifier);
  REP_CHECK_EQ(empty_destination.error().field, std::string("destination"));

  // A rejected candidate never produced a digest to declare.
  const auto valid = make("cand-1", "ob-1", "authority-1", target);
  REP_REQUIRE(valid.ok());
  REP_CHECK(valid.value().evidence_digest == valid.value().content_digest());
}

REP_TEST(Domain, ActionMatchesObligationTable) {
  // Rows follow kObligations, columns follow kActions.  Every pair is
  // pinned, so a widened or narrowed compatibility table fails here.
  const bool expected[6][7] = {
      // LiveMigrate, ColdMigrate, Quiesce, Detach, Rebind, Release, Depower
      {true, true, true, false, false, false, false},    // Workload
      {false, false, false, true, true, false, false},   // StorageReplica
      {false, false, false, true, true, false, false},   // NetworkPath
      {false, false, true, true, true, false, false},    // ServiceEndpoint
      {false, true, false, false, false, true, true},    // Reservation
      {true, true, true, false, false, false, true},     // Appliance
  };
  for (std::size_t row = 0; row < std::size(kObligations); ++row) {
    for (std::size_t column = 0; column < std::size(kActions); ++column) {
      const std::string what = std::string("obligation ") +
                               std::string(rep::to_string(kObligations[row])) + " with action " +
                               std::string(rep::to_string(kActions[column]));
      REP_CHECK_MSG(rep::action_matches_obligation(kObligations[row], kActions[column]) ==
                        expected[row][column],
                    what);
    }
  }
  // The representative incompatibilities called out by the contract.
  REP_CHECK(!rep::action_matches_obligation(rep::ObligationKind::Workload, rep::ActionKind::Detach));
  REP_CHECK(!rep::action_matches_obligation(rep::ObligationKind::Workload,
                                            rep::ActionKind::Release));
  REP_CHECK(!rep::action_matches_obligation(rep::ObligationKind::StorageReplica,
                                            rep::ActionKind::LiveMigrate));
  REP_CHECK(!rep::action_matches_obligation(rep::ObligationKind::NetworkPath,
                                            rep::ActionKind::ColdMigrate));
  REP_CHECK(rep::action_matches_obligation(rep::ObligationKind::Workload,
                                           rep::ActionKind::LiveMigrate));
  REP_CHECK(rep::action_matches_obligation(rep::ObligationKind::StorageReplica,
                                           rep::ActionKind::Rebind));
  REP_CHECK(rep::action_matches_obligation(rep::ObligationKind::Appliance,
                                           rep::ActionKind::Depower));
  REP_CHECK(rep::action_matches_obligation(rep::ObligationKind::Reservation,
                                           rep::ActionKind::Release));
}

REP_TEST(Domain, ActionMatchesDestinationTable) {
  // Rows follow kActions, columns follow kDestinations.
  const bool expected[7][4] = {
      // RackSlot, FabricEndpoint, StorageTarget, ServiceEndpoint
      {true, false, false, false},   // LiveMigrate
      {true, false, false, false},   // ColdMigrate
      {true, false, false, false},   // Quiesce
      {false, true, true, true},     // Detach
      {false, true, true, true},     // Rebind
      {true, false, false, false},   // Release
      {true, false, false, false},   // Depower
  };
  for (std::size_t row = 0; row < std::size(kActions); ++row) {
    for (std::size_t column = 0; column < std::size(kDestinations); ++column) {
      const std::string what = std::string("action ") + std::string(rep::to_string(kActions[row])) +
                               " with destination " +
                               std::string(rep::to_string(kDestinations[column]));
      REP_CHECK_MSG(rep::action_matches_destination(kActions[row], kDestinations[column]) ==
                        expected[row][column],
                    what);
    }
  }
  // The representative incompatibilities called out by the contract.
  REP_CHECK(!rep::action_matches_destination(rep::ActionKind::LiveMigrate,
                                             rep::DestinationKind::FabricEndpoint));
  REP_CHECK(!rep::action_matches_destination(rep::ActionKind::ColdMigrate,
                                             rep::DestinationKind::StorageTarget));
  REP_CHECK(!rep::action_matches_destination(rep::ActionKind::Detach,
                                             rep::DestinationKind::RackSlot));
  REP_CHECK(!rep::action_matches_destination(rep::ActionKind::Rebind,
                                             rep::DestinationKind::RackSlot));
  REP_CHECK(rep::action_matches_destination(rep::ActionKind::LiveMigrate,
                                            rep::DestinationKind::RackSlot));
  REP_CHECK(rep::action_matches_destination(rep::ActionKind::Detach,
                                            rep::DestinationKind::FabricEndpoint));
  REP_CHECK(rep::action_matches_destination(rep::ActionKind::Rebind,
                                            rep::DestinationKind::ServiceEndpoint));
}

REP_TEST_MAIN()
