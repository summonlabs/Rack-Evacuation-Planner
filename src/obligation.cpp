// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/obligation.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "detail/containers.hpp"

namespace rep {

Result<Obligation> Obligation::make(ObligationId id, ObligationKind kind, RackId source_rack,
                                    ResourceVector demand, bool protected_obligation,
                                    std::vector<ObligationId> depends_on) {
  if (id.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "obligation id must not be empty", "id");
  }
  if (source_rack.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "obligation source rack must not be empty",
                      "rack");
  }
  for (const ObligationId& dependency : depends_on) {
    if (dependency == id) {
      return make_error(ErrorCode::InvalidArgument,
                        "obligation " + id.str() + " cannot depend on itself", "depends_on");
    }
  }
  auto canonical = detail::sorted_unique(std::move(depends_on), "depends_on");
  if (!canonical.ok()) {
    return canonical.error();
  }

  Obligation obligation;
  obligation.id = std::move(id);
  obligation.kind = kind;
  obligation.source_rack = std::move(source_rack);
  obligation.demand = std::move(demand);
  obligation.protected_obligation = protected_obligation;
  obligation.depends_on = canonical.take();
  return obligation;
}

Digest Obligation::definition_digest() const { return digest_of(domains::kObligation, *this); }

void Obligation::encode(CanonicalWriter& writer) const {
  writer.text(id.view());
  writer.u16(static_cast<std::uint16_t>(kind));
  writer.text(source_rack.view());
  demand.encode(writer);
  writer.boolean(protected_obligation);
  encode_sequence(writer, depends_on);
}

Result<ObligationCatalogPayload> ObligationCatalogPayload::make(
    std::vector<Obligation> obligations) {
  auto canonical = detail::sorted_unique_by(
      std::move(obligations), [](const Obligation& item) { return item.id; }, "obligation");
  if (!canonical.ok()) {
    return canonical.error();
  }
  ObligationCatalogPayload payload;
  payload.obligations = canonical.take();
  return payload;
}

const Obligation* ObligationCatalogPayload::find(const ObligationId& id) const noexcept {
  const auto position = std::lower_bound(
      obligations.begin(), obligations.end(), id,
      [](const Obligation& candidate, const ObligationId& key) { return candidate.id < key; });
  if (position == obligations.end() || !(position->id == id)) {
    return nullptr;
  }
  return &*position;
}

Result<RackCompositionPayload> RackCompositionPayload::make(RackId rack,
                                                            Generation composition_revision,
                                                            std::vector<ObligationId> occupants) {
  if (rack.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "rack composition rack must not be empty",
                      "rack");
  }
  auto canonical = detail::sorted_unique(std::move(occupants), "rack composition occupants");
  if (!canonical.ok()) {
    return canonical.error();
  }
  RackCompositionPayload payload;
  payload.rack = std::move(rack);
  payload.composition_revision = composition_revision;
  payload.occupants = canonical.take();
  return payload;
}

Result<EnumerationPayload> EnumerationPayload::make(RackId rack, Generation composition_revision,
                                                    bool complete,
                                                    std::vector<ObligationId> enumerated) {
  if (rack.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "enumeration rack must not be empty", "rack");
  }
  auto canonical = detail::sorted_unique(std::move(enumerated), "enumeration set");
  if (!canonical.ok()) {
    return canonical.error();
  }
  EnumerationPayload payload;
  payload.rack = std::move(rack);
  payload.composition_revision = composition_revision;
  payload.complete = complete;
  payload.enumerated = canonical.take();
  return payload;
}

} // namespace rep
