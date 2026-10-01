// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/request.hpp"

#include <string>
#include <utility>

#include "detail/containers.hpp"

namespace rep {

std::vector<ObligationKind> default_evacuate_kinds(IsolationKind kind) {
  switch (kind) {
    case IsolationKind::Depower:
      return {ObligationKind::Workload,     ObligationKind::StorageReplica,
              ObligationKind::NetworkPath,  ObligationKind::ServiceEndpoint,
              ObligationKind::Reservation,  ObligationKind::Appliance};
    case IsolationKind::PhysicalService:
      return {ObligationKind::Workload,     ObligationKind::StorageReplica,
              ObligationKind::NetworkPath,  ObligationKind::ServiceEndpoint,
              ObligationKind::Reservation,  ObligationKind::Appliance};
    case IsolationKind::ThermalConstraint:
      return {ObligationKind::Workload, ObligationKind::Appliance};
    case IsolationKind::NetworkIsolation:
      return {ObligationKind::NetworkPath, ObligationKind::ServiceEndpoint};
  }
  return {};
}

Result<IsolationRequest> IsolationRequest::make(RackId rack, IsolationKind kind,
                                                Generation composition_revision,
                                                std::vector<ObligationKind> evacuate_kinds) {
  if (rack.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "isolation rack must not be empty", "rack");
  }
  if (evacuate_kinds.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "isolation request must name at least one obligation kind to evacuate",
                      "evacuate_kinds");
  }
  auto canonical = detail::sorted_unique(std::move(evacuate_kinds), "evacuate kinds");
  if (!canonical.ok()) {
    return canonical.error();
  }
  IsolationRequest request;
  request.rack = std::move(rack);
  request.kind = kind;
  request.composition_revision = composition_revision;
  request.evacuate_kinds = canonical.take();
  return request;
}

Digest IsolationRequest::digest() const { return digest_of(domains::kIsolationRequest, *this); }

void IsolationRequest::encode(CanonicalWriter& writer) const {
  writer.text(rack.view());
  writer.u16(static_cast<std::uint16_t>(kind));
  composition_revision.encode(writer);
  writer.u64(static_cast<std::uint64_t>(evacuate_kinds.size()));
  for (const ObligationKind entry : evacuate_kinds) {
    writer.u16(static_cast<std::uint16_t>(entry));
  }
}

Result<PlanRequest> PlanRequest::make(IdempotencyKey idempotency_key, AuthorityId requested_by,
                                      Epoch expected_epoch, UnixNanos evaluation_time,
                                      IsolationRequest isolation,
                                      LineageId lineage,
                                      std::vector<EvidenceSource> evidence_sources,
                                      EvidenceBundle evidence) {
  if (idempotency_key.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "request idempotency key must not be empty",
                      "idempotency_key");
  }
  if (requested_by.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "requesting authority must not be empty",
                      "requested_by");
  }
  auto canonical = detail::sorted_unique(std::move(evidence_sources), "evidence source");
  if (!canonical.ok()) {
    return canonical.error();
  }
  PlanRequest request;
  request.idempotency_key = std::move(idempotency_key);
  request.requested_by = std::move(requested_by);
  request.expected_epoch = expected_epoch;
  request.evaluation_time = evaluation_time;
  request.isolation = std::move(isolation);
  request.lineage = std::move(lineage);
  request.evidence_sources = canonical.take();
  request.evidence = std::move(evidence);
  return request;
}

Digest PlanRequest::digest() const { return digest_of(domains::kPlanRequest, *this); }

void PlanRequest::encode(CanonicalWriter& writer) const {
  writer.text(idempotency_key.view());
  writer.text(requested_by.view());
  // expected_epoch is deliberately absent: see PlanRequest::digest().
  evaluation_time.encode(writer);
  isolation.encode(writer);
  writer.text(lineage.view());
  encode_sequence(writer, evidence_sources);
  evidence.encode(writer);
}

} // namespace rep
