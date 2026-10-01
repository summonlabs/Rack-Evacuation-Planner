// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_OBLIGATION_HPP
#define REP_OBLIGATION_HPP

#include <cstdint>
#include <vector>

#include "rep/canonical.hpp"
#include "rep/export.hpp"
#include "rep/status.hpp"
#include "rep/types.hpp"

namespace rep {

// An obligation is one unit of work that must leave the source rack.  The
// planner does not invent obligations: the rack-composition authority
// enumerates the occupants, an obligation catalog defines them, and the
// planner decides what must leave.
//
// The definition is content-addressed: definition_digest() covers every field
// below, and every consumer verifies it, so a definition cannot be altered
// without the alteration being detected.
struct REP_API Obligation {
  ObligationId id;
  ObligationKind kind{ObligationKind::Workload};
  RackId source_rack;
  ResourceVector demand;
  bool protected_obligation{false};
  // Identifiers this obligation must not be evacuated before.  Sorted and
  // unique; self references are rejected.
  std::vector<ObligationId> depends_on;

  [[nodiscard]] static Result<Obligation> make(ObligationId id, ObligationKind kind,
                                               RackId source_rack, ResourceVector demand,
                                               bool protected_obligation,
                                               std::vector<ObligationId> depends_on);

  [[nodiscard]] Digest definition_digest() const;

  void encode(CanonicalWriter& writer) const;

  friend bool operator==(const Obligation& a, const Obligation& b) noexcept {
    return a.id == b.id && a.kind == b.kind && a.source_rack == b.source_rack &&
           a.demand == b.demand && a.protected_obligation == b.protected_obligation &&
           a.depends_on == b.depends_on;
  }
  friend auto operator<=>(const Obligation& a, const Obligation& b) noexcept {
    return a.id <=> b.id;
  }
};

// The payload carried by an obligation-catalog evidence stream.
struct REP_API ObligationCatalogPayload {
  std::vector<Obligation> obligations;   // canonical: sorted by id, unique

  [[nodiscard]] static Result<ObligationCatalogPayload> make(std::vector<Obligation> obligations);
  [[nodiscard]] const Obligation* find(const ObligationId& id) const noexcept;

  void encode(CanonicalWriter& writer) const {
    encode_sequence(writer, obligations);
  }
  friend bool operator==(const ObligationCatalogPayload& a,
                         const ObligationCatalogPayload& b) noexcept {
    return a.obligations == b.obligations;
  }
};

// The payload carried by a rack-composition evidence stream.  Occupants are
// the units the rack authority asserts are present; the enumeration stream
// separately proves that the list is complete for this revision.
struct REP_API RackCompositionPayload {
  RackId rack;
  Generation composition_revision;
  std::vector<ObligationId> occupants;   // canonical: sorted, unique

  [[nodiscard]] static Result<RackCompositionPayload> make(RackId rack,
                                                           Generation composition_revision,
                                                           std::vector<ObligationId> occupants);

  void encode(CanonicalWriter& writer) const {
    writer.text(rack.view());
    composition_revision.encode(writer);
    encode_sequence(writer, occupants);
  }
  friend bool operator==(const RackCompositionPayload& a,
                         const RackCompositionPayload& b) noexcept {
    return a.rack == b.rack && a.composition_revision == b.composition_revision &&
           a.occupants == b.occupants;
  }
};

// The payload carried by an enumeration evidence stream.  complete == true is
// the only thing that lets the planner claim the obligation set is known; the
// revision must match the composition revision being planned, and the
// enumerated set must equal the occupant set exactly.
struct REP_API EnumerationPayload {
  RackId rack;
  Generation composition_revision;
  bool complete{false};
  std::vector<ObligationId> enumerated;   // canonical: sorted, unique

  [[nodiscard]] static Result<EnumerationPayload> make(RackId rack,
                                                       Generation composition_revision,
                                                       bool complete,
                                                       std::vector<ObligationId> enumerated);

  void encode(CanonicalWriter& writer) const {
    writer.text(rack.view());
    composition_revision.encode(writer);
    writer.boolean(complete);
    encode_sequence(writer, enumerated);
  }
  friend bool operator==(const EnumerationPayload& a, const EnumerationPayload& b) noexcept {
    return a.rack == b.rack && a.composition_revision == b.composition_revision &&
           a.complete == b.complete && a.enumerated == b.enumerated;
  }
};

} // namespace rep

#endif // REP_OBLIGATION_HPP
