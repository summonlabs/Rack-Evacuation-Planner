// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_PLAN_HPP
#define REP_PLAN_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rep/canonical.hpp"
#include "rep/evidence.hpp"
#include "rep/export.hpp"
#include "rep/request.hpp"
#include "rep/status.hpp"
#include "rep/types.hpp"

namespace rep {

// One evidence stream the plan consumed, at the generation it consumed it.
struct REP_API PlanBinding {
  EvidenceKind kind{EvidenceKind::RackComposition};
  StreamRef stream;
  Generation generation;
  Epoch epoch;
  Digest digest;

  void encode(CanonicalWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(kind));
    stream.encode(writer);
    generation.encode(writer);
    epoch.encode(writer);
    writer.digest(digest);
  }
  friend bool operator==(const PlanBinding& a, const PlanBinding& b) noexcept {
    return a.kind == b.kind && a.stream == b.stream && a.generation == b.generation &&
           a.epoch == b.epoch && a.digest == b.digest;
  }
  friend auto operator<=>(const PlanBinding& a, const PlanBinding& b) noexcept {
    if (a.kind != b.kind) {
      return static_cast<std::uint16_t>(a.kind) <=> static_cast<std::uint16_t>(b.kind);
    }
    return a.stream <=> b.stream;
  }
};

// Why one specific candidate was not used.  Recorded per obligation so a
// rejected candidate always has a typed, deterministic explanation.
struct REP_API RejectedCandidate {
  CandidateId candidate;
  RejectionReason reason{RejectionReason::ObligationKindNotAllowed};

  void encode(CanonicalWriter& writer) const {
    writer.text(candidate.view());
    writer.u16(static_cast<std::uint16_t>(reason));
  }
  friend bool operator==(const RejectedCandidate& a, const RejectedCandidate& b) noexcept {
    return a.candidate == b.candidate && a.reason == b.reason;
  }
  friend auto operator<=>(const RejectedCandidate& a, const RejectedCandidate& b) noexcept {
    if (a.candidate != b.candidate) {
      return a.candidate <=> b.candidate;
    }
    return static_cast<std::uint16_t>(a.reason) <=> static_cast<std::uint16_t>(b.reason);
  }
};

// An in-scope obligation that this plan does not assign, with the typed reason
// and the candidates that were considered and rejected.
struct REP_API Residual {
  ObligationId obligation;
  ObligationKind kind{ObligationKind::Workload};
  ResidualReason reason{ResidualReason::NoCandidate};
  std::vector<RejectedCandidate> rejected;   // canonical: sorted, unique

  void encode(CanonicalWriter& writer) const {
    writer.text(obligation.view());
    writer.u16(static_cast<std::uint16_t>(kind));
    writer.u16(static_cast<std::uint16_t>(reason));
    encode_sequence(writer, rejected);
  }
  friend bool operator==(const Residual& a, const Residual& b) noexcept {
    return a.obligation == b.obligation && a.kind == b.kind && a.reason == b.reason &&
           a.rejected == b.rejected;
  }
  friend auto operator<=>(const Residual& a, const Residual& b) noexcept {
    return a.obligation <=> b.obligation;
  }
};

// An occupant that is proven outside the requested scope.
struct REP_API ScopeExclusion {
  ObligationId obligation;
  ObligationKind kind{ObligationKind::Workload};
  ScopeExclusionReason reason{ScopeExclusionReason::KindNotRequested};

  void encode(CanonicalWriter& writer) const {
    writer.text(obligation.view());
    writer.u16(static_cast<std::uint16_t>(kind));
    writer.u16(static_cast<std::uint16_t>(reason));
  }
  friend bool operator==(const ScopeExclusion& a, const ScopeExclusion& b) noexcept {
    return a.obligation == b.obligation && a.kind == b.kind && a.reason == b.reason;
  }
  friend auto operator<=>(const ScopeExclusion& a, const ScopeExclusion& b) noexcept {
    return a.obligation <=> b.obligation;
  }
};

// One obligation assigned to one candidate.  order_index is a dense 0..n-1
// position in the canonical evacuation order; wave is the dependency layer.
struct REP_API Assignment {
  ObligationId obligation;
  ObligationKind kind{ObligationKind::Workload};
  CandidateId candidate;
  ActionKind action{ActionKind::LiveMigrate};
  DestinationRef destination;
  AuthorityId authority;
  std::uint32_t wave{0};
  std::uint32_t order_index{0};

  void encode(CanonicalWriter& writer) const {
    writer.text(obligation.view());
    writer.u16(static_cast<std::uint16_t>(kind));
    writer.text(candidate.view());
    writer.u16(static_cast<std::uint16_t>(action));
    destination.encode(writer);
    writer.text(authority.view());
    writer.u32(wave);
    writer.u32(order_index);
  }
  friend bool operator==(const Assignment& a, const Assignment& b) noexcept {
    return a.obligation == b.obligation && a.kind == b.kind && a.candidate == b.candidate &&
           a.action == b.action && a.destination == b.destination && a.authority == b.authority &&
           a.wave == b.wave && a.order_index == b.order_index;
  }
  friend auto operator<=>(const Assignment& a, const Assignment& b) noexcept {
    if (a.order_index != b.order_index) {
      return a.order_index <=> b.order_index;
    }
    return a.obligation <=> b.obligation;
  }
};

// One bound stream whose evidence has moved since the plan was sealed.
struct REP_API StalenessFinding {
  StreamRef stream;
  StalenessKind kind{StalenessKind::GenerationMoved};
  Generation plan_generation;
  Generation current_generation;
  Epoch plan_epoch;
  Epoch current_epoch;

  void encode(CanonicalWriter& writer) const {
    stream.encode(writer);
    writer.u16(static_cast<std::uint16_t>(kind));
    plan_generation.encode(writer);
    current_generation.encode(writer);
    plan_epoch.encode(writer);
    current_epoch.encode(writer);
  }
  friend bool operator==(const StalenessFinding& a, const StalenessFinding& b) noexcept {
    return a.stream == b.stream && a.kind == b.kind &&
           a.plan_generation == b.plan_generation &&
           a.current_generation == b.current_generation && a.plan_epoch == b.plan_epoch &&
           a.current_epoch == b.current_epoch;
  }
  friend auto operator<=>(const StalenessFinding& a, const StalenessFinding& b) noexcept {
    if (a.stream != b.stream) {
      return a.stream <=> b.stream;
    }
    return static_cast<std::uint16_t>(a.kind) <=> static_cast<std::uint16_t>(b.kind);
  }
};

// The evacuation plan artifact.  A plan grants nothing and moves nothing: it
// is evidence that an evacuation is fully enumerated, ordered, and (when the
// status says so) resolved for one exact set of bound generations.
struct REP_API Plan {
  // ---- identity -----------------------------------------------------------
  PlanId id;
  PlannerId planner;
  PlanRevision revision;
  Sequence sequence;
  Epoch epoch;                 // planner incarnation that sealed the plan
  LineageId lineage;
  IdempotencyKey idempotency_key;
  Digest request_digest;

  // ---- binding ------------------------------------------------------------
  RackId source_rack;
  Generation composition_revision;
  IsolationKind isolation{IsolationKind::Depower};
  std::vector<ObligationKind> evacuate_kinds;   // canonical: sorted, unique
  std::vector<PlanBinding> bindings;            // canonical: sorted by stream

  // ---- content ------------------------------------------------------------
  PlanStatus status{PlanStatus::Indeterminate};
  std::vector<IndeterminacyReason> indeterminacy_reasons;   // canonical: sorted, unique
  std::vector<Assignment> assignments;                      // canonical: by order_index
  std::vector<Residual> residuals;                          // canonical: by obligation
  std::vector<ScopeExclusion> out_of_scope;                 // canonical: by obligation
  Digest plan_digest;

  // Content digest over every field above except plan_digest itself.
  [[nodiscard]] Digest content_digest() const;
  void encode_content(CanonicalWriter& writer) const;
  void encode(CanonicalWriter& writer) const;

  // Recomputes the digest and re-checks the structural invariants.
  [[nodiscard]] Result<void> verify() const;

  // Sets plan_digest and verifies the result.  Every plan the engine publishes
  // passes through this.
  [[nodiscard]] static Result<Plan> seal(Plan plan);

  // Bound evidence that has moved since this plan was sealed.  A non-empty
  // result means the plan is stale and proves nothing.
  [[nodiscard]] std::vector<StalenessFinding> staleness(const EvidenceBundle& current) const;
  [[nodiscard]] std::vector<StalenessFinding> staleness(const EvidenceBundle& current,
                                                        bool& any_stream_missing) const;

  // Verdict with fencing applied: staleness first, then status.
  [[nodiscard]] SafetyVerdict verdict(std::span<const StalenessFinding> findings) const;
  [[nodiscard]] SafetyVerdict verdict() const;

  [[nodiscard]] std::uint32_t wave_count() const;
  [[nodiscard]] std::size_t obligation_count() const;

  // Canonical, deterministic text report.  Stable across runs and platforms:
  // no addresses, no iteration order, no locale.
  [[nodiscard]] std::string to_text() const;

  friend bool operator==(const Plan& a, const Plan& b) noexcept {
    return a.id == b.id && a.planner == b.planner && a.revision == b.revision &&
           a.sequence == b.sequence && a.epoch == b.epoch && a.lineage == b.lineage &&
           a.idempotency_key == b.idempotency_key && a.request_digest == b.request_digest &&
           a.source_rack == b.source_rack &&
           a.composition_revision == b.composition_revision && a.isolation == b.isolation &&
           a.evacuate_kinds == b.evacuate_kinds && a.bindings == b.bindings &&
           a.indeterminacy_reasons == b.indeterminacy_reasons && a.status == b.status &&
           a.assignments == b.assignments && a.residuals == b.residuals &&
           a.out_of_scope == b.out_of_scope && a.plan_digest == b.plan_digest;
  }
};

// An independent audit of a plan against the request that produced it.  This
// recomputes conservation, ordering, capacity, and compatibility from the
// request evidence instead of trusting the planner's own bookkeeping, so it
// can find a plan that was sealed correctly but planned wrongly.  An empty
// result means the audit found nothing.
struct REP_API AuditFinding {
  std::string code;
  std::string detail;
};

[[nodiscard]] REP_API std::vector<AuditFinding> audit_plan(const Plan& plan,
                                                            const PlanRequest& request);

} // namespace rep

#endif // REP_PLAN_HPP
