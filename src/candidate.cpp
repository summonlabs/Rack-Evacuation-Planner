// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/candidate.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "detail/containers.hpp"

namespace rep {

bool action_matches_obligation(ObligationKind obligation, ActionKind action) noexcept {
  switch (obligation) {
    case ObligationKind::Workload:
      return action == ActionKind::LiveMigrate || action == ActionKind::ColdMigrate ||
             action == ActionKind::Quiesce;
    case ObligationKind::Appliance:
      return action == ActionKind::LiveMigrate || action == ActionKind::ColdMigrate ||
             action == ActionKind::Quiesce || action == ActionKind::Depower;
    case ObligationKind::StorageReplica:
    case ObligationKind::NetworkPath:
      return action == ActionKind::Detach || action == ActionKind::Rebind;
    case ObligationKind::ServiceEndpoint:
      return action == ActionKind::Quiesce || action == ActionKind::Detach ||
             action == ActionKind::Rebind;
    case ObligationKind::Reservation:
      return action == ActionKind::ColdMigrate || action == ActionKind::Release ||
             action == ActionKind::Depower;
  }
  return false;
}

bool action_matches_destination(ActionKind action, DestinationKind destination) noexcept {
  switch (action) {
    case ActionKind::LiveMigrate:
    case ActionKind::ColdMigrate:
    case ActionKind::Quiesce:
    case ActionKind::Release:
    case ActionKind::Depower:
      return destination == DestinationKind::RackSlot;
    case ActionKind::Detach:
    case ActionKind::Rebind:
      return destination == DestinationKind::FabricEndpoint ||
             destination == DestinationKind::StorageTarget ||
             destination == DestinationKind::ServiceEndpoint;
  }
  return false;
}

namespace {

Digest candidate_content_digest(const Candidate& candidate) {
  CanonicalWriter writer(domains::kCandidate);
  candidate.encode_content(writer);
  return writer.finish();
}

} // namespace

Result<Candidate> Candidate::make(CandidateId id, ObligationId obligation, ActionKind action,
                                  DestinationRef destination, AuthorityId authority,
                                  Generation authority_generation, Epoch authority_epoch,
                                  ResourceVector provision, std::uint32_t estimated_cost,
                                  bool requires_maintenance_window) {
  if (id.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "candidate id must not be empty", "id");
  }
  if (obligation.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "candidate obligation must not be empty",
                      "obligation");
  }
  if (authority.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "candidate authority must not be empty",
                      "authority");
  }
  if (destination.id.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "candidate destination must not be empty",
                      "destination");
  }

  Candidate candidate;
  candidate.id = std::move(id);
  candidate.obligation = std::move(obligation);
  candidate.action = action;
  candidate.destination = std::move(destination);
  candidate.authority = std::move(authority);
  candidate.authority_generation = authority_generation;
  candidate.authority_epoch = authority_epoch;
  candidate.provision = std::move(provision);
  candidate.estimated_cost = estimated_cost;
  candidate.requires_maintenance_window = requires_maintenance_window;
  candidate.evidence_digest = candidate_content_digest(candidate);
  return candidate;
}

Digest Candidate::content_digest() const { return candidate_content_digest(*this); }

void Candidate::encode_content(CanonicalWriter& writer) const {
  writer.text(id.view());
  writer.text(obligation.view());
  writer.u16(static_cast<std::uint16_t>(action));
  destination.encode(writer);
  writer.text(authority.view());
  authority_generation.encode(writer);
  authority_epoch.encode(writer);
  provision.encode(writer);
  writer.u32(estimated_cost);
  writer.boolean(requires_maintenance_window);
}

void Candidate::encode(CanonicalWriter& writer) const {
  encode_content(writer);
  writer.digest(evidence_digest);
}

Result<CandidateOffersPayload> CandidateOffersPayload::make(std::vector<Candidate> candidates) {
  auto canonical = detail::sorted_unique_by(
      std::move(candidates), [](const Candidate& item) { return item.id; }, "candidate");
  if (!canonical.ok()) {
    return canonical.error();
  }
  CandidateOffersPayload payload;
  payload.candidates = canonical.take();
  return payload;
}

const Candidate* CandidateOffersPayload::find(const CandidateId& id) const noexcept {
  const auto position = std::lower_bound(
      candidates.begin(), candidates.end(), id,
      [](const Candidate& candidate, const CandidateId& key) { return candidate.id < key; });
  if (position == candidates.end() || !(position->id == id)) {
    return nullptr;
  }
  return &*position;
}

} // namespace rep
