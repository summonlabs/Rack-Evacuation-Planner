// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

#include "detail/text.hpp"

namespace rep {

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

bool is_valid_identifier(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxIdentifierLength) {
    return false;
  }
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    const bool alphanumeric = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                              (c >= 'a' && c <= 'z');
    const bool punctuation = c == '_' || c == '.' || c == '-' || c == '/' || c == '@' || c == '+';
    if (!alphanumeric && !punctuation) {
      return false;
    }
  }
  return true;
}

std::string describe_identifier_problem(std::string_view text) {
  if (text.empty()) {
    return "identifier must not be empty";
  }
  if (text.size() > kMaxIdentifierLength) {
    return "identifier exceeds " + std::to_string(kMaxIdentifierLength) + " characters";
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    const bool alphanumeric = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                              (c >= 'a' && c <= 'z');
    const bool punctuation = c == '_' || c == '.' || c == '-' || c == '/' || c == '@' || c == '+';
    if (!alphanumeric && !punctuation) {
      return "identifier contains an unsupported character at offset " + std::to_string(i) +
             " (allowed: A-Z a-z 0-9 _ . - / @ +)";
    }
  }
  return "identifier is not valid";
}

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------

std::optional<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) noexcept {
  if (a > (std::numeric_limits<std::uint64_t>::max)() - b) {
    return std::nullopt;
  }
  return a + b;
}

std::optional<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) noexcept {
  if (a != 0 && b > (std::numeric_limits<std::uint64_t>::max)() / a) {
    return std::nullopt;
  }
  return a * b;
}

std::optional<std::uint64_t> checked_sub(std::uint64_t a, std::uint64_t b) noexcept {
  if (b > a) {
    return std::nullopt;
  }
  return a - b;
}

std::optional<std::int64_t> checked_add(std::int64_t a, std::int64_t b) noexcept {
  constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
  constexpr std::int64_t kMin = (std::numeric_limits<std::int64_t>::min)();
  if (b > 0 && a > kMax - b) {
    return std::nullopt;
  }
  if (b < 0 && a < kMin - b) {
    return std::nullopt;
  }
  return a + b;
}

// ---------------------------------------------------------------------------
// Enumeration text
// ---------------------------------------------------------------------------
namespace {

template <class Enum>
struct EnumName {
  Enum value;
  std::string_view name;
};

template <class Enum, std::size_t N>
[[nodiscard]] std::string_view enum_name(const EnumName<Enum> (&table)[N], Enum value) noexcept {
  for (const EnumName<Enum>& entry : table) {
    if (entry.value == value) {
      return entry.name;
    }
  }
  return "unknown";
}

template <class Enum, std::size_t N>
[[nodiscard]] std::optional<Enum> enum_value(const EnumName<Enum> (&table)[N],
                                             std::string_view name) noexcept {
  for (const EnumName<Enum>& entry : table) {
    if (entry.name == name) {
      return entry.value;
    }
  }
  return std::nullopt;
}

constexpr EnumName<ObligationKind> kObligationKindNames[] = {
    {ObligationKind::Workload, "workload"},
    {ObligationKind::StorageReplica, "storage_replica"},
    {ObligationKind::NetworkPath, "network_path"},
    {ObligationKind::ServiceEndpoint, "service_endpoint"},
    {ObligationKind::Reservation, "reservation"},
    {ObligationKind::Appliance, "appliance"},
};

constexpr EnumName<ActionKind> kActionKindNames[] = {
    {ActionKind::LiveMigrate, "live_migrate"}, {ActionKind::ColdMigrate, "cold_migrate"},
    {ActionKind::Quiesce, "quiesce"},          {ActionKind::Detach, "detach"},
    {ActionKind::Rebind, "rebind"},            {ActionKind::Release, "release"},
    {ActionKind::Depower, "depower"},
};

constexpr EnumName<DestinationKind> kDestinationKindNames[] = {
    {DestinationKind::RackSlot, "rack_slot"},
    {DestinationKind::FabricEndpoint, "fabric_endpoint"},
    {DestinationKind::StorageTarget, "storage_target"},
    {DestinationKind::ServiceEndpoint, "service_endpoint"},
};

constexpr EnumName<ResourceClass> kResourceClassNames[] = {
    {ResourceClass::CpuMillicores, "cpu_millicores"},
    {ResourceClass::MemoryBytes, "memory_bytes"},
    {ResourceClass::StorageBytes, "storage_bytes"},
    {ResourceClass::AcceleratorUnits, "accelerator_units"},
    {ResourceClass::NetworkKib, "network_kib"},
    {ResourceClass::PowerWatts, "power_watts"},
    {ResourceClass::RackUnits, "rack_units"},
};

constexpr EnumName<IsolationKind> kIsolationKindNames[] = {
    {IsolationKind::Depower, "depower"},
    {IsolationKind::ThermalConstraint, "thermal_constraint"},
    {IsolationKind::PhysicalService, "physical_service"},
    {IsolationKind::NetworkIsolation, "network_isolation"},
};

constexpr EnumName<EvidenceKind> kEvidenceKindNames[] = {
    {EvidenceKind::RackComposition, "rack_composition"},
    {EvidenceKind::Enumeration, "enumeration"},
    {EvidenceKind::ObligationCatalog, "obligation_catalog"},
    {EvidenceKind::Capacity, "capacity"},
    {EvidenceKind::PlacementPolicy, "placement_policy"},
    {EvidenceKind::Maintenance, "maintenance"},
    {EvidenceKind::FailureDomain, "failure_domain"},
    {EvidenceKind::AsiWorkloadState, "asi_workload_state"},
    {EvidenceKind::DfiObligation, "dfi_obligation"},
    {EvidenceKind::CandidateOffers, "candidate_offers"},
};

constexpr EnumName<PolicyRuleKind> kPolicyRuleKindNames[] = {
    {PolicyRuleKind::AllowAction, "allow_action"},
    {PolicyRuleKind::DenyAction, "deny_action"},
    {PolicyRuleKind::RequireDomainSpread, "require_domain_spread"},
    {PolicyRuleKind::AllowProtectedMove, "allow_protected_move"},
};

constexpr EnumName<WorkloadLifecycle> kWorkloadLifecycleNames[] = {
    {WorkloadLifecycle::Running, "running"}, {WorkloadLifecycle::Paused, "paused"},
    {WorkloadLifecycle::Stopped, "stopped"}, {WorkloadLifecycle::Failed, "failed"},
    {WorkloadLifecycle::Unknown, "unknown"},
};

constexpr EnumName<MigrationCapability> kMigrationCapabilityNames[] = {
    {MigrationCapability::LiveAllowed, "live_allowed"},
    {MigrationCapability::ColdOnly, "cold_only"},
    {MigrationCapability::NotMigratable, "not_migratable"},
    {MigrationCapability::Unknown, "unknown"},
};

constexpr EnumName<StorageAttachment> kStorageAttachmentNames[] = {
    {StorageAttachment::Stateless, "stateless"},
    {StorageAttachment::LocalState, "local_state"},
    {StorageAttachment::SharedVolume, "shared_volume"},
    {StorageAttachment::Unknown, "unknown"},
};

constexpr EnumName<FabricObligationKind> kFabricObligationKindNames[] = {
    {FabricObligationKind::PathAttachment, "path_attachment"},
    {FabricObligationKind::ZoningEntry, "zoning_entry"},
    {FabricObligationKind::ReplicaLease, "replica_lease"},
    {FabricObligationKind::NamespaceMapping, "namespace_mapping"},
};

constexpr EnumName<IncidentSeverity> kIncidentSeverityNames[] = {
    {IncidentSeverity::Advisory, "advisory"},
    {IncidentSeverity::Degraded, "degraded"},
    {IncidentSeverity::Critical, "critical"},
};

constexpr EnumName<PlanStatus> kPlanStatusNames[] = {
    {PlanStatus::Indeterminate, "indeterminate"},
    {PlanStatus::EmptySafe, "empty_safe"},
    {PlanStatus::Complete, "complete"},
    {PlanStatus::Partial, "partial"},
};

constexpr EnumName<SafetyVerdict> kSafetyVerdictNames[] = {
    {SafetyVerdict::Safe, "safe"},
    {SafetyVerdict::NotProven, "not_proven"},
    {SafetyVerdict::Indeterminate, "indeterminate"},
    {SafetyVerdict::Stale, "stale"},
};

constexpr EnumName<StalenessKind> kStalenessKindNames[] = {
    {StalenessKind::GenerationMoved, "generation_moved"},
    {StalenessKind::EpochChanged, "epoch_changed"},
    {StalenessKind::DigestChanged, "digest_changed"},
    {StalenessKind::StreamMissing, "stream_missing"},
};

constexpr EnumName<OutcomeKind> kOutcomeKindNames[] = {
    {OutcomeKind::Planned, "planned"},
    {OutcomeKind::Replayed, "replayed"},
    {OutcomeKind::RejectedInput, "rejected_input"},
    {OutcomeKind::RejectedStaleEpoch, "rejected_stale_epoch"},
    {OutcomeKind::RejectedIdempotencyConflict, "rejected_idempotency_conflict"},
    {OutcomeKind::RejectedPrecondition, "rejected_precondition"},
};

constexpr EnumName<ResidualReason> kResidualReasonNames[] = {
    {ResidualReason::NoCandidate, "no_candidate"},
    {ResidualReason::CandidatesRejected, "candidates_rejected"},
    {ResidualReason::ProtectedObligation, "protected_obligation"},
    {ResidualReason::NonMigratable, "non_migratable"},
    {ResidualReason::DependencyUnsatisfied, "dependency_unsatisfied"},
    {ResidualReason::DependencyCycle, "dependency_cycle"},
    {ResidualReason::CapacityExhausted, "capacity_exhausted"},
    {ResidualReason::MaintenanceBlocked, "maintenance_blocked"},
    {ResidualReason::IncidentActive, "incident_active"},
    {ResidualReason::FailureDomainConflict, "failure_domain_conflict"},
    {ResidualReason::EvidenceMissing, "evidence_missing"},
    {ResidualReason::PolicyDenied, "policy_denied"},
    {ResidualReason::LimitExceeded, "limit_exceeded"},
    {ResidualReason::StateUnknown, "state_unknown"},
};

constexpr EnumName<RejectionReason> kRejectionReasonNames[] = {
    {RejectionReason::ObligationKindNotAllowed, "obligation_kind_not_allowed"},
    {RejectionReason::ActionDestinationMismatch, "action_destination_mismatch"},
    {RejectionReason::SameFailureDomain, "same_failure_domain"},
    {RejectionReason::CapacityInsufficient, "capacity_insufficient"},
    {RejectionReason::MaintenanceWindowRequired, "maintenance_window_required"},
    {RejectionReason::MaintenanceWindowClosed, "maintenance_window_closed"},
    {RejectionReason::IncidentActive, "incident_active"},
    {RejectionReason::PolicyDenied, "policy_denied"},
    {RejectionReason::ProtectedObligation, "protected_obligation"},
    {RejectionReason::NonMigratable, "non_migratable"},
    {RejectionReason::StaleGeneration, "stale_generation"},
    {RejectionReason::StaleEpoch, "stale_epoch"},
    {RejectionReason::AuthorityMismatch, "authority_mismatch"},
    {RejectionReason::DigestMismatch, "digest_mismatch"},
    {RejectionReason::DestinationNotPermitted, "destination_not_permitted"},
    {RejectionReason::SelfDestination, "self_destination"},
    {RejectionReason::DuplicateCandidate, "duplicate_candidate"},
    {RejectionReason::CapacityClaimMismatch, "capacity_claim_mismatch"},
    {RejectionReason::DestinationUnknown, "destination_unknown"},
    {RejectionReason::FailureDomainUnknown, "failure_domain_unknown"},
};

constexpr EnumName<IndeterminacyReason> kIndeterminacyReasonNames[] = {
    {IndeterminacyReason::RackCompositionMissing, "rack_composition_missing"},
    {IndeterminacyReason::RackCompositionRevisionMismatch, "rack_composition_revision_mismatch"},
    {IndeterminacyReason::RackCompositionConflict, "rack_composition_conflict"},
    {IndeterminacyReason::EnumerationMissing, "enumeration_missing"},
    {IndeterminacyReason::EnumerationIncomplete, "enumeration_incomplete"},
    {IndeterminacyReason::EnumerationRevisionMismatch, "enumeration_revision_mismatch"},
    {IndeterminacyReason::EnumerationSetMismatch, "enumeration_set_mismatch"},
    {IndeterminacyReason::EnumerationConflict, "enumeration_conflict"},
    {IndeterminacyReason::ObligationCatalogMissing, "obligation_catalog_missing"},
    {IndeterminacyReason::ObligationUndefined, "obligation_undefined"},
    {IndeterminacyReason::ObligationDefinitionDuplicate, "obligation_definition_duplicate"},
    {IndeterminacyReason::ObligationSourceRackMismatch, "obligation_source_rack_mismatch"},
    {IndeterminacyReason::LimitExceeded, "limit_exceeded"},
};

constexpr EnumName<ScopeExclusionReason> kScopeExclusionReasonNames[] = {
    {ScopeExclusionReason::KindNotRequested, "kind_not_requested"},
};

} // namespace

#define REP_DEFINE_ENUM_TEXT(Type, Table)                                        \
  std::string_view to_string(Type value) noexcept { return enum_name(Table, value); } \
  std::optional<Type> Type##_from_string(std::string_view text) noexcept {       \
    return enum_value(Table, text);                                              \
  }

REP_DEFINE_ENUM_TEXT(ObligationKind, kObligationKindNames)
REP_DEFINE_ENUM_TEXT(ActionKind, kActionKindNames)
REP_DEFINE_ENUM_TEXT(DestinationKind, kDestinationKindNames)
REP_DEFINE_ENUM_TEXT(ResourceClass, kResourceClassNames)
REP_DEFINE_ENUM_TEXT(IsolationKind, kIsolationKindNames)
REP_DEFINE_ENUM_TEXT(EvidenceKind, kEvidenceKindNames)
REP_DEFINE_ENUM_TEXT(PolicyRuleKind, kPolicyRuleKindNames)
REP_DEFINE_ENUM_TEXT(WorkloadLifecycle, kWorkloadLifecycleNames)
REP_DEFINE_ENUM_TEXT(MigrationCapability, kMigrationCapabilityNames)
REP_DEFINE_ENUM_TEXT(StorageAttachment, kStorageAttachmentNames)
REP_DEFINE_ENUM_TEXT(FabricObligationKind, kFabricObligationKindNames)
REP_DEFINE_ENUM_TEXT(IncidentSeverity, kIncidentSeverityNames)
REP_DEFINE_ENUM_TEXT(PlanStatus, kPlanStatusNames)
REP_DEFINE_ENUM_TEXT(SafetyVerdict, kSafetyVerdictNames)
REP_DEFINE_ENUM_TEXT(StalenessKind, kStalenessKindNames)
REP_DEFINE_ENUM_TEXT(OutcomeKind, kOutcomeKindNames)
REP_DEFINE_ENUM_TEXT(ResidualReason, kResidualReasonNames)
REP_DEFINE_ENUM_TEXT(RejectionReason, kRejectionReasonNames)
REP_DEFINE_ENUM_TEXT(IndeterminacyReason, kIndeterminacyReasonNames)
REP_DEFINE_ENUM_TEXT(ScopeExclusionReason, kScopeExclusionReasonNames)

#undef REP_DEFINE_ENUM_TEXT

// ---------------------------------------------------------------------------
// Resource vectors
// ---------------------------------------------------------------------------

Result<ResourceVector> ResourceVector::make(std::vector<ResourceAmount> entries) {
  std::vector<ResourceAmount> kept;
  kept.reserve(entries.size());
  for (const ResourceAmount& entry : entries) {
    if (entry.amount != 0) {
      kept.push_back(entry);
    }
  }
  std::sort(kept.begin(), kept.end(), [](const ResourceAmount& a, const ResourceAmount& b) {
    return static_cast<std::uint16_t>(a.resource) < static_cast<std::uint16_t>(b.resource);
  });
  for (std::size_t i = 1; i < kept.size(); ++i) {
    if (kept[i].resource == kept[i - 1].resource) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "resource vector repeats class " +
                            std::string(to_string(kept[i].resource)),
                        "resource");
    }
  }
  ResourceVector result;
  result.entries_ = std::move(kept);
  return result;
}

Result<ResourceVector> ResourceVector::parse(std::string_view text) {
  std::vector<ResourceAmount> entries;
  if (!text.empty()) {
    for (const std::string_view token : detail::split(text, ';')) {
      const std::size_t separator = token.find(':');
      if (separator == std::string_view::npos || separator == 0 || separator + 1 >= token.size()) {
        return make_error(ErrorCode::InvalidEncoding,
                          "resource token must be class:amount, got '" +
                              detail::escape_bytes(token) + "'",
                          "resource");
      }
      const auto resource = ResourceClass_from_string(token.substr(0, separator));
      if (!resource.has_value()) {
        return make_error(ErrorCode::InvalidEncoding,
                          "unknown resource class '" +
                              detail::escape_bytes(token.substr(0, separator)) + "'",
                          "resource");
      }
      std::uint64_t amount = 0;
      if (!detail::parse_u64(token.substr(separator + 1), amount)) {
        return make_error(ErrorCode::InvalidEncoding,
                          "resource amount must be an unsigned decimal, got '" +
                              detail::escape_bytes(token.substr(separator + 1)) + "'",
                          "resource");
      }
      entries.push_back(ResourceAmount{*resource, amount});
    }
  }
  return make(std::move(entries));
}

std::uint64_t ResourceVector::get(ResourceClass resource) const noexcept {
  for (const ResourceAmount& entry : entries_) {
    if (entry.resource == resource) {
      return entry.amount;
    }
  }
  return 0;
}

std::span<const ResourceAmount> ResourceVector::entries() const noexcept {
  return std::span<const ResourceAmount>(entries_.data(), entries_.size());
}

bool ResourceVector::dominates(const ResourceVector& demand) const noexcept {
  for (const ResourceAmount& entry : demand.entries_) {
    if (get(entry.resource) < entry.amount) {
      return false;
    }
  }
  return true;
}

Result<ResourceVector> ResourceVector::subtract(const ResourceVector& demand) const {
  std::vector<ResourceAmount> result;
  result.reserve(entries_.size());
  for (const ResourceAmount& entry : entries_) {
    const std::uint64_t used = demand.get(entry.resource);
    if (used > entry.amount) {
      return make_error(ErrorCode::Underflow,
                        "resource " + std::string(to_string(entry.resource)) +
                            " short by " + std::to_string(used - entry.amount),
                        "resource");
    }
    result.push_back(ResourceAmount{entry.resource, entry.amount - used});
  }
  // Every demanded class must exist in this vector; a class absent from the
  // supply vector is a zero supply.
  for (const ResourceAmount& entry : demand.entries_) {
    if (get(entry.resource) == 0) {
      return make_error(ErrorCode::Underflow,
                        "resource " + std::string(to_string(entry.resource)) +
                            " is not present in the supply vector",
                        "resource");
    }
  }
  return make(std::move(result));
}

Result<ResourceVector> ResourceVector::add(const ResourceVector& other) const {
  std::vector<ResourceAmount> result = entries_;
  for (const ResourceAmount& entry : other.entries_) {
    bool merged = false;
    for (ResourceAmount& existing : result) {
      if (existing.resource == entry.resource) {
        const auto sum = checked_add(existing.amount, entry.amount);
        if (!sum.has_value()) {
          return make_error(ErrorCode::Overflow,
                            "resource " + std::string(to_string(entry.resource)) +
                                " overflows 64 bits",
                            "resource");
        }
        existing.amount = *sum;
        merged = true;
        break;
      }
    }
    if (!merged) {
      result.push_back(entry);
    }
  }
  return make(std::move(result));
}

std::string ResourceVector::to_text() const {
  std::string result;
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (i != 0) {
      result.push_back(';');
    }
    result += to_string(entries_[i].resource);
    result.push_back(':');
    result += std::to_string(entries_[i].amount);
  }
  return result;
}

void ResourceVector::encode(CanonicalWriter& writer) const {
  writer.u64(static_cast<std::uint64_t>(entries_.size()));
  for (const ResourceAmount& entry : entries_) {
    entry.encode(writer);
  }
}

// ---------------------------------------------------------------------------
// Destination and stream references
// ---------------------------------------------------------------------------

std::string DestinationRef::to_text() const {
  return std::string(to_string(kind)) + ":" + id.str();
}

Result<DestinationRef> DestinationRef::parse(std::string_view text) {
  const std::size_t separator = text.find(':');
  if (separator == std::string_view::npos || separator == 0 || separator + 1 >= text.size()) {
    return make_error(ErrorCode::InvalidEncoding,
                      "destination must be kind:id, got '" + detail::escape_bytes(text) + "'",
                      "destination");
  }
  const auto kind = DestinationKind_from_string(text.substr(0, separator));
  if (!kind.has_value()) {
    return make_error(ErrorCode::InvalidEncoding,
                      "unknown destination kind '" +
                          detail::escape_bytes(text.substr(0, separator)) + "'",
                      "destination");
  }
  const auto id = DestinationId::parse(text.substr(separator + 1));
  if (!id.ok()) {
    return id.error();
  }
  return DestinationRef{*kind, id.value()};
}

std::string StreamRef::to_text() const {
  return authority.str() + "/" + stream.str();
}

} // namespace rep
