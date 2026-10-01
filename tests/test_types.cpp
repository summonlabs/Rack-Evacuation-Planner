// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Tests for the foundational value types: identifiers, checked arithmetic,
// strong counters, resource vectors, destination references, and the canonical
// text spelling of every enumeration.

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "rep/canonical.hpp"
#include "rep/types.hpp"

#include "testkit.hpp"

namespace {

constexpr std::uint64_t kU64Max = (std::numeric_limits<std::uint64_t>::max)();
constexpr std::uint32_t kU32Max = (std::numeric_limits<std::uint32_t>::max)();
constexpr std::int64_t kI64Max = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kI64Min = (std::numeric_limits<std::int64_t>::min)();

[[nodiscard]] std::string id_problem(std::string_view text) {
  return rep::describe_identifier_problem(text);
}

[[nodiscard]] std::string hex_of_text(std::string_view text) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  result.reserve(text.size() * 2);
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    result.push_back(kDigits[byte >> 4u]);
    result.push_back(kDigits[byte & 0x0fu]);
  }
  return result;
}

template <class Id>
void check_id_round_trip(std::string_view text) {
  const auto parsed = Id::parse(text);
  REP_REQUIRE(parsed.ok());
  REP_CHECK_EQ(parsed.value().view(), text);
  REP_CHECK_EQ(parsed.value().str(), std::string(text));
  REP_CHECK(!parsed.value().empty());

  const auto again = Id::parse(parsed.value().view());
  REP_REQUIRE(again.ok());
  REP_CHECK(again.value() == parsed.value());

  // encode() is the canonical text form: u64 length then the bytes.
  rep::CanonicalWriter writer("rep.test.id.v1");
  parsed.value().encode(writer);
  REP_CHECK_EQ(writer.size(), std::size_t{8} + text.size());
  REP_CHECK_EQ(writer.to_hex().substr(16), hex_of_text(text));
}

// Round trips every named value of one enumeration and pins the two negative
// behaviours: an unmapped code has no spelling, and "unknown" is only a value
// where the enumeration actually declares one.
template <class Enum, std::size_t N>
void check_enum_text(const char* label, const Enum (&values)[N],
                     std::optional<Enum> (*from_string)(std::string_view), bool has_unknown_value) {
  const std::string tag(label);
  std::vector<std::string_view> names;
  for (const Enum value : values) {
    const std::string_view name = rep::to_string(value);
    REP_CHECK_MSG(!name.empty(), "enum " + tag + " has no text for a declared value");
    if (!has_unknown_value) {
      REP_CHECK_MSG(name != std::string_view("unknown"),
                    "enum " + tag + " spells a declared value 'unknown'");
    }
    const auto parsed = from_string(name);
    REP_REQUIRE(parsed.has_value());
    REP_CHECK_MSG(*parsed == value,
                  "enum " + tag + " round trip failed for '" + std::string(name) + "'");
    for (const std::string_view other : names) {
      REP_CHECK_MSG(other != name,
                    "enum " + tag + " spells two values '" + std::string(name) + "'");
    }
    names.push_back(name);
  }

  // An unmapped numeric code has no name at all.
  const Enum unmapped = static_cast<Enum>(60000);
  REP_CHECK_EQ(rep::to_string(unmapped), std::string_view("unknown"));

  // Text that is not a spelling never parses, in any casing.
  REP_CHECK(!from_string("").has_value());
  REP_CHECK(!from_string("no_such_value").has_value());
  REP_CHECK(!from_string("Unknown").has_value());
  REP_CHECK(!from_string(std::string(rep::to_string(values[0])) + " ").has_value());

  if (has_unknown_value) {
    const auto parsed = from_string("unknown");
    REP_REQUIRE(parsed.has_value());
    REP_CHECK_EQ(rep::to_string(*parsed), std::string_view("unknown"));
  } else {
    REP_CHECK(!from_string("unknown").has_value());
  }

  // The unmapped code never survives a trip through its own text.
  const auto via_text = from_string(rep::to_string(unmapped));
  REP_CHECK(!via_text.has_value() || *via_text != unmapped);
}

constexpr rep::ObligationKind kObligationKinds[] = {
    rep::ObligationKind::Workload,        rep::ObligationKind::StorageReplica,
    rep::ObligationKind::NetworkPath,     rep::ObligationKind::ServiceEndpoint,
    rep::ObligationKind::Reservation,     rep::ObligationKind::Appliance};

constexpr rep::ActionKind kActionKinds[] = {
    rep::ActionKind::LiveMigrate, rep::ActionKind::ColdMigrate, rep::ActionKind::Quiesce,
    rep::ActionKind::Detach,      rep::ActionKind::Rebind,      rep::ActionKind::Release,
    rep::ActionKind::Depower};

constexpr rep::DestinationKind kDestinationKinds[] = {
    rep::DestinationKind::RackSlot, rep::DestinationKind::FabricEndpoint,
    rep::DestinationKind::StorageTarget, rep::DestinationKind::ServiceEndpoint};

constexpr rep::ResourceClass kResourceClasses[] = {
    rep::ResourceClass::CpuMillicores,   rep::ResourceClass::MemoryBytes,
    rep::ResourceClass::StorageBytes,    rep::ResourceClass::AcceleratorUnits,
    rep::ResourceClass::NetworkKib,      rep::ResourceClass::PowerWatts,
    rep::ResourceClass::RackUnits};

constexpr rep::IsolationKind kIsolationKinds[] = {
    rep::IsolationKind::Depower, rep::IsolationKind::ThermalConstraint,
    rep::IsolationKind::PhysicalService, rep::IsolationKind::NetworkIsolation};

constexpr rep::EvidenceKind kEvidenceKinds[] = {
    rep::EvidenceKind::RackComposition, rep::EvidenceKind::Enumeration,
    rep::EvidenceKind::ObligationCatalog, rep::EvidenceKind::Capacity,
    rep::EvidenceKind::PlacementPolicy, rep::EvidenceKind::Maintenance,
    rep::EvidenceKind::FailureDomain, rep::EvidenceKind::AsiWorkloadState,
    rep::EvidenceKind::DfiObligation, rep::EvidenceKind::CandidateOffers};

constexpr rep::PolicyRuleKind kPolicyRuleKinds[] = {
    rep::PolicyRuleKind::AllowAction, rep::PolicyRuleKind::DenyAction,
    rep::PolicyRuleKind::RequireDomainSpread, rep::PolicyRuleKind::AllowProtectedMove};

constexpr rep::WorkloadLifecycle kWorkloadLifecycles[] = {
    rep::WorkloadLifecycle::Running, rep::WorkloadLifecycle::Paused,
    rep::WorkloadLifecycle::Stopped, rep::WorkloadLifecycle::Failed,
    rep::WorkloadLifecycle::Unknown};

constexpr rep::MigrationCapability kMigrationCapabilities[] = {
    rep::MigrationCapability::LiveAllowed, rep::MigrationCapability::ColdOnly,
    rep::MigrationCapability::NotMigratable, rep::MigrationCapability::Unknown};

constexpr rep::StorageAttachment kStorageAttachments[] = {
    rep::StorageAttachment::Stateless, rep::StorageAttachment::LocalState,
    rep::StorageAttachment::SharedVolume, rep::StorageAttachment::Unknown};

constexpr rep::FabricObligationKind kFabricObligationKinds[] = {
    rep::FabricObligationKind::PathAttachment, rep::FabricObligationKind::ZoningEntry,
    rep::FabricObligationKind::ReplicaLease, rep::FabricObligationKind::NamespaceMapping};

constexpr rep::IncidentSeverity kIncidentSeverities[] = {
    rep::IncidentSeverity::Advisory, rep::IncidentSeverity::Degraded,
    rep::IncidentSeverity::Critical};

constexpr rep::PlanStatus kPlanStatuses[] = {
    rep::PlanStatus::Indeterminate, rep::PlanStatus::EmptySafe, rep::PlanStatus::Complete,
    rep::PlanStatus::Partial};

constexpr rep::SafetyVerdict kSafetyVerdicts[] = {
    rep::SafetyVerdict::Safe, rep::SafetyVerdict::NotProven, rep::SafetyVerdict::Indeterminate,
    rep::SafetyVerdict::Stale};

constexpr rep::StalenessKind kStalenessKinds[] = {
    rep::StalenessKind::GenerationMoved, rep::StalenessKind::EpochChanged,
    rep::StalenessKind::DigestChanged, rep::StalenessKind::StreamMissing};

constexpr rep::OutcomeKind kOutcomeKinds[] = {
    rep::OutcomeKind::Planned, rep::OutcomeKind::Replayed, rep::OutcomeKind::RejectedInput,
    rep::OutcomeKind::RejectedStaleEpoch, rep::OutcomeKind::RejectedIdempotencyConflict,
    rep::OutcomeKind::RejectedPrecondition};

constexpr rep::ResidualReason kResidualReasons[] = {
    rep::ResidualReason::NoCandidate, rep::ResidualReason::CandidatesRejected,
    rep::ResidualReason::ProtectedObligation, rep::ResidualReason::NonMigratable,
    rep::ResidualReason::DependencyUnsatisfied, rep::ResidualReason::DependencyCycle,
    rep::ResidualReason::CapacityExhausted, rep::ResidualReason::MaintenanceBlocked,
    rep::ResidualReason::IncidentActive, rep::ResidualReason::FailureDomainConflict,
    rep::ResidualReason::EvidenceMissing, rep::ResidualReason::PolicyDenied,
    rep::ResidualReason::LimitExceeded, rep::ResidualReason::StateUnknown};

constexpr rep::RejectionReason kRejectionReasons[] = {
    rep::RejectionReason::ObligationKindNotAllowed,
    rep::RejectionReason::ActionDestinationMismatch,
    rep::RejectionReason::SameFailureDomain,
    rep::RejectionReason::CapacityInsufficient,
    rep::RejectionReason::MaintenanceWindowRequired,
    rep::RejectionReason::MaintenanceWindowClosed,
    rep::RejectionReason::IncidentActive,
    rep::RejectionReason::PolicyDenied,
    rep::RejectionReason::ProtectedObligation,
    rep::RejectionReason::NonMigratable,
    rep::RejectionReason::StaleGeneration,
    rep::RejectionReason::StaleEpoch,
    rep::RejectionReason::AuthorityMismatch,
    rep::RejectionReason::DigestMismatch,
    rep::RejectionReason::DestinationNotPermitted,
    rep::RejectionReason::SelfDestination,
    rep::RejectionReason::DuplicateCandidate,
    rep::RejectionReason::CapacityClaimMismatch,
    rep::RejectionReason::DestinationUnknown,
    rep::RejectionReason::FailureDomainUnknown};

constexpr rep::IndeterminacyReason kIndeterminacyReasons[] = {
    rep::IndeterminacyReason::RackCompositionMissing,
    rep::IndeterminacyReason::RackCompositionRevisionMismatch,
    rep::IndeterminacyReason::RackCompositionConflict,
    rep::IndeterminacyReason::EnumerationMissing,
    rep::IndeterminacyReason::EnumerationIncomplete,
    rep::IndeterminacyReason::EnumerationRevisionMismatch,
    rep::IndeterminacyReason::EnumerationSetMismatch,
    rep::IndeterminacyReason::EnumerationConflict,
    rep::IndeterminacyReason::ObligationCatalogMissing,
    rep::IndeterminacyReason::ObligationUndefined,
    rep::IndeterminacyReason::ObligationDefinitionDuplicate,
    rep::IndeterminacyReason::ObligationSourceRackMismatch,
    rep::IndeterminacyReason::LimitExceeded};

constexpr rep::ScopeExclusionReason kScopeExclusionReasons[] = {
    rep::ScopeExclusionReason::KindNotRequested};

}  // namespace

REP_TEST(Types, IdentifierValidationAcceptsTheDocumentedAlphabet) {
  REP_CHECK(rep::is_valid_identifier("a"));
  REP_CHECK(rep::is_valid_identifier("A"));
  REP_CHECK(rep::is_valid_identifier("0"));
  REP_CHECK(rep::is_valid_identifier("rack-01"));
  REP_CHECK(rep::is_valid_identifier("rack_a.b"));
  REP_CHECK(rep::is_valid_identifier("rack/slot-3"));
  REP_CHECK(rep::is_valid_identifier("authority@fabric.example"));
  REP_CHECK(rep::is_valid_identifier("a+b"));
  REP_CHECK(rep::is_valid_identifier("AZaz09_./@+-"));
  REP_CHECK(rep::is_valid_identifier(std::string(rep::kMaxIdentifierLength, 'x')));
  REP_CHECK(!rep::is_valid_identifier("fabric.endpoint:svc"));
}

REP_TEST(Types, IdentifierValidationRejectsEverythingElse) {
  REP_CHECK(!rep::is_valid_identifier(""));
  REP_CHECK(!rep::is_valid_identifier(std::string(rep::kMaxIdentifierLength + 1, 'x')));
  REP_CHECK(!rep::is_valid_identifier("a b"));
  REP_CHECK(!rep::is_valid_identifier(" leading"));
  REP_CHECK(!rep::is_valid_identifier("trailing "));
  REP_CHECK(!rep::is_valid_identifier("a=b"));
  REP_CHECK(!rep::is_valid_identifier("a:b"));
  REP_CHECK(!rep::is_valid_identifier("a,b"));
  REP_CHECK(!rep::is_valid_identifier("a#b"));
  REP_CHECK(!rep::is_valid_identifier("a\tb"));
  REP_CHECK(!rep::is_valid_identifier("a\nb"));
  REP_CHECK(!rep::is_valid_identifier("a\rb"));
  REP_CHECK(!rep::is_valid_identifier(std::string("a\0b", 3)));
  REP_CHECK(!rep::is_valid_identifier("\x7f"));
  // A non-ASCII byte is rejected, never folded or normalised.
  REP_CHECK(!rep::is_valid_identifier("caf\xc3\xa9"));
  REP_CHECK(!rep::is_valid_identifier("\xff"));
  REP_CHECK(!rep::is_valid_identifier("a!b"));
  REP_CHECK(!rep::is_valid_identifier("a*b"));
  REP_CHECK(!rep::is_valid_identifier("a?b"));
  REP_CHECK(!rep::is_valid_identifier("a[b]"));
  REP_CHECK(!rep::is_valid_identifier("a{b}"));
}

REP_TEST(Types, IdentifierProblemNamesTheOffendingOffset) {
  REP_CHECK_EQ(id_problem(""), std::string("identifier must not be empty"));
  REP_CHECK_EQ(id_problem(std::string(129, 'x')),
               std::string("identifier exceeds 128 characters"));
  const std::string bad = "rack 1";
  const std::string described = id_problem(bad);
  REP_CHECK_MSG(described.find("offset 4") != std::string::npos, described);
  REP_CHECK_MSG(described.find("unsupported character") != std::string::npos, described);

  // The offset is zero based and counts bytes, not characters that were
  // folded away.
  const std::string first_bad = " rack";
  const std::string first_described = id_problem(first_bad);
  REP_CHECK_MSG(first_described.find("offset 0") != std::string::npos, first_described);

  const std::string colon = "a:b";
  const std::string colon_described = id_problem(colon);
  REP_CHECK_MSG(colon_described.find("offset 1") != std::string::npos, colon_described);

  const std::string embedded_nul("a\0b", 3);
  const std::string nul_described = id_problem(embedded_nul);
  REP_CHECK_MSG(nul_described.find("offset 1") != std::string::npos, nul_described);
}

REP_TEST(Types, BasicIdParseRoundTripAndTypeSeparation) {
  // Every identifier type round trips through its own parse(), and the types
  // are distinct at compile time, so a rack id can never be handed to an
  // obligation-id parameter.
  static_assert(!std::is_same_v<rep::RackId, rep::ObligationId>);
  static_assert(!std::is_same_v<rep::RackId, rep::DestinationId>);
  static_assert(!std::is_same_v<rep::StreamId, rep::AuthorityId>);
  static_assert(!std::is_same_v<rep::PlanId, rep::LineageId>);
  static_assert(!std::is_same_v<rep::Generation, rep::Epoch>);
  static_assert(!std::is_same_v<rep::Generation, rep::Sequence>);
  static_assert(!std::is_same_v<rep::PlanRevision, rep::Generation>);
  static_assert(!std::is_same_v<rep::UnixNanos, rep::Generation>);

  check_id_round_trip<rep::RackId>("rack-a");
  check_id_round_trip<rep::ObligationId>("ob-1");
  check_id_round_trip<rep::CandidateId>("cand-7");
  check_id_round_trip<rep::StreamId>("composition");
  check_id_round_trip<rep::AuthorityId>("rack-authority@example");
  check_id_round_trip<rep::DestinationId>("slot-3");
  check_id_round_trip<rep::PlannerId>("planner-0");
  check_id_round_trip<rep::PlanId>("plan-42");
  check_id_round_trip<rep::LineageId>("lineage-1");
  check_id_round_trip<rep::IdempotencyKey>("idem/key-1");
  check_id_round_trip<rep::FailureDomainId>("domain-2");
  check_id_round_trip<rep::WriterId>("writer-1");

  // The same text in two id types stays comparable within each type, and the
  // values only meet through their text.
  const rep::RackId rack = rep::RackId("shared-name");
  const rep::ObligationId obligation = rep::ObligationId("shared-name");
  REP_CHECK(rack == rep::RackId("shared-name"));
  REP_CHECK(obligation == rep::ObligationId("shared-name"));
  REP_CHECK(rack != rep::RackId("other-name"));
  REP_CHECK(rack < rep::RackId("z"));
  REP_CHECK(rep::ObligationId("a") < obligation);
  REP_CHECK_EQ(rack.str(), obligation.str());

  // The default value is empty and is not a valid parse result.
  const rep::RackId defaulted;
  REP_CHECK(defaulted.empty());
  REP_CHECK(!rep::is_valid_identifier(defaulted.view()));

  // A rejected parse reports the identifier error and the described problem.
  const auto bad = rep::ObligationId::parse("bad id");
  REP_REQUIRE(!bad.ok());
  REP_CHECK_EQ(bad.error().code, rep::ErrorCode::InvalidIdentifier);
  REP_CHECK_EQ(bad.error().message, id_problem("bad id"));

  const auto too_long = rep::RackId::parse(std::string(200, 'r'));
  REP_REQUIRE(!too_long.ok());
  REP_CHECK_EQ(too_long.error().code, rep::ErrorCode::InvalidIdentifier);

  const auto empty_parse = rep::RackId::parse("");
  REP_REQUIRE(!empty_parse.ok());
  REP_CHECK_EQ(empty_parse.error().code, rep::ErrorCode::InvalidIdentifier);
}

REP_TEST(Types, CheckedAddUnsignedAtTheBoundary) {
  constexpr std::uint64_t zero = 0;
  constexpr std::uint64_t one = 1;
  constexpr std::uint64_t two = 2;
  REP_CHECK_EQ(rep::checked_add(zero, zero), std::optional<std::uint64_t>(0));
  REP_CHECK_EQ(rep::checked_add(zero, kU64Max), std::optional<std::uint64_t>(kU64Max));
  REP_CHECK_EQ(rep::checked_add(kU64Max, zero), std::optional<std::uint64_t>(kU64Max));
  REP_CHECK_EQ(rep::checked_add(kU64Max - one, one), std::optional<std::uint64_t>(kU64Max));
  REP_CHECK_EQ(rep::checked_add(one, kU64Max - one), std::optional<std::uint64_t>(kU64Max));
  REP_CHECK(!rep::checked_add(kU64Max, one).has_value());
  REP_CHECK(!rep::checked_add(one, kU64Max).has_value());
  REP_CHECK(!rep::checked_add(kU64Max, kU64Max).has_value());
  REP_CHECK(!rep::checked_add(kU64Max - one, two).has_value());

  // Stepping up to the maximum one at a time never wraps.
  std::uint64_t value = kU64Max - 3;
  for (int step = 0; step < 3; ++step) {
    const auto next = rep::checked_add(value, one);
    REP_REQUIRE(next.has_value());
    value = *next;
  }
  REP_CHECK_EQ(value, kU64Max);
  REP_CHECK(!rep::checked_add(value, one).has_value());
  REP_CHECK_EQ(value, kU64Max);
}

REP_TEST(Types, CheckedMulUnsignedAtTheBoundary) {
  constexpr std::uint64_t zero = 0;
  constexpr std::uint64_t one = 1;
  constexpr std::uint64_t two = 2;
  REP_CHECK_EQ(rep::checked_mul(zero, zero), std::optional<std::uint64_t>(0));
  REP_CHECK_EQ(rep::checked_mul(kU64Max, zero), std::optional<std::uint64_t>(0));
  REP_CHECK_EQ(rep::checked_mul(zero, kU64Max), std::optional<std::uint64_t>(0));
  REP_CHECK_EQ(rep::checked_mul(kU64Max, one), std::optional<std::uint64_t>(kU64Max));
  REP_CHECK_EQ(rep::checked_mul(kU64Max - one, one), std::optional<std::uint64_t>(kU64Max - 1));
  REP_CHECK(!rep::checked_mul(kU64Max, two).has_value());
  REP_CHECK(!rep::checked_mul(two, kU64Max).has_value());
  REP_CHECK(!rep::checked_mul(kU64Max, kU64Max).has_value());

  const std::uint64_t two_to_32 = std::uint64_t{1} << 32;
  REP_CHECK_EQ(rep::checked_mul(two_to_32, two_to_32 - 1),
               std::optional<std::uint64_t>(kU64Max - (two_to_32 - 1)));
  REP_CHECK(!rep::checked_mul(two_to_32, two_to_32).has_value());
  REP_CHECK_EQ(rep::checked_mul(std::uint64_t{1} << 31, two),
               std::optional<std::uint64_t>(std::uint64_t{1} << 32));
}

REP_TEST(Types, CheckedSubUnsignedAtTheBoundary) {
  constexpr std::uint64_t zero = 0;
  constexpr std::uint64_t one = 1;
  REP_CHECK_EQ(rep::checked_sub(zero, zero), std::optional<std::uint64_t>(0));
  REP_CHECK_EQ(rep::checked_sub(kU64Max, kU64Max), std::optional<std::uint64_t>(0));
  REP_CHECK_EQ(rep::checked_sub(kU64Max, zero), std::optional<std::uint64_t>(kU64Max));
  REP_CHECK_EQ(rep::checked_sub(kU64Max, kU64Max - one), std::optional<std::uint64_t>(1));
  REP_CHECK_EQ(rep::checked_sub(one, one), std::optional<std::uint64_t>(0));
  REP_CHECK(!rep::checked_sub(zero, one).has_value());
  REP_CHECK(!rep::checked_sub(one, kU64Max).has_value());
  REP_CHECK(!rep::checked_sub(kU64Max - one, kU64Max).has_value());
  REP_CHECK(!rep::checked_sub(zero, kU64Max).has_value());
}

REP_TEST(Types, CheckedAddSignedAtBothLimits) {
  REP_CHECK_EQ(rep::checked_add(std::int64_t{0}, std::int64_t{0}),
               std::optional<std::int64_t>(0));
  REP_CHECK_EQ(rep::checked_add(kI64Max, std::int64_t{0}), std::optional<std::int64_t>(kI64Max));
  REP_CHECK_EQ(rep::checked_add(kI64Min, std::int64_t{0}), std::optional<std::int64_t>(kI64Min));
  REP_CHECK_EQ(rep::checked_add(kI64Max - 1, std::int64_t{1}),
               std::optional<std::int64_t>(kI64Max));
  REP_CHECK_EQ(rep::checked_add(kI64Min + 1, std::int64_t{-1}),
               std::optional<std::int64_t>(kI64Min));
  REP_CHECK(!rep::checked_add(kI64Max, std::int64_t{1}).has_value());
  REP_CHECK(!rep::checked_add(std::int64_t{1}, kI64Max).has_value());
  REP_CHECK(!rep::checked_add(kI64Min, std::int64_t{-1}).has_value());
  REP_CHECK(!rep::checked_add(std::int64_t{-1}, kI64Min).has_value());
  REP_CHECK(!rep::checked_add(kI64Max, kI64Max).has_value());
  REP_CHECK(!rep::checked_add(kI64Min, kI64Min).has_value());
  // These two are representable and must not be refused.
  REP_CHECK_EQ(rep::checked_add(kI64Min, kI64Max), std::optional<std::int64_t>(-1));
  REP_CHECK_EQ(rep::checked_add(kI64Max, kI64Min), std::optional<std::int64_t>(-1));
  REP_CHECK_EQ(rep::checked_add(std::int64_t{1}, std::int64_t{-1}),
               std::optional<std::int64_t>(0));

  // Walking to the limit one step at a time stays exact.
  std::int64_t value = kI64Max - 2;
  for (int step = 0; step < 2; ++step) {
    const auto next = rep::checked_add(value, 1);
    REP_REQUIRE(next.has_value());
    value = *next;
  }
  REP_CHECK_EQ(value, kI64Max);
  REP_CHECK(!rep::checked_add(value, 1).has_value());
}

REP_TEST(Types, CheckedIncrementCounters) {
  REP_CHECK_EQ(rep::checked_increment(rep::Generation(0)),
               std::optional<rep::Generation>(rep::Generation(1)));
  REP_CHECK_EQ(rep::checked_increment(rep::Generation(kU64Max - 1)),
               std::optional<rep::Generation>(rep::Generation(kU64Max)));
  REP_CHECK(!rep::checked_increment(rep::Generation(kU64Max)).has_value());

  REP_CHECK_EQ(rep::checked_increment(rep::Epoch(7)),
               std::optional<rep::Epoch>(rep::Epoch(8)));
  REP_CHECK_EQ(rep::checked_increment(rep::Epoch(kU64Max - 1)),
               std::optional<rep::Epoch>(rep::Epoch(kU64Max)));
  REP_CHECK(!rep::checked_increment(rep::Epoch(kU64Max)).has_value());

  REP_CHECK_EQ(rep::checked_increment(rep::Sequence(0)),
               std::optional<rep::Sequence>(rep::Sequence(1)));
  REP_CHECK_EQ(rep::checked_increment(rep::Sequence(kU64Max - 1)),
               std::optional<rep::Sequence>(rep::Sequence(kU64Max)));
  REP_CHECK(!rep::checked_increment(rep::Sequence(kU64Max)).has_value());

  REP_CHECK_EQ(rep::checked_increment(rep::PlanRevision(0)),
               std::optional<rep::PlanRevision>(rep::PlanRevision(1)));
  REP_CHECK_EQ(rep::checked_increment(rep::PlanRevision(kU32Max - 1)),
               std::optional<rep::PlanRevision>(rep::PlanRevision(kU32Max)));
  REP_CHECK(!rep::checked_increment(rep::PlanRevision(kU32Max)).has_value());

  // The default value is zero, and incrementing it is the first revision.
  const rep::Generation fresh;
  REP_CHECK_EQ(fresh.value(), std::uint64_t{0});
  REP_CHECK_EQ(rep::checked_increment(fresh),
               std::optional<rep::Generation>(rep::Generation(1)));

  // Counters of different types never compare or convert into each other; the
  // static assertions in the id test pin the type separation.
  REP_CHECK(rep::Generation(5) == rep::Generation(5));
  REP_CHECK(rep::Generation(4) < rep::Generation(5));
  REP_CHECK(rep::PlanRevision(4) < rep::PlanRevision(5));
  REP_CHECK(rep::UnixNanos(-5) < rep::UnixNanos(5));
}

REP_TEST(Types, ResourceVectorCanonicalForm) {
  const auto made = rep::ResourceVector::make({{rep::ResourceClass::MemoryBytes, 5},
                                               {rep::ResourceClass::CpuMillicores, 0},
                                               {rep::ResourceClass::StorageBytes, 0}});
  REP_REQUIRE(made.ok());
  // Explicit zeroes are dropped: a zero demand is indistinguishable from an
  // absent entry.
  REP_CHECK_EQ(made.value().entries().size(), std::size_t{1});
  REP_CHECK_EQ(made.value().get(rep::ResourceClass::MemoryBytes), std::uint64_t{5});
  REP_CHECK_EQ(made.value().get(rep::ResourceClass::CpuMillicores), std::uint64_t{0});
  REP_CHECK_EQ(made.value().get(rep::ResourceClass::StorageBytes), std::uint64_t{0});
  REP_CHECK_EQ(made.value().get(rep::ResourceClass::RackUnits), std::uint64_t{0});
  REP_CHECK_EQ(made.value().to_text(), std::string("memory_bytes:5"));

  // Sorted by class code, whatever order the entries arrived in.
  const auto sorted = rep::ResourceVector::make({{rep::ResourceClass::RackUnits, 1},
                                                 {rep::ResourceClass::CpuMillicores, 2},
                                                 {rep::ResourceClass::MemoryBytes, 3}});
  REP_REQUIRE(sorted.ok());
  const auto entries = sorted.value().entries();
  REP_REQUIRE(entries.size() == 3);
  REP_CHECK_EQ(entries[0].resource, rep::ResourceClass::CpuMillicores);
  REP_CHECK_EQ(entries[1].resource, rep::ResourceClass::MemoryBytes);
  REP_CHECK_EQ(entries[2].resource, rep::ResourceClass::RackUnits);
  REP_CHECK_EQ(sorted.value().to_text(), std::string("cpu_millicores:2;memory_bytes:3;rack_units:1"));

  // The canonical encoding is the entry count followed by the sorted entries.
  rep::CanonicalWriter writer("rep.test.rv.v1");
  sorted.value().encode(writer);
  REP_CHECK_EQ(writer.to_hex(),
               std::string("0300000000000000" "0100" "0200000000000000" "0200"
                           "0300000000000000" "0700" "0100000000000000"));

  // Two orders of the same entries make the same value.
  const auto reversed = rep::ResourceVector::make({{rep::ResourceClass::RackUnits, 1},
                                                   {rep::ResourceClass::MemoryBytes, 3},
                                                   {rep::ResourceClass::CpuMillicores, 2}});
  REP_REQUIRE(reversed.ok());
  REP_CHECK(reversed.value() == sorted.value());

  // A repeated class is a contradiction, not a sum.
  const auto duplicate = rep::ResourceVector::make(
      {{rep::ResourceClass::CpuMillicores, 1}, {rep::ResourceClass::CpuMillicores, 2}});
  REP_REQUIRE(!duplicate.ok());
  REP_CHECK_EQ(duplicate.error().code, rep::ErrorCode::DuplicateIdentity);

  const auto empty = rep::ResourceVector::make({});
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().empty());
  REP_CHECK(empty.value().entries().empty());
  REP_CHECK_EQ(empty.value().to_text(), std::string(""));
  REP_CHECK_EQ(empty.value().get(rep::ResourceClass::CpuMillicores), std::uint64_t{0});
}

REP_TEST(Types, ResourceVectorParseAndTextRoundTrip) {
  const auto parsed = rep::ResourceVector::parse("cpu_millicores:3;memory_bytes:1024");
  REP_REQUIRE(parsed.ok());
  REP_CHECK_EQ(parsed.value().to_text(), std::string("cpu_millicores:3;memory_bytes:1024"));
  const auto again = rep::ResourceVector::parse(parsed.value().to_text());
  REP_REQUIRE(again.ok());
  REP_CHECK(again.value() == parsed.value());

  // The canonical text form is independent of the input order.
  const auto shuffled = rep::ResourceVector::parse("memory_bytes:1024;cpu_millicores:3");
  REP_REQUIRE(shuffled.ok());
  REP_CHECK(shuffled.value() == parsed.value());

  // The empty string is the empty vector, and it round trips.
  const auto empty = rep::ResourceVector::parse("");
  REP_REQUIRE(empty.ok());
  REP_CHECK(empty.value().empty());
  REP_CHECK_EQ(empty.value().to_text(), std::string(""));
  const auto empty_again = rep::ResourceVector::parse(empty.value().to_text());
  REP_REQUIRE(empty_again.ok());
  REP_CHECK(empty_again.value() == empty.value());

  // The extreme amount survives the text form.
  const std::uint64_t biggest = (std::numeric_limits<std::uint64_t>::max)();
  const auto huge = rep::ResourceVector::parse("rack_units:" + std::to_string(biggest));
  REP_REQUIRE(huge.ok());
  REP_CHECK_EQ(huge.value().get(rep::ResourceClass::RackUnits), biggest);
  REP_CHECK_EQ(huge.value().to_text(), std::string("rack_units:") + std::to_string(biggest));

  const char* const malformed[] = {"cpu_millicores", ":5", "cpu_millicores:", "bogus:1",
                                   "cpu_millicores:abc", "cpu_millicores:-1",
                                   "cpu_millicores:1;", "cpu_millicores:1;;memory_bytes:2",
                                   "cpu_millicores:18446744073709551616"};
  for (const char* const text : malformed) {
    const auto bad = rep::ResourceVector::parse(text);
    REP_CHECK_MSG(!bad.ok(), std::string("expected '") + text + "' to be refused");
    if (!bad.ok()) {
      REP_CHECK_MSG(bad.error().code == rep::ErrorCode::InvalidEncoding ||
                        bad.error().code == rep::ErrorCode::DuplicateIdentity,
                    std::string("unexpected error code for '") + text + "'");
    }
  }

  const auto repeated = rep::ResourceVector::parse("cpu_millicores:1;cpu_millicores:2");
  REP_REQUIRE(!repeated.ok());
  REP_CHECK_EQ(repeated.error().code, rep::ErrorCode::DuplicateIdentity);

  // A zero written explicitly is accepted and normalised away.
  const auto zero = rep::ResourceVector::parse("cpu_millicores:0");
  REP_REQUIRE(zero.ok());
  REP_CHECK(zero.value().empty());
  REP_CHECK_EQ(zero.value().to_text(), std::string(""));
}

REP_TEST(Types, ResourceVectorDominance) {
  const auto supply = rep::ResourceVector::make(
      {{rep::ResourceClass::CpuMillicores, 4}, {rep::ResourceClass::MemoryBytes, 100}});
  REP_REQUIRE(supply.ok());
  const auto demand = rep::ResourceVector::make(
      {{rep::ResourceClass::CpuMillicores, 4}, {rep::ResourceClass::MemoryBytes, 100}});
  REP_REQUIRE(demand.ok());

  REP_CHECK(supply.value().dominates(demand.value()));
  REP_CHECK(supply.value().dominates(rep::ResourceVector()));

  const auto smaller = rep::ResourceVector::make(
      {{rep::ResourceClass::CpuMillicores, 3}, {rep::ResourceClass::MemoryBytes, 99}});
  REP_REQUIRE(smaller.ok());
  REP_CHECK(supply.value().dominates(smaller.value()));
  REP_CHECK(!smaller.value().dominates(supply.value()));

  const auto over_cpu = rep::ResourceVector::make({{rep::ResourceClass::CpuMillicores, 5}});
  REP_REQUIRE(over_cpu.ok());
  REP_CHECK(!supply.value().dominates(over_cpu.value()));

  // A class the supply never mentions is a zero supply, not an unlimited one.
  const auto absent = rep::ResourceVector::make({{rep::ResourceClass::AcceleratorUnits, 1}});
  REP_REQUIRE(absent.ok());
  REP_CHECK(!supply.value().dominates(absent.value()));
  REP_CHECK(!rep::ResourceVector().dominates(over_cpu.value()));
}

REP_TEST(Types, ResourceVectorCheckedSubtract) {
  const auto supply = rep::ResourceVector::make(
      {{rep::ResourceClass::CpuMillicores, 4}, {rep::ResourceClass::MemoryBytes, 100}});
  REP_REQUIRE(supply.ok());
  const auto used = rep::ResourceVector::make(
      {{rep::ResourceClass::CpuMillicores, 1}, {rep::ResourceClass::MemoryBytes, 40}});
  REP_REQUIRE(used.ok());
  const auto remainder = supply.value().subtract(used.value());
  REP_REQUIRE(remainder.ok());
  const auto expected = rep::ResourceVector::make(
      {{rep::ResourceClass::CpuMillicores, 3}, {rep::ResourceClass::MemoryBytes, 60}});
  REP_REQUIRE(expected.ok());
  REP_CHECK(remainder.value() == expected.value());
  REP_CHECK_EQ(remainder.value().to_text(), std::string("cpu_millicores:3;memory_bytes:60"));

  // Subtracting exactly what is available leaves nothing.
  const auto exhausted = supply.value().subtract(supply.value());
  REP_REQUIRE(exhausted.ok());
  REP_CHECK(exhausted.value().empty());

  // Subtracting nothing is the identity.
  const auto untouched = supply.value().subtract(rep::ResourceVector());
  REP_REQUIRE(untouched.ok());
  REP_CHECK(untouched.value() == supply.value());

  // Shortfall on a class that is present.
  const auto too_much_cpu = rep::ResourceVector::make({{rep::ResourceClass::CpuMillicores, 5}});
  REP_REQUIRE(too_much_cpu.ok());
  const auto short_cpu = supply.value().subtract(too_much_cpu.value());
  REP_REQUIRE(!short_cpu.ok());
  REP_CHECK_EQ(short_cpu.error().code, rep::ErrorCode::Underflow);

  // Shortfall on a class that is entirely absent from the supply: a zero
  // supply underflows rather than being ignored.
  const auto absent = rep::ResourceVector::make({{rep::ResourceClass::AcceleratorUnits, 1}});
  REP_REQUIRE(absent.ok());
  const auto short_absent = supply.value().subtract(absent.value());
  REP_REQUIRE(!short_absent.ok());
  REP_CHECK_EQ(short_absent.error().code, rep::ErrorCode::Underflow);
  REP_CHECK_MSG(short_absent.error().message.find("not present") != std::string::npos,
                short_absent.error().message);

  // The empty supply cannot cover any demand.
  const auto from_empty = rep::ResourceVector().subtract(used.value());
  REP_REQUIRE(!from_empty.ok());
  REP_CHECK_EQ(from_empty.error().code, rep::ErrorCode::Underflow);

  // A maximum-sized supply can be reduced but not below zero.
  const std::uint64_t biggest = (std::numeric_limits<std::uint64_t>::max)();
  const auto huge = rep::ResourceVector::make({{rep::ResourceClass::PowerWatts, biggest}});
  REP_REQUIRE(huge.ok());
  const auto one = rep::ResourceVector::make({{rep::ResourceClass::PowerWatts, 1}});
  REP_REQUIRE(one.ok());
  const auto reduced = huge.value().subtract(one.value());
  REP_REQUIRE(reduced.ok());
  REP_CHECK_EQ(reduced.value().get(rep::ResourceClass::PowerWatts), biggest - 1);
  const auto below_zero = huge.value().subtract(
      rep::ResourceVector::make({{rep::ResourceClass::PowerWatts, biggest}}).value());
  REP_REQUIRE(below_zero.ok());
  REP_CHECK(below_zero.value().empty());
}

REP_TEST(Types, ResourceVectorCheckedAdd) {
  const auto left = rep::ResourceVector::make(
      {{rep::ResourceClass::CpuMillicores, 2}, {rep::ResourceClass::MemoryBytes, 10}});
  REP_REQUIRE(left.ok());
  const auto right = rep::ResourceVector::make(
      {{rep::ResourceClass::MemoryBytes, 5}, {rep::ResourceClass::RackUnits, 1}});
  REP_REQUIRE(right.ok());
  const auto sum = left.value().add(right.value());
  REP_REQUIRE(sum.ok());
  const auto expected = rep::ResourceVector::make({{rep::ResourceClass::CpuMillicores, 2},
                                                   {rep::ResourceClass::MemoryBytes, 15},
                                                   {rep::ResourceClass::RackUnits, 1}});
  REP_REQUIRE(expected.ok());
  REP_CHECK(sum.value() == expected.value());
  REP_CHECK_EQ(sum.value().to_text(),
               std::string("cpu_millicores:2;memory_bytes:15;rack_units:1"));

  const std::uint64_t biggest = (std::numeric_limits<std::uint64_t>::max)();
  const auto near_max = rep::ResourceVector::make({{rep::ResourceClass::StorageBytes, biggest - 1}});
  REP_REQUIRE(near_max.ok());
  const auto one = rep::ResourceVector::make({{rep::ResourceClass::StorageBytes, 1}});
  REP_REQUIRE(one.ok());
  const auto exact = near_max.value().add(one.value());
  REP_REQUIRE(exact.ok());
  REP_CHECK_EQ(exact.value().get(rep::ResourceClass::StorageBytes), biggest);

  const auto full = rep::ResourceVector::make({{rep::ResourceClass::StorageBytes, biggest}});
  REP_REQUIRE(full.ok());
  const auto overflow = full.value().add(one.value());
  REP_REQUIRE(!overflow.ok());
  REP_CHECK_EQ(overflow.error().code, rep::ErrorCode::Overflow);

  // Adding a class the other side does not carry simply introduces it.
  const auto introduced = rep::ResourceVector().add(full.value());
  REP_REQUIRE(introduced.ok());
  REP_CHECK(introduced.value() == full.value());

  // Addition is commutative on the canonical form.
  const auto other_order = right.value().add(left.value());
  REP_REQUIRE(other_order.ok());
  REP_CHECK(other_order.value() == sum.value());
}

REP_TEST(Types, DestinationRefParseAndTextRoundTrip) {
  const auto rack_slot = rep::DestinationRef::parse("rack_slot:rack-1");
  REP_REQUIRE(rack_slot.ok());
  REP_CHECK_EQ(rack_slot.value().kind, rep::DestinationKind::RackSlot);
  REP_CHECK_EQ(rack_slot.value().id.str(), std::string("rack-1"));
  REP_CHECK_EQ(rack_slot.value().to_text(), std::string("rack_slot:rack-1"));

  const auto fabric = rep::DestinationRef::parse("fabric_endpoint:fc-9");
  REP_REQUIRE(fabric.ok());
  REP_CHECK_EQ(fabric.value().kind, rep::DestinationKind::FabricEndpoint);
  REP_CHECK_EQ(fabric.value().to_text(), std::string("fabric_endpoint:fc-9"));

  const auto storage = rep::DestinationRef::parse("storage_target:vol-2");
  REP_REQUIRE(storage.ok());
  REP_CHECK_EQ(storage.value().kind, rep::DestinationKind::StorageTarget);
  REP_CHECK_EQ(storage.value().to_text(), std::string("storage_target:vol-2"));

  const auto service = rep::DestinationRef::parse("service_endpoint:svc-3");
  REP_REQUIRE(service.ok());
  REP_CHECK_EQ(service.value().kind, rep::DestinationKind::ServiceEndpoint);
  REP_CHECK_EQ(service.value().to_text(), std::string("service_endpoint:svc-3"));

  // Every kind round trips through its text form.
  for (const rep::DestinationRef& reference :
       {rack_slot.value(), fabric.value(), storage.value(), service.value()}) {
    const auto again = rep::DestinationRef::parse(reference.to_text());
    REP_REQUIRE(again.ok());
    REP_CHECK(again.value() == reference);
  }

  // The kind participates in ordering and equality.
  REP_CHECK(rack_slot.value() != fabric.value());
  REP_CHECK(rack_slot.value() < fabric.value());
  const rep::DestinationRef same_id_other_kind{rep::DestinationKind::FabricEndpoint,
                                               rep::DestinationId("rack-1")};
  REP_CHECK(same_id_other_kind != rack_slot.value());

  // A default reference is the "any destination" scope: rack slot, empty id.
  const rep::DestinationRef any;
  REP_CHECK_EQ(any.kind, rep::DestinationKind::RackSlot);
  REP_CHECK(any.id.empty());

  const char* const malformed[] = {"rack-B", "bogus:x", "rack_slot:", "rack_slot", ":rack-1", ""};
  for (const char* const text : malformed) {
    const auto bad = rep::DestinationRef::parse(text);
    REP_CHECK_MSG(!bad.ok(), std::string("expected '") + text + "' to be refused");
  }
  const auto unknown_kind = rep::DestinationRef::parse("bogus:x");
  REP_REQUIRE(!unknown_kind.ok());
  REP_CHECK_EQ(unknown_kind.error().code, rep::ErrorCode::InvalidEncoding);
  const auto empty_id = rep::DestinationRef::parse("rack_slot:");
  REP_REQUIRE(!empty_id.ok());
  REP_CHECK_EQ(empty_id.error().code, rep::ErrorCode::InvalidEncoding);
  const auto missing_kind = rep::DestinationRef::parse("rack-B");
  REP_REQUIRE(!missing_kind.ok());
  REP_CHECK_EQ(missing_kind.error().code, rep::ErrorCode::InvalidEncoding);

  // A malformed id inside a well formed kind is reported by the identifier
  // rule, not by the destination rule.
  const auto bad_id = rep::DestinationRef::parse("rack_slot:bad id");
  REP_REQUIRE(!bad_id.ok());
  REP_CHECK_EQ(bad_id.error().code, rep::ErrorCode::InvalidIdentifier);

  // Stream references render as authority/stream and compare as a pair.
  const rep::StreamRef first{rep::AuthorityId("a-authority"), rep::StreamId("composition")};
  const rep::StreamRef second{rep::AuthorityId("a-authority"), rep::StreamId("enumeration")};
  const rep::StreamRef third{rep::AuthorityId("b-authority"), rep::StreamId("composition")};
  REP_CHECK_EQ(first.to_text(), std::string("a-authority/composition"));
  REP_CHECK(first != second);
  REP_CHECK(first < second);
  REP_CHECK(first < third);
  REP_CHECK(second < third);
}

REP_TEST(Types, EnumTextRoundTripObligationsAndActions) {
  check_enum_text("obligation_kind", kObligationKinds, &rep::ObligationKind_from_string, false);
  check_enum_text("action_kind", kActionKinds, &rep::ActionKind_from_string, false);
  check_enum_text("destination_kind", kDestinationKinds, &rep::DestinationKind_from_string, false);
  check_enum_text("policy_rule_kind", kPolicyRuleKinds, &rep::PolicyRuleKind_from_string, false);
  check_enum_text("isolation_kind", kIsolationKinds, &rep::IsolationKind_from_string, false);
  check_enum_text("fabric_obligation_kind", kFabricObligationKinds,
                  &rep::FabricObligationKind_from_string, false);
}

REP_TEST(Types, EnumTextRoundTripStateAndEvidence) {
  check_enum_text("evidence_kind", kEvidenceKinds, &rep::EvidenceKind_from_string, false);
  check_enum_text("resource_class", kResourceClasses, &rep::ResourceClass_from_string, false);
  check_enum_text("workload_lifecycle", kWorkloadLifecycles, &rep::WorkloadLifecycle_from_string,
                  true);
  check_enum_text("migration_capability", kMigrationCapabilities,
                  &rep::MigrationCapability_from_string, true);
  check_enum_text("storage_attachment", kStorageAttachments, &rep::StorageAttachment_from_string,
                  true);
  check_enum_text("incident_severity", kIncidentSeverities, &rep::IncidentSeverity_from_string,
                  false);
}

REP_TEST(Types, EnumTextRoundTripPlanAndReasons) {
  check_enum_text("plan_status", kPlanStatuses, &rep::PlanStatus_from_string, false);
  check_enum_text("safety_verdict", kSafetyVerdicts, &rep::SafetyVerdict_from_string, false);
  check_enum_text("staleness_kind", kStalenessKinds, &rep::StalenessKind_from_string, false);
  check_enum_text("outcome_kind", kOutcomeKinds, &rep::OutcomeKind_from_string, false);
  check_enum_text("residual_reason", kResidualReasons, &rep::ResidualReason_from_string, false);
  check_enum_text("rejection_reason", kRejectionReasons, &rep::RejectionReason_from_string, false);
  check_enum_text("indeterminacy_reason", kIndeterminacyReasons,
                  &rep::IndeterminacyReason_from_string, false);
  check_enum_text("scope_exclusion_reason", kScopeExclusionReasons,
                  &rep::ScopeExclusionReason_from_string, false);
}

REP_TEST(Types, EnumCodesAreDistinctAndStable) {
  // Codes are part of the serialized contract: every declared value of every
  // enumeration has a unique, non-zero code.
  const std::uint16_t obligation_codes[] = {1, 2, 3, 4, 5, 6};
  for (std::size_t i = 0; i < std::size(kObligationKinds); ++i) {
    REP_CHECK_EQ(static_cast<std::uint16_t>(kObligationKinds[i]), obligation_codes[i]);
  }
  const std::uint16_t action_codes[] = {1, 2, 3, 4, 5, 6, 7};
  for (std::size_t i = 0; i < std::size(kActionKinds); ++i) {
    REP_CHECK_EQ(static_cast<std::uint16_t>(kActionKinds[i]), action_codes[i]);
  }
  const std::uint16_t destination_codes[] = {1, 2, 3, 4};
  for (std::size_t i = 0; i < std::size(kDestinationKinds); ++i) {
    REP_CHECK_EQ(static_cast<std::uint16_t>(kDestinationKinds[i]), destination_codes[i]);
  }
  const std::uint16_t resource_codes[] = {1, 2, 3, 4, 5, 6, 7};
  for (std::size_t i = 0; i < std::size(kResourceClasses); ++i) {
    REP_CHECK_EQ(static_cast<std::uint16_t>(kResourceClasses[i]), resource_codes[i]);
  }
  const std::uint16_t evidence_codes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  for (std::size_t i = 0; i < std::size(kEvidenceKinds); ++i) {
    REP_CHECK_EQ(static_cast<std::uint16_t>(kEvidenceKinds[i]), evidence_codes[i]);
  }
  // PlanStatus is the one enumeration whose zero code is a real value.
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::PlanStatus::Indeterminate), std::uint16_t{0});
  REP_CHECK_EQ(rep::to_string(rep::PlanStatus::Indeterminate), std::string_view("indeterminate"));

  // Residual and rejection reasons keep their exact published codes.
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::ResidualReason::StateUnknown), std::uint16_t{14});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::ResidualReason::NoCandidate), std::uint16_t{1});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::RejectionReason::FailureDomainUnknown),
               std::uint16_t{20});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::RejectionReason::ObligationKindNotAllowed),
               std::uint16_t{1});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::IndeterminacyReason::LimitExceeded),
               std::uint16_t{13});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::ScopeExclusionReason::KindNotRequested),
               std::uint16_t{1});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::SafetyVerdict::Stale), std::uint16_t{4});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::StalenessKind::StreamMissing), std::uint16_t{4});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::OutcomeKind::RejectedPrecondition),
               std::uint16_t{6});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::IsolationKind::NetworkIsolation),
               std::uint16_t{4});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::IncidentSeverity::Critical), std::uint16_t{3});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::FabricObligationKind::NamespaceMapping),
               std::uint16_t{4});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::PolicyRuleKind::AllowProtectedMove),
               std::uint16_t{4});
  REP_CHECK_EQ(static_cast<std::uint16_t>(rep::WorkloadLifecycle::Unknown), std::uint16_t{5});

  // The error codes the planner reports are stable too.
  REP_CHECK_EQ(rep::to_string(rep::ErrorCode::Ok), std::string_view("ok"));
  REP_CHECK_EQ(rep::to_string(rep::ErrorCode::DigestMismatch), std::string_view("digest_mismatch"));
  REP_CHECK_EQ(rep::to_string(rep::ErrorCode::Underflow), std::string_view("underflow"));
  REP_CHECK_EQ(rep::to_string(rep::ErrorCode::Overflow), std::string_view("overflow"));
}

REP_TEST_MAIN()
