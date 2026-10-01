// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_EVIDENCE_HPP
#define REP_EVIDENCE_HPP

#include <cstddef>
#include <span>
#include <variant>
#include <vector>

#include "rep/adjacent.hpp"
#include "rep/candidate.hpp"
#include "rep/canonical.hpp"
#include "rep/export.hpp"
#include "rep/obligation.hpp"
#include "rep/policy.hpp"
#include "rep/status.hpp"
#include "rep/types.hpp"

namespace rep {

// Every evidence record carries the identity of the authority that published
// it, the generation of that stream, the authority's control epoch, and a
// digest over the payload.  None of these is optional: a record without them
// cannot be bound to a plan, and a plan that cannot name its inputs cannot be
// fenced against them.
struct REP_API EvidenceStamp {
  StreamRef source;
  Generation generation;
  Epoch epoch;
  Digest content_digest;

  void encode(CanonicalWriter& writer) const {
    source.encode(writer);
    generation.encode(writer);
    epoch.encode(writer);
    writer.digest(content_digest);
  }
  friend bool operator==(const EvidenceStamp& a, const EvidenceStamp& b) noexcept {
    return a.source == b.source && a.generation == b.generation && a.epoch == b.epoch &&
           a.content_digest == b.content_digest;
  }
};

using EvidencePayload =
    std::variant<RackCompositionPayload, EnumerationPayload, ObligationCatalogPayload,
                 CapacityPayload, PlacementPolicyPayload, MaintenancePayload,
                 FailureDomainTopology, AsiWorkloadPayload, DfiObligationPayload,
                 CandidateOffersPayload>;

struct REP_API EvidenceRecord {
  EvidenceKind kind{EvidenceKind::RackComposition};
  EvidenceStamp stamp;
  EvidencePayload payload;

  void encode(CanonicalWriter& writer) const;

  friend bool operator==(const EvidenceRecord& a, const EvidenceRecord& b) noexcept;
};

// Names the stream the request requires for one evidence kind.  Authority is
// chosen explicitly by the requester: a record that happens to be present in
// the bundle is never used unless the request bound it.  More than one stream
// may be bound to the same kind, in which case their contents are merged and
// conflicting duplicate facts are refused rather than resolved.
struct REP_API EvidenceSource {
  EvidenceKind kind{EvidenceKind::RackComposition};
  StreamRef source;

  void encode(CanonicalWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(kind));
    source.encode(writer);
  }
  friend bool operator==(const EvidenceSource& a, const EvidenceSource& b) noexcept {
    return a.kind == b.kind && a.source == b.source;
  }
  friend auto operator<=>(const EvidenceSource& a, const EvidenceSource& b) noexcept {
    if (a.kind != b.kind) {
      return static_cast<std::uint16_t>(a.kind) <=> static_cast<std::uint16_t>(b.kind);
    }
    return a.source <=> b.source;
  }
};

// The canonical domain tag used for one evidence kind's payload digest.
[[nodiscard]] REP_API std::string_view evidence_payload_domain(EvidenceKind kind) noexcept;

// Computes the payload digest of a record under its kind's domain tag.
[[nodiscard]] REP_API Digest evidence_payload_digest(EvidenceKind kind,
                                                     const EvidencePayload& payload);

// A validated, canonically ordered set of evidence records.
class REP_API EvidenceBundle {
 public:
  EvidenceBundle() = default;

  // Validates every record (kind/payload agreement, payload digest, limits),
  // then sorts by (kind, source) and rejects duplicate (kind, source) pairs.
  [[nodiscard]] static Result<EvidenceBundle> make(
      std::vector<EvidenceRecord> records, std::size_t max_records = 4096);

  [[nodiscard]] std::span<const EvidenceRecord> records() const noexcept {
    return std::span<const EvidenceRecord>(records_.data(), records_.size());
  }
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] bool empty() const noexcept { return records_.empty(); }

  [[nodiscard]] const EvidenceRecord* find(EvidenceKind kind, const StreamRef& source) const
      noexcept;
  [[nodiscard]] std::vector<const EvidenceRecord*> find_all(EvidenceKind kind) const;

  void encode(CanonicalWriter& writer) const { encode_sequence(writer, records_); }
  friend bool operator==(const EvidenceBundle& a, const EvidenceBundle& b) noexcept {
    return a.records_ == b.records_;
  }

 private:
  std::vector<EvidenceRecord> records_;
};

} // namespace rep

#endif // REP_EVIDENCE_HPP
