// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_CANDIDATE_HPP
#define REP_CANDIDATE_HPP

#include <cstdint>
#include <vector>

#include "rep/canonical.hpp"
#include "rep/export.hpp"
#include "rep/status.hpp"
#include "rep/types.hpp"

namespace rep {

// Static compatibility tables.  These describe which action can express which
// obligation and which destination kind that action can target.  They encode
// no policy: a compatible pair is still subject to eligibility, capacity, and
// placement rules.
[[nodiscard]] REP_API bool action_matches_obligation(ObligationKind obligation,
                                                     ActionKind action) noexcept;
[[nodiscard]] REP_API bool action_matches_destination(ActionKind action,
                                                      DestinationKind destination) noexcept;

// A typed destination/action candidate supplied by an adjacent authority.
// The planner never synthesizes one: a candidate exists because an authority
// that owns the destination, the migration, or the path asserted that this
// specific action against this specific destination is available at a
// specific generation.
struct REP_API Candidate {
  CandidateId id;
  ObligationId obligation;
  ActionKind action{ActionKind::LiveMigrate};
  DestinationRef destination;
  // The authority that asserts this action is possible.  It must be bound in
  // the request's evidence source map, and its generation and epoch must be
  // the ones that stream published.
  AuthorityId authority;
  Generation authority_generation;
  Epoch authority_epoch;
  // Resources the action requires at the destination.  Must cover the
  // obligation's own demand.
  ResourceVector provision;
  // Deterministic non-negative cost used only to order equally eligible
  // candidates.  Smaller is preferred.
  std::uint32_t estimated_cost{0};
  // True when the action may only run inside an open maintenance window.
  bool requires_maintenance_window{false};
  // Content digest of every field above.  Verified on load.
  Digest evidence_digest;

  [[nodiscard]] static Result<Candidate> make(CandidateId id, ObligationId obligation,
                                              ActionKind action, DestinationRef destination,
                                              AuthorityId authority,
                                              Generation authority_generation,
                                              Epoch authority_epoch, ResourceVector provision,
                                              std::uint32_t estimated_cost,
                                              bool requires_maintenance_window);

  [[nodiscard]] Digest content_digest() const;
  void encode_content(CanonicalWriter& writer) const;
  void encode(CanonicalWriter& writer) const;

  friend bool operator==(const Candidate& a, const Candidate& b) noexcept {
    return a.id == b.id && a.obligation == b.obligation && a.action == b.action &&
           a.destination == b.destination && a.authority == b.authority &&
           a.authority_generation == b.authority_generation &&
           a.authority_epoch == b.authority_epoch && a.provision == b.provision &&
           a.estimated_cost == b.estimated_cost &&
           a.requires_maintenance_window == b.requires_maintenance_window &&
           a.evidence_digest == b.evidence_digest;
  }
  friend auto operator<=>(const Candidate& a, const Candidate& b) noexcept {
    return a.id <=> b.id;
  }
};

struct REP_API CandidateOffersPayload {
  std::vector<Candidate> candidates;   // canonical: sorted by id, unique

  [[nodiscard]] static Result<CandidateOffersPayload> make(std::vector<Candidate> candidates);
  [[nodiscard]] const Candidate* find(const CandidateId& id) const noexcept;

  void encode(CanonicalWriter& writer) const { encode_sequence(writer, candidates); }
  friend bool operator==(const CandidateOffersPayload& a,
                         const CandidateOffersPayload& b) noexcept {
    return a.candidates == b.candidates;
  }
};

} // namespace rep

#endif // REP_CANDIDATE_HPP
