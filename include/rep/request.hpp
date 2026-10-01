// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_REQUEST_HPP
#define REP_REQUEST_HPP

#include <vector>

#include "rep/canonical.hpp"
#include "rep/evidence.hpp"
#include "rep/export.hpp"
#include "rep/status.hpp"
#include "rep/types.hpp"

namespace rep {

// What isolation the requester wants proven.
struct REP_API IsolationRequest {
  RackId rack;
  IsolationKind kind{IsolationKind::Depower};
  // The exact rack composition revision the request is about.  A plan built
  // against any other revision cannot answer this request.
  Generation composition_revision;
  // Which obligation kinds must leave the rack.  Kinds not listed are
  // reported as outside scope, with that reason recorded.
  std::vector<ObligationKind> evacuate_kinds;   // canonical: sorted, unique, non-empty

  [[nodiscard]] static Result<IsolationRequest> make(RackId rack, IsolationKind kind,
                                                     Generation composition_revision,
                                                     std::vector<ObligationKind> evacuate_kinds);

  [[nodiscard]] Digest digest() const;
  void encode(CanonicalWriter& writer) const;

  friend bool operator==(const IsolationRequest& a, const IsolationRequest& b) noexcept {
    return a.rack == b.rack && a.kind == b.kind &&
           a.composition_revision == b.composition_revision &&
           a.evacuate_kinds == b.evacuate_kinds;
  }
};

// The obligation kinds that a given isolation normally requires to leave.
// This is a convenience for callers, not a planner default: the request still
// states its own scope explicitly.
[[nodiscard]] REP_API std::vector<ObligationKind> default_evacuate_kinds(IsolationKind kind);

// One complete planning request.  A request is the unit of idempotency: the
// same key with the same digest is a replay of an already committed
// operation, the same key with a different digest is a conflict.
struct REP_API PlanRequest {
  IdempotencyKey idempotency_key;
  AuthorityId requested_by;
  // The control incarnation the requester believes it is talking to.  A
  // non-zero value that does not match the engine's current epoch is refused:
  // the caller read state from an authority that is no longer current.
  // Zero asserts nothing.
  Epoch expected_epoch;
  // The instant the plan is evaluated at.  The planner never reads a clock:
  // maintenance windows and incident state are evaluated against this value.
  UnixNanos evaluation_time;
  IsolationRequest isolation;
  // Empty selects a fresh lineage; otherwise the plan continues the lineage
  // and its revision increases.
  LineageId lineage;
  std::vector<EvidenceSource> evidence_sources;   // canonical: sorted, unique
  EvidenceBundle evidence;

  [[nodiscard]] static Result<PlanRequest> make(IdempotencyKey idempotency_key,
                                                AuthorityId requested_by, Epoch expected_epoch,
                                                UnixNanos evaluation_time,
                                                IsolationRequest isolation, LineageId lineage,
                                                std::vector<EvidenceSource> evidence_sources,
                                                EvidenceBundle evidence);

  // Identity of the operation, independent of the caller's epoch view.  The
  // expected epoch is a precondition fence, not part of what is being asked
  // for, so a retry that carries a stale epoch view is still demonstrably the
  // same operation and is resolved as a replay before any epoch check.
  [[nodiscard]] Digest digest() const;
  void encode(CanonicalWriter& writer) const;

  friend bool operator==(const PlanRequest& a, const PlanRequest& b) noexcept {
    return a.idempotency_key == b.idempotency_key && a.requested_by == b.requested_by &&
           a.expected_epoch == b.expected_epoch && a.evaluation_time == b.evaluation_time &&
           a.isolation == b.isolation &&
           a.lineage == b.lineage && a.evidence_sources == b.evidence_sources &&
           a.evidence == b.evidence;
  }
};

} // namespace rep

#endif // REP_REQUEST_HPP
