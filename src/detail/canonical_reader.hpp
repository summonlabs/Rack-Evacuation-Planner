// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Strict reader for the canonical encoding.  Not installed: durable decoding
// is the store's business, not the public API's.
//
// Every read either succeeds and advances exactly the bytes it consumed, or
// fails.  There is no partial read, no default value, and no resynchronisation.

#ifndef REP_SRC_DETAIL_CANONICAL_READER_HPP
#define REP_SRC_DETAIL_CANONICAL_READER_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "rep/digest.hpp"
#include "rep/status.hpp"

namespace rep::detail {

class CanonicalReader {
 public:
  explicit CanonicalReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == bytes_.size(); }

  [[nodiscard]] Result<void> read_domain_tag() {
    const auto length = u64();
    if (!length.ok()) {
      return length.error();
    }
    if (length.value() > remaining()) {
      return make_error(ErrorCode::Truncated, "domain tag runs past the end of the buffer");
    }
    const auto* begin = reinterpret_cast<const char*>(bytes_.data() + offset_);
    domain_tag_.assign(begin, static_cast<std::size_t>(length.value()));
    offset_ += static_cast<std::size_t>(length.value());
    return {};
  }
  [[nodiscard]] const std::string& domain_tag() const noexcept { return domain_tag_; }

  [[nodiscard]] Result<std::uint8_t> u8() {
    if (remaining() < 1) {
      return make_error(ErrorCode::Truncated, "buffer ended while reading a byte");
    }
    const auto value = static_cast<std::uint8_t>(bytes_[offset_]);
    ++offset_;
    return value;
  }

  [[nodiscard]] Result<std::uint16_t> u16() {
    if (remaining() < 2) {
      return make_error(ErrorCode::Truncated, "buffer ended while reading a 16-bit value");
    }
    std::uint16_t value = 0;
    for (unsigned index = 0; index < 2; ++index) {
      value |= static_cast<std::uint16_t>(bytes_[offset_ + index]) << (index * 8u);
    }
    offset_ += 2;
    return value;
  }

  [[nodiscard]] Result<std::uint32_t> u32() {
    if (remaining() < 4) {
      return make_error(ErrorCode::Truncated, "buffer ended while reading a 32-bit value");
    }
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
      value |= static_cast<std::uint32_t>(bytes_[offset_ + index]) << (index * 8u);
    }
    offset_ += 4;
    return value;
  }

  [[nodiscard]] Result<std::uint64_t> u64() {
    if (remaining() < 8) {
      return make_error(ErrorCode::Truncated, "buffer ended while reading a 64-bit value");
    }
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
      value |= static_cast<std::uint64_t>(bytes_[offset_ + index]) << (index * 8u);
    }
    offset_ += 8;
    return value;
  }

  [[nodiscard]] Result<std::int64_t> i64() {
    const auto raw = u64();
    if (!raw.ok()) {
      return raw.error();
    }
    return static_cast<std::int64_t>(raw.value());
  }

  [[nodiscard]] Result<bool> boolean() {
    const auto raw = u8();
    if (!raw.ok()) {
      return raw.error();
    }
    if (raw.value() > 1) {
      return make_error(ErrorCode::Corrupt, "canonical boolean is neither 0 nor 1");
    }
    return raw.value() == 1;
  }

  [[nodiscard]] Result<std::string> text(std::uint64_t max_length) {
    const auto length = u64();
    if (!length.ok()) {
      return length.error();
    }
    if (length.value() > remaining()) {
      return make_error(ErrorCode::Truncated, "text runs past the end of the buffer");
    }
    if (length.value() > max_length) {
      return make_error(ErrorCode::LimitExceeded, "text exceeds the decoding limit");
    }
    const auto* begin = reinterpret_cast<const char*>(bytes_.data() + offset_);
    std::string value(begin, static_cast<std::size_t>(length.value()));
    offset_ += static_cast<std::size_t>(length.value());
    return value;
  }

  [[nodiscard]] Result<Digest> digest() {
    if (remaining() < kDigestBytes) {
      return make_error(ErrorCode::Truncated, "buffer ended while reading a digest");
    }
    Digest value;
    for (std::size_t index = 0; index < kDigestBytes; ++index) {
      value.bytes[index] = static_cast<std::uint8_t>(bytes_[offset_ + index]);
    }
    offset_ += kDigestBytes;
    return value;
  }

  // Reads an element count and refuses anything above the caller's limit
  // before allocating.
  [[nodiscard]] Result<std::uint64_t> count(std::uint64_t max_items) {
    const auto length = u64();
    if (!length.ok()) {
      return length.error();
    }
    if (length.value() > max_items) {
      return make_error(ErrorCode::LimitExceeded,
                        "container declares " + std::to_string(length.value()) +
                            " elements, limit is " + std::to_string(max_items));
    }
    return length;
  }

  // Refuses an element count that cannot possibly fit in the remaining bytes,
  // so a corrupt length cannot make the decoder allocate wildly.
  [[nodiscard]] Result<std::uint64_t> count_bounded(std::uint64_t max_items,
                                                    std::size_t min_bytes_per_item) {
    const auto length = count(max_items);
    if (!length.ok()) {
      return length.error();
    }
    if (min_bytes_per_item != 0 &&
        length.value() > remaining() / min_bytes_per_item) {
      return make_error(ErrorCode::Corrupt,
                        "container length is impossible for the remaining bytes");
    }
    return length;
  }

 private:
  std::span<const std::byte> bytes_;
  std::size_t offset_{0};
  std::string domain_tag_;
};

} // namespace rep::detail

#endif // REP_SRC_DETAIL_CANONICAL_READER_HPP
