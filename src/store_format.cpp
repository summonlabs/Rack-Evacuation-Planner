// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "detail/store_format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "detail/canonical_reader.hpp"
#include "rep/canonical.hpp"
#include "rep/digest.hpp"

namespace rep::detail {
namespace {

constexpr std::size_t kStateHeaderSize = 104;
constexpr std::size_t kStateTrailerSize = 32;
constexpr std::size_t kCurrentHeaderSize = 96;
constexpr std::uint64_t kMaxDecodedItems = 1000000;

constexpr std::array<std::byte, 8> kStateMagic = {
    std::byte{'R'}, std::byte{'E'}, std::byte{'P'}, std::byte{'S'},
    std::byte{'T'}, std::byte{'O'}, std::byte{'R'}, std::byte{'1'}};
constexpr std::array<std::byte, 8> kCurrentMagic = {
    std::byte{'R'}, std::byte{'E'}, std::byte{'P'}, std::byte{'C'},
    std::byte{'U'}, std::byte{'R'}, std::byte{'N'}, std::byte{'T'}};

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
  }
}

void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
  }
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8u);
  }
  return value;
}

[[nodiscard]] std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8u);
  }
  return value;
}

[[nodiscard]] Digest digest_of_bytes(std::span<const std::byte> bytes) {
  Sha256 hasher;
  hasher.update(bytes);
  return Digest{hasher.finish()};
}

[[nodiscard]] bool magic_matches(std::span<const std::byte> bytes,
                                 const std::array<std::byte, 8>& magic) {
  return bytes.size() >= 8 && std::memcmp(bytes.data(), magic.data(), 8) == 0;
}

void write_digest(std::vector<std::byte>& out, const Digest& digest) {
  const auto* begin = reinterpret_cast<const std::byte*>(digest.bytes.data());
  out.insert(out.end(), begin, begin + digest.bytes.size());
}

[[nodiscard]] Digest read_digest(std::span<const std::byte> bytes, std::size_t offset) {
  Digest digest;
  std::memcpy(digest.bytes.data(), bytes.data() + offset, digest.bytes.size());
  return digest;
}

// ---------------------------------------------------------------------------
// Strict decoding
// ---------------------------------------------------------------------------

#define REP_KNOWN_ENUM(Type)                                       \
  [[nodiscard]] bool is_known(Type value) noexcept {               \
    return Type##_from_string(to_string(value)).has_value();       \
  }

REP_KNOWN_ENUM(PlanStatus)
REP_KNOWN_ENUM(ObligationKind)
REP_KNOWN_ENUM(ActionKind)
REP_KNOWN_ENUM(DestinationKind)
REP_KNOWN_ENUM(IsolationKind)
REP_KNOWN_ENUM(EvidenceKind)
REP_KNOWN_ENUM(ResidualReason)
REP_KNOWN_ENUM(RejectionReason)
REP_KNOWN_ENUM(IndeterminacyReason)
REP_KNOWN_ENUM(ScopeExclusionReason)
REP_KNOWN_ENUM(OutcomeKind)

#undef REP_KNOWN_ENUM

template <class Enum>
[[nodiscard]] Result<Enum> read_enum(CanonicalReader& reader) {
  const auto code = reader.u16();
  if (!code.ok()) {
    return code.error();
  }
  const auto value = static_cast<Enum>(code.value());
  if (!is_known(value)) {
    return make_error(ErrorCode::Corrupt,
                      "enumeration code " + std::to_string(code.value()) + " is not known");
  }
  return value;
}

template <class Id>
[[nodiscard]] Result<Id> read_identifier(CanonicalReader& reader) {
  const auto text = reader.text(kMaxIdentifierLength);
  if (!text.ok()) {
    return text.error();
  }
  return Id::parse(text.value());
}

[[nodiscard]] Result<void> read_destination(CanonicalReader& reader, DestinationRef& out) {
  const auto kind = read_enum<DestinationKind>(reader);
  if (!kind.ok()) {
    return kind.error();
  }
  const auto id = read_identifier<DestinationId>(reader);
  if (!id.ok()) {
    return id.error();
  }
  out.kind = kind.value();
  out.id = id.value();
  return {};
}

[[nodiscard]] Result<void> read_stream(CanonicalReader& reader, StreamRef& out) {
  const auto authority = read_identifier<AuthorityId>(reader);
  if (!authority.ok()) {
    return authority.error();
  }
  const auto stream = read_identifier<StreamId>(reader);
  if (!stream.ok()) {
    return stream.error();
  }
  out.authority = authority.value();
  out.stream = stream.value();
  return {};
}

[[nodiscard]] Result<void> read_binding(CanonicalReader& reader, PlanBinding& out) {
  const auto kind = read_enum<EvidenceKind>(reader);
  if (!kind.ok()) {
    return kind.error();
  }
  out.kind = kind.value();
  const auto stream = read_stream(reader, out.stream);
  if (!stream.ok()) {
    return stream.error();
  }
  const auto generation = reader.u64();
  if (!generation.ok()) {
    return generation.error();
  }
  out.generation = Generation{generation.value()};
  const auto epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.error();
  }
  out.epoch = Epoch{epoch.value()};
  const auto digest = reader.digest();
  if (!digest.ok()) {
    return digest.error();
  }
  out.digest = digest.value();
  return {};
}

[[nodiscard]] Result<void> read_rejected(CanonicalReader& reader, RejectedCandidate& out) {
  const auto candidate = read_identifier<CandidateId>(reader);
  if (!candidate.ok()) {
    return candidate.error();
  }
  out.candidate = candidate.value();
  const auto reason = read_enum<RejectionReason>(reader);
  if (!reason.ok()) {
    return reason.error();
  }
  out.reason = reason.value();
  return {};
}

[[nodiscard]] Result<void> read_residual(CanonicalReader& reader, Residual& out) {
  const auto obligation = read_identifier<ObligationId>(reader);
  if (!obligation.ok()) {
    return obligation.error();
  }
  out.obligation = obligation.value();
  const auto kind = read_enum<ObligationKind>(reader);
  if (!kind.ok()) {
    return kind.error();
  }
  out.kind = kind.value();
  const auto reason = read_enum<ResidualReason>(reader);
  if (!reason.ok()) {
    return reason.error();
  }
  out.reason = reason.value();
  const auto count = reader.count_bounded(kMaxDecodedItems, 2);
  if (!count.ok()) {
    return count.error();
  }
  out.rejected.reserve(static_cast<std::size_t>(count.value()));
  for (std::uint64_t index = 0; index < count.value(); ++index) {
    RejectedCandidate entry;
    const auto status = read_rejected(reader, entry);
    if (!status.ok()) {
      return status.error();
    }
    out.rejected.push_back(std::move(entry));
  }
  return {};
}

[[nodiscard]] Result<void> read_exclusion(CanonicalReader& reader, ScopeExclusion& out) {
  const auto obligation = read_identifier<ObligationId>(reader);
  if (!obligation.ok()) {
    return obligation.error();
  }
  out.obligation = obligation.value();
  const auto kind = read_enum<ObligationKind>(reader);
  if (!kind.ok()) {
    return kind.error();
  }
  out.kind = kind.value();
  const auto reason = read_enum<ScopeExclusionReason>(reader);
  if (!reason.ok()) {
    return reason.error();
  }
  out.reason = reason.value();
  return {};
}

[[nodiscard]] Result<void> read_assignment(CanonicalReader& reader, Assignment& out) {
  const auto obligation = read_identifier<ObligationId>(reader);
  if (!obligation.ok()) {
    return obligation.error();
  }
  out.obligation = obligation.value();
  const auto kind = read_enum<ObligationKind>(reader);
  if (!kind.ok()) {
    return kind.error();
  }
  out.kind = kind.value();
  const auto candidate = read_identifier<CandidateId>(reader);
  if (!candidate.ok()) {
    return candidate.error();
  }
  out.candidate = candidate.value();
  const auto action = read_enum<ActionKind>(reader);
  if (!action.ok()) {
    return action.error();
  }
  out.action = action.value();
  const auto destination = read_destination(reader, out.destination);
  if (!destination.ok()) {
    return destination.error();
  }
  const auto authority = read_identifier<AuthorityId>(reader);
  if (!authority.ok()) {
    return authority.error();
  }
  out.authority = authority.value();
  const auto wave = reader.u32();
  if (!wave.ok()) {
    return wave.error();
  }
  out.wave = wave.value();
  const auto order = reader.u32();
  if (!order.ok()) {
    return order.error();
  }
  out.order_index = order.value();
  return {};
}

[[nodiscard]] Result<void> read_plan(CanonicalReader& reader, Plan& out) {
  const auto id = read_identifier<PlanId>(reader);
  if (!id.ok()) {
    return id.error();
  }
  out.id = id.value();
  const auto planner = read_identifier<PlannerId>(reader);
  if (!planner.ok()) {
    return planner.error();
  }
  out.planner = planner.value();
  const auto revision = reader.u64();
  if (!revision.ok()) {
    return revision.error();
  }
  if (revision.value() > 0xffffffffull) {
    return make_error(ErrorCode::Corrupt, "plan revision exceeds 32 bits");
  }
  out.revision = PlanRevision{static_cast<std::uint32_t>(revision.value())};
  const auto sequence = reader.u64();
  if (!sequence.ok()) {
    return sequence.error();
  }
  out.sequence = Sequence{sequence.value()};
  const auto epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.error();
  }
  out.epoch = Epoch{epoch.value()};
  const auto lineage = read_identifier<LineageId>(reader);
  if (!lineage.ok()) {
    return lineage.error();
  }
  out.lineage = lineage.value();
  const auto key = read_identifier<IdempotencyKey>(reader);
  if (!key.ok()) {
    return key.error();
  }
  out.idempotency_key = key.value();
  const auto request_digest = reader.digest();
  if (!request_digest.ok()) {
    return request_digest.error();
  }
  out.request_digest = request_digest.value();
  const auto rack = read_identifier<RackId>(reader);
  if (!rack.ok()) {
    return rack.error();
  }
  out.source_rack = rack.value();
  const auto composition_revision = reader.u64();
  if (!composition_revision.ok()) {
    return composition_revision.error();
  }
  out.composition_revision = Generation{composition_revision.value()};
  const auto isolation = read_enum<IsolationKind>(reader);
  if (!isolation.ok()) {
    return isolation.error();
  }
  out.isolation = isolation.value();

  const auto kind_count = reader.count_bounded(kMaxDecodedItems, 2);
  if (!kind_count.ok()) {
    return kind_count.error();
  }
  out.evacuate_kinds.reserve(static_cast<std::size_t>(kind_count.value()));
  for (std::uint64_t index = 0; index < kind_count.value(); ++index) {
    const auto kind = read_enum<ObligationKind>(reader);
    if (!kind.ok()) {
      return kind.error();
    }
    out.evacuate_kinds.push_back(kind.value());
  }

  const auto binding_count = reader.count_bounded(kMaxDecodedItems, 80);
  if (!binding_count.ok()) {
    return binding_count.error();
  }
  out.bindings.reserve(static_cast<std::size_t>(binding_count.value()));
  for (std::uint64_t index = 0; index < binding_count.value(); ++index) {
    PlanBinding binding;
    const auto status = read_binding(reader, binding);
    if (!status.ok()) {
      return status.error();
    }
    out.bindings.push_back(std::move(binding));
  }

  const auto plan_status = read_enum<PlanStatus>(reader);
  if (!plan_status.ok()) {
    return plan_status.error();
  }
  out.status = plan_status.value();

  const auto reason_count = reader.count_bounded(kMaxDecodedItems, 2);
  if (!reason_count.ok()) {
    return reason_count.error();
  }
  out.indeterminacy_reasons.reserve(static_cast<std::size_t>(reason_count.value()));
  for (std::uint64_t index = 0; index < reason_count.value(); ++index) {
    const auto reason = read_enum<IndeterminacyReason>(reader);
    if (!reason.ok()) {
      return reason.error();
    }
    out.indeterminacy_reasons.push_back(reason.value());
  }

  const auto assignment_count = reader.count_bounded(kMaxDecodedItems, 40);
  if (!assignment_count.ok()) {
    return assignment_count.error();
  }
  out.assignments.reserve(static_cast<std::size_t>(assignment_count.value()));
  for (std::uint64_t index = 0; index < assignment_count.value(); ++index) {
    Assignment assignment;
    const auto status = read_assignment(reader, assignment);
    if (!status.ok()) {
      return status.error();
    }
    out.assignments.push_back(std::move(assignment));
  }

  const auto residual_count = reader.count_bounded(kMaxDecodedItems, 10);
  if (!residual_count.ok()) {
    return residual_count.error();
  }
  out.residuals.reserve(static_cast<std::size_t>(residual_count.value()));
  for (std::uint64_t index = 0; index < residual_count.value(); ++index) {
    Residual residual;
    const auto status = read_residual(reader, residual);
    if (!status.ok()) {
      return status.error();
    }
    out.residuals.push_back(std::move(residual));
  }

  const auto exclusion_count = reader.count_bounded(kMaxDecodedItems, 6);
  if (!exclusion_count.ok()) {
    return exclusion_count.error();
  }
  out.out_of_scope.reserve(static_cast<std::size_t>(exclusion_count.value()));
  for (std::uint64_t index = 0; index < exclusion_count.value(); ++index) {
    ScopeExclusion exclusion;
    const auto status = read_exclusion(reader, exclusion);
    if (!status.ok()) {
      return status.error();
    }
    out.out_of_scope.push_back(std::move(exclusion));
  }

  const auto plan_digest = reader.digest();
  if (!plan_digest.ok()) {
    return plan_digest.error();
  }
  out.plan_digest = plan_digest.value();
  return {};
}

[[nodiscard]] Result<void> read_idempotency(CanonicalReader& reader, IdempotencyRecord& out) {
  const auto key = read_identifier<IdempotencyKey>(reader);
  if (!key.ok()) {
    return key.error();
  }
  out.key = key.value();
  const auto digest = reader.digest();
  if (!digest.ok()) {
    return digest.error();
  }
  out.request_digest = digest.value();
  const auto plan_id = read_identifier<PlanId>(reader);
  if (!plan_id.ok()) {
    return plan_id.error();
  }
  out.plan_id = plan_id.value();
  const auto revision = reader.u64();
  if (!revision.ok()) {
    return revision.error();
  }
  if (revision.value() > 0xffffffffull) {
    return make_error(ErrorCode::Corrupt, "idempotency revision exceeds 32 bits");
  }
  out.revision = PlanRevision{static_cast<std::uint32_t>(revision.value())};
  const auto sequence = reader.u64();
  if (!sequence.ok()) {
    return sequence.error();
  }
  out.sequence = Sequence{sequence.value()};
  const auto outcome = read_enum<OutcomeKind>(reader);
  if (!outcome.ok()) {
    return outcome.error();
  }
  out.outcome = outcome.value();
  return {};
}

[[nodiscard]] Result<void> read_state_body(CanonicalReader& reader, StoreState& out,
                                           std::uint64_t max_plans) {
  const auto sequence = reader.u64();
  if (!sequence.ok()) {
    return sequence.error();
  }
  out.sequence = Sequence{sequence.value()};
  const auto epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.error();
  }
  out.epoch = Epoch{epoch.value()};
  const auto writer = read_identifier<WriterId>(reader);
  if (!writer.ok()) {
    return writer.error();
  }
  out.writer = writer.value();
  const auto opened_at = reader.i64();
  if (!opened_at.ok()) {
    return opened_at.error();
  }
  out.opened_at = UnixNanos{opened_at.value()};

  const auto plan_count = reader.count_bounded(max_plans, 100);
  if (!plan_count.ok()) {
    return plan_count.error();
  }
  out.plans.reserve(static_cast<std::size_t>(plan_count.value()));
  for (std::uint64_t index = 0; index < plan_count.value(); ++index) {
    Plan plan;
    const auto status = read_plan(reader, plan);
    if (!status.ok()) {
      return status.error();
    }
    const auto verified = plan.verify();
    if (!verified.ok()) {
      return verified.error();
    }
    out.plans.push_back(std::move(plan));
  }

  const auto key_count = reader.count_bounded(max_plans, 60);
  if (!key_count.ok()) {
    return key_count.error();
  }
  out.keys.reserve(static_cast<std::size_t>(key_count.value()));
  for (std::uint64_t index = 0; index < key_count.value(); ++index) {
    IdempotencyRecord record;
    const auto status = read_idempotency(reader, record);
    if (!status.ok()) {
      return status.error();
    }
    out.keys.push_back(std::move(record));
  }
  return {};
}

} // namespace

std::string store_state_file_name(Sequence sequence) {
  std::string digits = std::to_string(sequence.value());
  if (digits.size() < 16) {
    digits.insert(0, 16 - digits.size(), '0');
  }
  return std::string(kStoreStatePrefix) + digits + std::string(kStoreStateSuffix);
}

Result<Sequence> parse_state_file_name(std::string_view name) {
  if (name.size() < kStoreStatePrefix.size() + kStoreStateSuffix.size()) {
    return make_error(ErrorCode::Corrupt, "state file name is too short");
  }
  if (name.substr(0, kStoreStatePrefix.size()) != kStoreStatePrefix ||
      name.substr(name.size() - kStoreStateSuffix.size()) != kStoreStateSuffix) {
    return make_error(ErrorCode::Corrupt, "state file name does not match the format");
  }
  const std::string_view digits =
      name.substr(kStoreStatePrefix.size(), name.size() - kStoreStatePrefix.size() -
                                               kStoreStateSuffix.size());
  if (digits.empty() || digits.size() > 20) {
    return make_error(ErrorCode::Corrupt, "state file name has no sequence digits");
  }
  std::uint64_t value = 0;
  for (const char digit : digits) {
    if (digit < '0' || digit > '9') {
      return make_error(ErrorCode::Corrupt, "state file name contains a non-digit");
    }
    const std::uint64_t next = value * 10u + static_cast<std::uint64_t>(digit - '0');
    if (next < value) {
      return make_error(ErrorCode::Overflow, "state file sequence overflows 64 bits");
    }
    value = next;
  }
  return Sequence{value};
}

std::vector<std::byte> encode_store_state(const StoreState& state) {
  // The payload is self-describing: it opens with its own domain tag, so a
  // reader can tell what it decoded before it decodes it.
  CanonicalWriter payload_writer(domains::kStoreState);
  payload_writer.text(domains::kStoreState);
  state.encode(payload_writer);
  const std::vector<std::byte> payload = payload_writer.buffer();
  // The header records the digest of the payload bytes exactly as they appear
  // in the file, which is what a reader can recompute without knowing how the
  // payload was framed.
  const Digest payload_digest =
      digest_of_bytes(std::span<const std::byte>(payload.data(), payload.size()));

  std::vector<std::byte> header;
  header.reserve(kStateHeaderSize);
  header.insert(header.end(), kStateMagic.begin(), kStateMagic.end());
  put_u32(header, kStoreFormatVersion);
  put_u32(header, static_cast<std::uint32_t>(kStateHeaderSize));
  put_u64(header, state.sequence.value());
  put_u64(header, state.epoch.value());
  put_u64(header, static_cast<std::uint64_t>(payload.size()));
  write_digest(header, payload_digest);
  write_digest(header, digest_of_bytes(header));

  std::vector<std::byte> file;
  file.reserve(header.size() + payload.size() + kStateTrailerSize);
  file.insert(file.end(), header.begin(), header.end());
  file.insert(file.end(), payload.begin(), payload.end());
  const Digest trailer = digest_of_bytes(file);
  write_digest(file, trailer);
  return file;
}

Result<StoreState> decode_store_state(std::span<const std::byte> bytes) {
  if (bytes.size() < kStateHeaderSize + kStateTrailerSize) {
    return make_error(ErrorCode::Truncated, "state file is shorter than its fixed framing");
  }
  if (!magic_matches(bytes, kStateMagic)) {
    return make_error(ErrorCode::Corrupt, "state file magic does not match");
  }
  const std::uint32_t version = read_u32(bytes, 8);
  if (version != kStoreFormatVersion) {
    return make_error(ErrorCode::UnsupportedVersion,
                      "state file format version " + std::to_string(version) +
                          " is not supported");
  }
  if (read_u32(bytes, 12) != kStateHeaderSize) {
    return make_error(ErrorCode::Corrupt, "state file header size is not the expected size");
  }
  if (!(digest_of_bytes(bytes.subspan(0, 72)) == read_digest(bytes, 72))) {
    return make_error(ErrorCode::DigestMismatch, "state file header digest does not verify");
  }
  const std::uint64_t payload_length = read_u64(bytes, 32);
  const std::uint64_t expected = static_cast<std::uint64_t>(kStateHeaderSize) + payload_length +
                                 static_cast<std::uint64_t>(kStateTrailerSize);
  if (expected != bytes.size()) {
    return make_error(ErrorCode::Truncated,
                      "state file length " + std::to_string(bytes.size()) +
                          " does not match the declared length " + std::to_string(expected));
  }
  const auto payload = bytes.subspan(kStateHeaderSize, static_cast<std::size_t>(payload_length));
  if (!(digest_of_bytes(payload) == read_digest(bytes, 40))) {
    return make_error(ErrorCode::DigestMismatch, "state file payload digest does not verify");
  }
  if (!(digest_of_bytes(bytes.subspan(0, bytes.size() - kStateTrailerSize)) ==
        read_digest(bytes, bytes.size() - kStateTrailerSize))) {
    return make_error(ErrorCode::DigestMismatch, "state file trailer digest does not verify");
  }

  CanonicalReader reader(payload);
  const auto tag = reader.read_domain_tag();
  if (!tag.ok()) {
    return tag.error();
  }
  if (reader.domain_tag() != domains::kStoreState) {
    return make_error(ErrorCode::Corrupt, "state payload domain tag does not match");
  }
  StoreState state;
  const auto body = read_state_body(reader, state, kMaxDecodedItems);
  if (!body.ok()) {
    return body.error();
  }
  if (!reader.at_end()) {
    return make_error(ErrorCode::Corrupt, "state payload has trailing bytes");
  }
  if (state.sequence.value() != read_u64(bytes, 16)) {
    return make_error(ErrorCode::Corrupt, "state sequence disagrees with the file header");
  }
  if (state.epoch.value() != read_u64(bytes, 24)) {
    return make_error(ErrorCode::Corrupt, "state epoch disagrees with the file header");
  }
  return state;
}

std::vector<std::byte> encode_store_current(const StoreCurrent& current) {
  std::vector<std::byte> header;
  header.reserve(kCurrentHeaderSize);
  header.insert(header.end(), kCurrentMagic.begin(), kCurrentMagic.end());
  put_u32(header, kStoreFormatVersion);
  put_u32(header, static_cast<std::uint32_t>(kCurrentHeaderSize));
  put_u64(header, current.sequence.value());
  put_u64(header, static_cast<std::uint64_t>(current.state_file_name.size()));
  write_digest(header, current.state_digest);
  write_digest(header, digest_of_bytes(header));

  std::vector<std::byte> file = std::move(header);
  const auto* name_bytes = reinterpret_cast<const std::byte*>(current.state_file_name.data());
  file.insert(file.end(), name_bytes, name_bytes + current.state_file_name.size());
  return file;
}

Result<StoreCurrent> decode_store_current(std::span<const std::byte> bytes) {
  if (bytes.size() < kCurrentHeaderSize) {
    return make_error(ErrorCode::Truncated, "current pointer is shorter than its header");
  }
  if (!magic_matches(bytes, kCurrentMagic)) {
    return make_error(ErrorCode::Corrupt, "current pointer magic does not match");
  }
  const std::uint32_t version = read_u32(bytes, 8);
  if (version != kStoreFormatVersion) {
    return make_error(ErrorCode::UnsupportedVersion,
                      "current pointer format version " + std::to_string(version) +
                          " is not supported");
  }
  if (read_u32(bytes, 12) != kCurrentHeaderSize) {
    return make_error(ErrorCode::Corrupt, "current pointer header size is not the expected size");
  }
  if (!(digest_of_bytes(bytes.subspan(0, 64)) == read_digest(bytes, 64))) {
    return make_error(ErrorCode::DigestMismatch, "current pointer header digest does not verify");
  }
  const std::uint64_t name_length = read_u64(bytes, 24);
  if (bytes.size() != kCurrentHeaderSize + name_length) {
    return make_error(ErrorCode::Truncated,
                      "current pointer length does not match its declared file name length");
  }
  const auto* name_data = reinterpret_cast<const char*>(bytes.data() + kCurrentHeaderSize);
  StoreCurrent current;
  current.sequence = Sequence{read_u64(bytes, 16)};
  current.state_digest = read_digest(bytes, 32);
  current.state_file_name.assign(name_data, static_cast<std::size_t>(name_length));
  for (const char character : current.state_file_name) {
    const bool safe = (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'z') || character == '-' ||
                      character == '.';
    if (!safe) {
      return make_error(ErrorCode::Corrupt, "current pointer names an unsafe path");
    }
  }
  const auto parsed = parse_state_file_name(current.state_file_name);
  if (!parsed.ok()) {
    return parsed.error();
  }
  if (!(parsed.value() == current.sequence)) {
    return make_error(ErrorCode::Corrupt,
                      "current pointer names a state file whose sequence disagrees with it");
  }
  return current;
}

} // namespace rep::detail
