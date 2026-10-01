// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/evidence.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "detail/containers.hpp"

namespace rep {

std::string_view evidence_payload_domain(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::RackComposition:
      return domains::kRackComposition;
    case EvidenceKind::Enumeration:
      return domains::kEnumeration;
    case EvidenceKind::ObligationCatalog:
      return domains::kObligationCatalog;
    case EvidenceKind::Capacity:
      return domains::kCapacity;
    case EvidenceKind::PlacementPolicy:
      return domains::kPlacementPolicy;
    case EvidenceKind::Maintenance:
      return domains::kMaintenance;
    case EvidenceKind::FailureDomain:
      return domains::kFailureDomain;
    case EvidenceKind::AsiWorkloadState:
      return domains::kAsiWorkload;
    case EvidenceKind::DfiObligation:
      return domains::kDfiObligation;
    case EvidenceKind::CandidateOffers:
      return domains::kCandidateOffers;
  }
  return domains::kEvidenceEnvelope;
}

Digest evidence_payload_digest(EvidenceKind kind, const EvidencePayload& payload) {
  CanonicalWriter writer(evidence_payload_domain(kind));
  std::visit([&writer](const auto& record) { record.encode(writer); }, payload);
  return writer.finish();
}

void EvidenceRecord::encode(CanonicalWriter& writer) const {
  writer.u16(static_cast<std::uint16_t>(kind));
  stamp.encode(writer);
  std::visit([&writer](const auto& record) { record.encode(writer); }, payload);
}

bool operator==(const EvidenceRecord& a, const EvidenceRecord& b) noexcept {
  return a.kind == b.kind && a.stamp == b.stamp && a.payload == b.payload;
}

namespace {

[[nodiscard]] bool payload_matches_kind(EvidenceKind kind, const EvidencePayload& payload) noexcept {
  switch (kind) {
    case EvidenceKind::RackComposition:
      return std::holds_alternative<RackCompositionPayload>(payload);
    case EvidenceKind::Enumeration:
      return std::holds_alternative<EnumerationPayload>(payload);
    case EvidenceKind::ObligationCatalog:
      return std::holds_alternative<ObligationCatalogPayload>(payload);
    case EvidenceKind::Capacity:
      return std::holds_alternative<CapacityPayload>(payload);
    case EvidenceKind::PlacementPolicy:
      return std::holds_alternative<PlacementPolicyPayload>(payload);
    case EvidenceKind::Maintenance:
      return std::holds_alternative<MaintenancePayload>(payload);
    case EvidenceKind::FailureDomain:
      return std::holds_alternative<FailureDomainTopology>(payload);
    case EvidenceKind::AsiWorkloadState:
      return std::holds_alternative<AsiWorkloadPayload>(payload);
    case EvidenceKind::DfiObligation:
      return std::holds_alternative<DfiObligationPayload>(payload);
    case EvidenceKind::CandidateOffers:
      return std::holds_alternative<CandidateOffersPayload>(payload);
  }
  return false;
}

[[nodiscard]] Result<void> validate_record(const EvidenceRecord& record) {
  if (!payload_matches_kind(record.kind, record.payload)) {
    return make_error(ErrorCode::InvalidArgument,
                      "evidence record kind " + std::string(to_string(record.kind)) +
                          " does not match its payload",
                      "evidence");
  }
  const Digest computed = evidence_payload_digest(record.kind, record.payload);
  if (!(computed == record.stamp.content_digest)) {
    return make_error(ErrorCode::DigestMismatch,
                      "evidence stream " + record.stamp.source.to_text() +
                          " payload digest does not match the asserted digest",
                      "evidence");
  }
  if (const auto* offers = std::get_if<CandidateOffersPayload>(&record.payload)) {
    for (const Candidate& candidate : offers->candidates) {
      if (!(candidate.content_digest() == candidate.evidence_digest)) {
        return make_error(ErrorCode::DigestMismatch,
                          "candidate " + candidate.id.str() +
                              " does not match its own declaration digest",
                          "candidate");
      }
    }
  }
  return {};
}

} // namespace

Result<EvidenceBundle> EvidenceBundle::make(std::vector<EvidenceRecord> records,
                                            std::size_t max_records) {
  if (records.size() > max_records) {
    return make_error(ErrorCode::LimitExceeded,
                      "evidence bundle holds " + std::to_string(records.size()) +
                          " records, limit is " + std::to_string(max_records),
                      "evidence");
  }
  for (const EvidenceRecord& record : records) {
    const auto valid = validate_record(record);
    if (!valid.ok()) {
      return valid.error();
    }
  }
  std::sort(records.begin(), records.end(), [](const EvidenceRecord& a, const EvidenceRecord& b) {
    const auto a_kind = static_cast<std::uint16_t>(a.kind);
    const auto b_kind = static_cast<std::uint16_t>(b.kind);
    if (a_kind != b_kind) {
      return a_kind < b_kind;
    }
    return a.stamp.source < b.stamp.source;
  });
  for (std::size_t i = 1; i < records.size(); ++i) {
    if (records[i].kind == records[i - 1].kind && records[i].stamp.source == records[i - 1].stamp.source) {
      return make_error(ErrorCode::AlreadyExists,
                        "evidence bundle publishes stream " +
                            records[i].stamp.source.to_text() + " twice",
                        "evidence");
    }
  }
  EvidenceBundle bundle;
  bundle.records_ = std::move(records);
  return bundle;
}

const EvidenceRecord* EvidenceBundle::find(EvidenceKind kind, const StreamRef& source) const
    noexcept {
  const auto position = std::lower_bound(
      records_.begin(), records_.end(), std::pair<EvidenceKind, StreamRef>(kind, source),
      [](const EvidenceRecord& record, const std::pair<EvidenceKind, StreamRef>& key) {
        const auto record_kind = static_cast<std::uint16_t>(record.kind);
        const auto key_kind = static_cast<std::uint16_t>(key.first);
        if (record_kind != key_kind) {
          return record_kind < key_kind;
        }
        return record.stamp.source < key.second;
      });
  if (position == records_.end() || position->kind != kind ||
      !(position->stamp.source == source)) {
    return nullptr;
  }
  return &*position;
}

std::vector<const EvidenceRecord*> EvidenceBundle::find_all(EvidenceKind kind) const {
  std::vector<const EvidenceRecord*> result;
  for (const EvidenceRecord& record : records_) {
    if (record.kind == kind) {
      result.push_back(&record);
    }
  }
  return result;
}

} // namespace rep
