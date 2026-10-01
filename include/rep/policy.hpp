// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_POLICY_HPP
#define REP_POLICY_HPP

#include <cstdint>
#include <vector>

#include "rep/canonical.hpp"
#include "rep/export.hpp"
#include "rep/status.hpp"
#include "rep/types.hpp"

namespace rep {

// A placement-policy rule asserted by the policy authority.  The planner
// evaluates rules but never invents one: an empty rule set is a policy
// decision, not a planner default.
//
// Evaluation order (documented in the README and implemented once, in
// planner.cpp):
//   1. DenyAction   - any matching rule rejects the candidate.
//   2. AllowAction - if at least one rule exists for the obligation kind, the
//                    candidate must match one; otherwise the action is
//                    permitted.
//   3. RequireDomainSpread - any matching rule makes failure-domain spread
//                    mandatory for that obligation kind.
//   4. AllowProtectedMove   - a protected obligation needs a matching rule to
//                    be assignable at all.
struct REP_API PolicyRule {
  PolicyRuleKind kind{PolicyRuleKind::AllowAction};
  ObligationKind obligation_kind{ObligationKind::Workload};
  ActionKind action{ActionKind::LiveMigrate};
  DestinationKind destination_kind{DestinationKind::RackSlot};
  FailureDomainId domain;   // empty means "any domain"

  void encode(CanonicalWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(kind));
    writer.u16(static_cast<std::uint16_t>(obligation_kind));
    writer.u16(static_cast<std::uint16_t>(action));
    writer.u16(static_cast<std::uint16_t>(destination_kind));
    writer.text(domain.view());
  }
  friend bool operator==(const PolicyRule& a, const PolicyRule& b) noexcept {
    return a.kind == b.kind && a.obligation_kind == b.obligation_kind && a.action == b.action &&
           a.destination_kind == b.destination_kind && a.domain == b.domain;
  }
  friend auto operator<=>(const PolicyRule& a, const PolicyRule& b) noexcept {
    if (a.kind != b.kind) {
      return static_cast<std::uint16_t>(a.kind) <=> static_cast<std::uint16_t>(b.kind);
    }
    if (a.obligation_kind != b.obligation_kind) {
      return static_cast<std::uint16_t>(a.obligation_kind) <=>
             static_cast<std::uint16_t>(b.obligation_kind);
    }
    if (a.action != b.action) {
      return static_cast<std::uint16_t>(a.action) <=> static_cast<std::uint16_t>(b.action);
    }
    if (a.destination_kind != b.destination_kind) {
      return static_cast<std::uint16_t>(a.destination_kind) <=>
             static_cast<std::uint16_t>(b.destination_kind);
    }
    return a.domain <=> b.domain;
  }
};

struct REP_API DomainMember {
  FailureDomainId domain;
  RackId rack;
  DestinationRef destination;
  ObligationId obligation;
  bool is_rack{false};

  void encode(CanonicalWriter& writer) const {
    writer.text(domain.view());
    writer.boolean(is_rack);
    writer.text(rack.view());
    destination.encode(writer);
    writer.text(obligation.view());
  }
  friend bool operator==(const DomainMember& a, const DomainMember& b) noexcept {
    return a.domain == b.domain && a.rack == b.rack && a.destination == b.destination &&
           a.obligation == b.obligation && a.is_rack == b.is_rack;
  }
  friend auto operator<=>(const DomainMember& a, const DomainMember& b) noexcept {
    if (a.is_rack != b.is_rack) {
      return a.is_rack <=> b.is_rack;
    }
    if (a.rack != b.rack) {
      return a.rack <=> b.rack;
    }
    if (a.destination != b.destination) {
      return a.destination <=> b.destination;
    }
    if (a.obligation != b.obligation) {
      return a.obligation <=> b.obligation;
    }
    return a.domain <=> b.domain;
  }
};

// Failure-domain membership.  A member whose domain is unknown is simply
// absent: absence never implies a domain, so a spread requirement that cannot
// be evaluated rejects the candidate instead of assuming safety.
struct REP_API FailureDomainTopology {
  std::vector<DomainMember> members;   // canonical: sorted, unique

  [[nodiscard]] static Result<FailureDomainTopology> make(std::vector<DomainMember> members);
  [[nodiscard]] std::optional<FailureDomainId> domain_of_rack(const RackId& rack) const;
  [[nodiscard]] std::optional<FailureDomainId> domain_of_destination(
      const DestinationRef& destination) const;
  [[nodiscard]] std::optional<FailureDomainId> domain_of_obligation(
      const ObligationId& obligation) const;

  void encode(CanonicalWriter& writer) const { encode_sequence(writer, members); }
  friend bool operator==(const FailureDomainTopology& a,
                         const FailureDomainTopology& b) noexcept {
    return a.members == b.members;
  }
};

// A maintenance window scoped by failure domain and/or destination.  A window
// that permits evacuation opens an action; a window that does not permit
// evacuation freezes it.  All bounds are half-open [starts_at, ends_at).
struct REP_API MaintenanceWindow {
  FailureDomainId domain;             // empty means "any domain"
  DestinationRef destination;         // RackSlot with empty id means "any destination"
  ActionKind action{ActionKind::LiveMigrate};
  UnixNanos starts_at;
  UnixNanos ends_at;
  bool permits_evacuation{false};

  [[nodiscard]] bool covers_destination(const DestinationRef& candidate,
                                        const std::optional<FailureDomainId>& candidate_domain) const;
  [[nodiscard]] bool is_open_at(UnixNanos instant) const;

  void encode(CanonicalWriter& writer) const {
    writer.text(domain.view());
    destination.encode(writer);
    writer.u16(static_cast<std::uint16_t>(action));
    starts_at.encode(writer);
    ends_at.encode(writer);
    writer.boolean(permits_evacuation);
  }
  friend bool operator==(const MaintenanceWindow& a, const MaintenanceWindow& b) noexcept {
    return a.domain == b.domain && a.destination == b.destination && a.action == b.action &&
           a.starts_at == b.starts_at && a.ends_at == b.ends_at &&
           a.permits_evacuation == b.permits_evacuation;
  }
  friend auto operator<=>(const MaintenanceWindow& a, const MaintenanceWindow& b) noexcept {
    if (a.domain != b.domain) {
      return a.domain <=> b.domain;
    }
    if (a.destination != b.destination) {
      return a.destination <=> b.destination;
    }
    if (a.action != b.action) {
      return static_cast<std::uint16_t>(a.action) <=> static_cast<std::uint16_t>(b.action);
    }
    if (a.starts_at != b.starts_at) {
      return a.starts_at <=> b.starts_at;
    }
    return a.ends_at <=> b.ends_at;
  }
};

// An active incident.  An incident does not expire on its own: it stays
// authoritative until the maintenance authority publishes a generation without
// it.
struct REP_API Incident {
  FailureDomainId domain;
  IncidentSeverity severity{IncidentSeverity::Advisory};
  bool blocks_evacuation{true};

  void encode(CanonicalWriter& writer) const {
    writer.text(domain.view());
    writer.u16(static_cast<std::uint16_t>(severity));
    writer.boolean(blocks_evacuation);
  }
  friend bool operator==(const Incident& a, const Incident& b) noexcept {
    return a.domain == b.domain && a.severity == b.severity &&
           a.blocks_evacuation == b.blocks_evacuation;
  }
  friend auto operator<=>(const Incident& a, const Incident& b) noexcept {
    if (a.domain != b.domain) {
      return a.domain <=> b.domain;
    }
    if (a.severity != b.severity) {
      return static_cast<std::uint16_t>(a.severity) <=> static_cast<std::uint16_t>(b.severity);
    }
    return a.blocks_evacuation <=> b.blocks_evacuation;
  }
};

struct REP_API PlacementPolicyPayload {
  std::vector<PolicyRule> rules;   // canonical: sorted, unique

  [[nodiscard]] static Result<PlacementPolicyPayload> make(std::vector<PolicyRule> rules);
  void encode(CanonicalWriter& writer) const { encode_sequence(writer, rules); }
  friend bool operator==(const PlacementPolicyPayload& a,
                         const PlacementPolicyPayload& b) noexcept {
    return a.rules == b.rules;
  }
};

struct REP_API MaintenancePayload {
  std::vector<MaintenanceWindow> windows;
  std::vector<Incident> incidents;

  [[nodiscard]] static Result<MaintenancePayload> make(std::vector<MaintenanceWindow> windows,
                                                       std::vector<Incident> incidents);
  void encode(CanonicalWriter& writer) const {
    encode_sequence(writer, windows);
    encode_sequence(writer, incidents);
  }
  friend bool operator==(const MaintenancePayload& a, const MaintenancePayload& b) noexcept {
    return a.windows == b.windows && a.incidents == b.incidents;
  }
};

} // namespace rep

#endif // REP_POLICY_HPP
