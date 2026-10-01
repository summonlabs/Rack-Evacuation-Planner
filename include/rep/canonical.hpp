// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_CANONICAL_HPP
#define REP_CANONICAL_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rep/digest.hpp"
#include "rep/export.hpp"

namespace rep {

// Canonical byte encoding used for every digest and for durable publication.
//
// Rules (stable, versioned by the domain tag):
//   * integers are fixed width little endian; signed values are two's
//     complement bit patterns of the same width;
//   * booleans are a single byte, 0x00 or 0x01;
//   * enums are their stable 16-bit code;
//   * every variable length byte sequence (text, blob, nested record) is
//     preceded by its element count as a 64-bit little endian value;
//   * containers are written in the canonical order defined by their element
//     comparator, never in iteration order of a hash container;
//   * nothing is written that depends on host endianness, pointer width,
//     locale, or padding.
//
// The digest of an encoded record is SHA-256 over
//   u64(len(domain_tag)) || domain_tag || encoded_bytes
// so that two different record kinds can never produce the same digest even
// when their encodings coincide.
class REP_API CanonicalWriter {
 public:
  explicit CanonicalWriter(std::string_view domain_tag);

  CanonicalWriter& u8(std::uint8_t value);
  CanonicalWriter& u16(std::uint16_t value);
  CanonicalWriter& u32(std::uint32_t value);
  CanonicalWriter& u64(std::uint64_t value);
  CanonicalWriter& i32(std::int32_t value);
  CanonicalWriter& i64(std::int64_t value);
  CanonicalWriter& boolean(bool value);
  CanonicalWriter& byte(std::byte value);
  CanonicalWriter& bytes(std::span<const std::byte> value);   // u64 length + payload
  CanonicalWriter& raw(std::span<const std::byte> value);     // payload only
  CanonicalWriter& text(std::string_view value);              // u64 length + payload
  CanonicalWriter& digest(const Digest& value);               // 32 raw bytes

  [[nodiscard]] const std::vector<std::byte>& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::span<const std::byte> view() const noexcept {
    return std::span<const std::byte>(buffer_.data(), buffer_.size());
  }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] const std::string& domain_tag() const noexcept { return domain_tag_; }

  // Digest of domain_tag followed by every byte written so far.
  [[nodiscard]] Digest finish() const noexcept;

  // Lowercase hex of the encoded bytes (not of the digest).
  [[nodiscard]] std::string to_hex() const;

 private:
  std::vector<std::byte> buffer_;
  std::string domain_tag_;
};

// Writes a container in its canonical order: element count, then elements.
template <class T>
void encode_sequence(CanonicalWriter& writer, const std::vector<T>& items) {
  writer.u64(static_cast<std::uint64_t>(items.size()));
  for (const T& item : items) {
    item.encode(writer);
  }
}

// Computes the canonical digest of one value under a domain tag.
template <class T>
[[nodiscard]] Digest digest_of(std::string_view domain_tag, const T& value) {
  CanonicalWriter writer(domain_tag);
  value.encode(writer);
  return writer.finish();
}

// Computes the canonical bytes of one value under a domain tag.
template <class T>
[[nodiscard]] std::vector<std::byte> encode_to_bytes(std::string_view domain_tag, const T& value) {
  CanonicalWriter writer(domain_tag);
  value.encode(writer);
  return writer.buffer();
}

// Domain separation tags.  Bumping any of these invalidates previously
// computed digests for that record kind, which is exactly the intent.
namespace domains {

inline constexpr std::string_view kRackComposition = "rep.payload.rack-composition.v1";
inline constexpr std::string_view kEnumeration = "rep.payload.enumeration.v1";
inline constexpr std::string_view kObligationCatalog = "rep.payload.obligation-catalog.v1";
inline constexpr std::string_view kCapacity = "rep.payload.capacity.v1";
inline constexpr std::string_view kPlacementPolicy = "rep.payload.placement-policy.v1";
inline constexpr std::string_view kMaintenance = "rep.payload.maintenance.v1";
inline constexpr std::string_view kFailureDomain = "rep.payload.failure-domain.v1";
inline constexpr std::string_view kAsiWorkload = "rep.payload.asi-workload.v1";
inline constexpr std::string_view kDfiObligation = "rep.payload.dfi-obligation.v1";
inline constexpr std::string_view kEvidenceEnvelope = "rep.evidence-envelope.v1";
inline constexpr std::string_view kPlan = "rep.plan.v1";
inline constexpr std::string_view kPlanRequest = "rep.plan-request.v1";
inline constexpr std::string_view kIsolationRequest = "rep.isolation-request.v1";
inline constexpr std::string_view kObligation = "rep.obligation.v1";
inline constexpr std::string_view kCandidate = "rep.candidate.v1";
inline constexpr std::string_view kCandidateOffers = "rep.payload.candidate-offers.v1";
inline constexpr std::string_view kStoreState = "rep.store-state.v1";
inline constexpr std::string_view kScenario = "rep.scenario.v1";

} // namespace domains

} // namespace rep

#endif // REP_CANONICAL_HPP
