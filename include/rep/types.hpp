// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_TYPES_HPP
#define REP_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "rep/canonical.hpp"
#include "rep/digest.hpp"
#include "rep/export.hpp"
#include "rep/status.hpp"

namespace rep {

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxIdentifierLength = 128;

// Identifiers are deliberately restricted to a small, unambiguous, ASCII-only
// alphabet.  This keeps them safe in the canonical text format, keeps byte
// order equal to the order a human reads, and removes every locale question
// from comparison.  Anything outside the alphabet is rejected, never folded.
[[nodiscard]] REP_API bool is_valid_identifier(std::string_view text) noexcept;
[[nodiscard]] REP_API std::string describe_identifier_problem(std::string_view text);

template <class Tag>
class BasicId {
 public:
  BasicId() = default;
  explicit BasicId(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] static Result<BasicId> parse(std::string_view text) {
    if (!is_valid_identifier(text)) {
      return make_error(ErrorCode::InvalidIdentifier, describe_identifier_problem(text));
    }
    return BasicId(std::string(text));
  }

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

  [[nodiscard]] std::string str() const { return value_; }

  void encode(CanonicalWriter& writer) const { writer.text(value_); }

  friend bool operator==(const BasicId& a, const BasicId& b) noexcept {
    return a.value_ == b.value_;
  }
  friend auto operator<=>(const BasicId& a, const BasicId& b) noexcept {
    return a.value_.compare(b.value_) <=> 0;
  }

 private:
  std::string value_;
};

using RackId = BasicId<struct RackIdTag>;
using ObligationId = BasicId<struct ObligationIdTag>;
using CandidateId = BasicId<struct CandidateIdTag>;
using StreamId = BasicId<struct StreamIdTag>;
using AuthorityId = BasicId<struct AuthorityIdTag>;
using DestinationId = BasicId<struct DestinationIdTag>;
using PlannerId = BasicId<struct PlannerIdTag>;
using PlanId = BasicId<struct PlanIdTag>;
using LineageId = BasicId<struct LineageIdTag>;
using IdempotencyKey = BasicId<struct IdempotencyKeyTag>;
using FailureDomainId = BasicId<struct FailureDomainIdTag>;
using WriterId = BasicId<struct WriterIdTag>;

// ---------------------------------------------------------------------------
// Checked arithmetic.  Every externally influenced counter, capacity, and
// generation flows through these; they refuse overflow rather than wrapping.
// ---------------------------------------------------------------------------

[[nodiscard]] REP_API std::optional<std::uint64_t> checked_add(std::uint64_t a,
                                                               std::uint64_t b) noexcept;
[[nodiscard]] REP_API std::optional<std::uint64_t> checked_mul(std::uint64_t a,
                                                               std::uint64_t b) noexcept;
[[nodiscard]] REP_API std::optional<std::uint64_t> checked_sub(std::uint64_t a,
                                                               std::uint64_t b) noexcept;
[[nodiscard]] REP_API std::optional<std::int64_t> checked_add(std::int64_t a,
                                                              std::int64_t b) noexcept;

// ---------------------------------------------------------------------------
// Strong scalar types.  Generations, epochs, and sequences are all 64-bit
// counters, so they are distinct types: mixing them is a compile error.
// ---------------------------------------------------------------------------

template <class Tag, class T>
class StrongValue {
 public:
  using value_type = T;

  constexpr StrongValue() noexcept = default;
  constexpr explicit StrongValue(T value) noexcept : value_(value) {}

  [[nodiscard]] constexpr T value() const noexcept { return value_; }

  void encode(CanonicalWriter& writer) const {
    if constexpr (std::is_signed_v<T>) {
      writer.i64(static_cast<std::int64_t>(value_));
    } else {
      writer.u64(static_cast<std::uint64_t>(value_));
    }
  }

  friend constexpr bool operator==(StrongValue a, StrongValue b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr auto operator<=>(StrongValue a, StrongValue b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  T value_{};
};

template <class Tag, class T>
[[nodiscard]] constexpr std::optional<StrongValue<Tag, T>> checked_increment(
    StrongValue<Tag, T> value) noexcept {
  if (value.value() == (std::numeric_limits<T>::max)()) {
    return std::nullopt;
  }
  return StrongValue<Tag, T>(static_cast<T>(value.value() + 1));
}

using Generation = StrongValue<struct GenerationTag, std::uint64_t>;
using Epoch = StrongValue<struct EpochTag, std::uint64_t>;
using Sequence = StrongValue<struct SequenceTag, std::uint64_t>;
using PlanRevision = StrongValue<struct PlanRevisionTag, std::uint32_t>;
using UnixNanos = StrongValue<struct UnixNanosTag, std::int64_t>;

// ---------------------------------------------------------------------------
// Enumerations.  Codes are stable and serialized; names are the canonical
// text spellings used by the scenario format and by plan reports.
// ---------------------------------------------------------------------------

enum class ObligationKind : std::uint16_t {
  Workload = 1,
  StorageReplica = 2,
  NetworkPath = 3,
  ServiceEndpoint = 4,
  Reservation = 5,
  Appliance = 6,
};

enum class ActionKind : std::uint16_t {
  LiveMigrate = 1,
  ColdMigrate = 2,
  Quiesce = 3,
  Detach = 4,
  Rebind = 5,
  Release = 6,
  Depower = 7,
};

enum class DestinationKind : std::uint16_t {
  RackSlot = 1,
  FabricEndpoint = 2,
  StorageTarget = 3,
  ServiceEndpoint = 4,
};

enum class ResourceClass : std::uint16_t {
  CpuMillicores = 1,
  MemoryBytes = 2,
  StorageBytes = 3,
  AcceleratorUnits = 4,
  NetworkKib = 5,
  PowerWatts = 6,
  RackUnits = 7,
};

enum class IsolationKind : std::uint16_t {
  Depower = 1,
  ThermalConstraint = 2,
  PhysicalService = 3,
  NetworkIsolation = 4,
};

enum class EvidenceKind : std::uint16_t {
  RackComposition = 1,
  Enumeration = 2,
  ObligationCatalog = 3,
  Capacity = 4,
  PlacementPolicy = 5,
  Maintenance = 6,
  FailureDomain = 7,
  AsiWorkloadState = 8,
  DfiObligation = 9,
  CandidateOffers = 10,
};

enum class PolicyRuleKind : std::uint16_t {
  AllowAction = 1,
  DenyAction = 2,
  RequireDomainSpread = 3,
  AllowProtectedMove = 4,
};

enum class WorkloadLifecycle : std::uint16_t {
  Running = 1,
  Paused = 2,
  Stopped = 3,
  Failed = 4,
  Unknown = 5,
};

enum class MigrationCapability : std::uint16_t {
  LiveAllowed = 1,
  ColdOnly = 2,
  NotMigratable = 3,
  Unknown = 4,
};

enum class StorageAttachment : std::uint16_t {
  Stateless = 1,
  LocalState = 2,
  SharedVolume = 3,
  Unknown = 4,
};

enum class FabricObligationKind : std::uint16_t {
  PathAttachment = 1,
  ZoningEntry = 2,
  ReplicaLease = 3,
  NamespaceMapping = 4,
};

enum class IncidentSeverity : std::uint16_t {
  Advisory = 1,
  Degraded = 2,
  Critical = 3,
};

enum class PlanStatus : std::uint16_t {
  Indeterminate = 0,
  EmptySafe = 1,
  Complete = 2,
  Partial = 3,
};

enum class SafetyVerdict : std::uint16_t {
  Safe = 1,
  NotProven = 2,
  Indeterminate = 3,
  Stale = 4,
};

enum class StalenessKind : std::uint16_t {
  GenerationMoved = 1,
  EpochChanged = 2,
  DigestChanged = 3,
  StreamMissing = 4,
};

enum class OutcomeKind : std::uint16_t {
  Planned = 1,
  Replayed = 2,
  RejectedInput = 3,
  RejectedStaleEpoch = 4,
  RejectedIdempotencyConflict = 5,
  RejectedPrecondition = 6,
};

// Why an in-scope obligation could not be assigned.
enum class ResidualReason : std::uint16_t {
  NoCandidate = 1,
  CandidatesRejected = 2,
  ProtectedObligation = 3,
  NonMigratable = 4,
  DependencyUnsatisfied = 5,
  DependencyCycle = 6,
  CapacityExhausted = 7,
  MaintenanceBlocked = 8,
  IncidentActive = 9,
  FailureDomainConflict = 10,
  EvidenceMissing = 11,
  PolicyDenied = 12,
  LimitExceeded = 13,
  // The adjacent authority answered, and its answer is "unknown": the planner
  // cannot treat an explicit unknown as permission.
  StateUnknown = 14,
};

// Why one specific candidate was not eligible.
enum class RejectionReason : std::uint16_t {
  ObligationKindNotAllowed = 1,
  ActionDestinationMismatch = 2,
  SameFailureDomain = 3,
  CapacityInsufficient = 4,
  MaintenanceWindowRequired = 5,
  MaintenanceWindowClosed = 6,
  IncidentActive = 7,
  PolicyDenied = 8,
  ProtectedObligation = 9,
  NonMigratable = 10,
  StaleGeneration = 11,
  StaleEpoch = 12,
  AuthorityMismatch = 13,
  DigestMismatch = 14,
  DestinationNotPermitted = 15,
  SelfDestination = 16,
  DuplicateCandidate = 17,
  CapacityClaimMismatch = 18,
  DestinationUnknown = 19,
  FailureDomainUnknown = 20,
};

// Why the obligation set itself could not be established.  A contradiction
// inside the obligation-set layer is reported here, because the answer it
// destroys is the obligation set itself.
enum class IndeterminacyReason : std::uint16_t {
  RackCompositionMissing = 1,
  RackCompositionRevisionMismatch = 2,
  RackCompositionConflict = 3,
  EnumerationMissing = 4,
  EnumerationIncomplete = 5,
  EnumerationRevisionMismatch = 6,
  EnumerationSetMismatch = 7,
  EnumerationConflict = 8,
  ObligationCatalogMissing = 9,
  ObligationUndefined = 10,
  ObligationDefinitionDuplicate = 11,
  ObligationSourceRackMismatch = 12,
  LimitExceeded = 13,
};

enum class ScopeExclusionReason : std::uint16_t {
  KindNotRequested = 1,
};

#define REP_DECLARE_ENUM_TEXT(Type)                                                   \
  [[nodiscard]] REP_API std::string_view to_string(Type value) noexcept;              \
  [[nodiscard]] REP_API std::optional<Type> Type##_from_string(std::string_view text) noexcept

REP_DECLARE_ENUM_TEXT(ObligationKind);
REP_DECLARE_ENUM_TEXT(ActionKind);
REP_DECLARE_ENUM_TEXT(DestinationKind);
REP_DECLARE_ENUM_TEXT(ResourceClass);
REP_DECLARE_ENUM_TEXT(IsolationKind);
REP_DECLARE_ENUM_TEXT(EvidenceKind);
REP_DECLARE_ENUM_TEXT(PolicyRuleKind);
REP_DECLARE_ENUM_TEXT(WorkloadLifecycle);
REP_DECLARE_ENUM_TEXT(MigrationCapability);
REP_DECLARE_ENUM_TEXT(StorageAttachment);
REP_DECLARE_ENUM_TEXT(FabricObligationKind);
REP_DECLARE_ENUM_TEXT(IncidentSeverity);
REP_DECLARE_ENUM_TEXT(PlanStatus);
REP_DECLARE_ENUM_TEXT(SafetyVerdict);
REP_DECLARE_ENUM_TEXT(StalenessKind);
REP_DECLARE_ENUM_TEXT(OutcomeKind);
REP_DECLARE_ENUM_TEXT(ResidualReason);
REP_DECLARE_ENUM_TEXT(RejectionReason);
REP_DECLARE_ENUM_TEXT(IndeterminacyReason);
REP_DECLARE_ENUM_TEXT(ScopeExclusionReason);

#undef REP_DECLARE_ENUM_TEXT

// ---------------------------------------------------------------------------
// Resource vectors
// ---------------------------------------------------------------------------

struct ResourceAmount {
  ResourceClass resource{ResourceClass::CpuMillicores};
  std::uint64_t amount{0};

  void encode(CanonicalWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(resource));
    writer.u64(amount);
  }
  friend bool operator==(const ResourceAmount& a, const ResourceAmount& b) noexcept {
    return a.resource == b.resource && a.amount == b.amount;
  }
  friend auto operator<=>(const ResourceAmount& a, const ResourceAmount& b) noexcept {
    if (a.resource != b.resource) {
      return static_cast<std::uint16_t>(a.resource) <=> static_cast<std::uint16_t>(b.resource);
    }
    return a.amount <=> b.amount;
  }
};

// A sparse vector of resource amounts.  Canonical form is: entries sorted by
// resource class, unique, and without explicit zeroes (a zero demand is
// indistinguishable from an absent entry, by construction).
class REP_API ResourceVector {
 public:
  ResourceVector() = default;

  // Sorts, rejects duplicate classes, and drops zero entries.
  [[nodiscard]] static Result<ResourceVector> make(std::vector<ResourceAmount> entries);

  // Canonical text form: "class:amount;class:amount" (empty when unset).
  [[nodiscard]] static Result<ResourceVector> parse(std::string_view text);

  [[nodiscard]] std::uint64_t get(ResourceClass resource) const noexcept;
  [[nodiscard]] std::span<const ResourceAmount> entries() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

  // True when this vector can cover every entry of demand.
  [[nodiscard]] bool dominates(const ResourceVector& demand) const noexcept;

  // Checked subtraction; Underflow when any entry of demand is not covered.
  [[nodiscard]] Result<ResourceVector> subtract(const ResourceVector& demand) const;

  // Checked addition; Overflow when any resulting amount overflows 64 bits.
  [[nodiscard]] Result<ResourceVector> add(const ResourceVector& other) const;

  [[nodiscard]] std::string to_text() const;

  void encode(CanonicalWriter& writer) const;
  friend bool operator==(const ResourceVector& a, const ResourceVector& b) noexcept {
    return a.entries_ == b.entries_;
  }

 private:
  std::vector<ResourceAmount> entries_;
};

// A typed destination reference: the kind decides which adjacent authority is
// able to act, the id names the specific target.
struct DestinationRef {
  DestinationKind kind{DestinationKind::RackSlot};
  DestinationId id;

  void encode(CanonicalWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(kind));
    writer.text(id.view());
  }
  friend bool operator==(const DestinationRef& a, const DestinationRef& b) noexcept {
    return a.kind == b.kind && a.id == b.id;
  }
  friend auto operator<=>(const DestinationRef& a, const DestinationRef& b) noexcept {
    if (a.kind != b.kind) {
      return static_cast<std::uint16_t>(a.kind) <=> static_cast<std::uint16_t>(b.kind);
    }
    return a.id <=> b.id;
  }
  // Canonical text form "kind:id".
  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] static Result<DestinationRef> parse(std::string_view text);
};

// A stream is a (producing authority, stream name) pair.  This is the unit of
// generation binding: any move of a bound stream's generation makes a plan
// that consumed it detectably stale.
struct StreamRef {
  AuthorityId authority;
  StreamId stream;

  void encode(CanonicalWriter& writer) const {
    writer.text(authority.view());
    writer.text(stream.view());
  }
  friend bool operator==(const StreamRef& a, const StreamRef& b) noexcept {
    return a.authority == b.authority && a.stream == b.stream;
  }
  friend auto operator<=>(const StreamRef& a, const StreamRef& b) noexcept {
    if (a.authority != b.authority) {
      return a.authority <=> b.authority;
    }
    return a.stream <=> b.stream;
  }
  [[nodiscard]] std::string to_text() const;
};

} // namespace rep

#endif // REP_TYPES_HPP
