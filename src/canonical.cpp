// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/canonical.hpp"

#include <cstring>

namespace rep {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

} // namespace

CanonicalWriter::CanonicalWriter(std::string_view domain_tag) : domain_tag_(domain_tag) {}

CanonicalWriter& CanonicalWriter::u8(std::uint8_t value) {
  buffer_.push_back(static_cast<std::byte>(value));
  return *this;
}

CanonicalWriter& CanonicalWriter::u16(std::uint16_t value) {
  for (unsigned shift = 0; shift < 16; shift += 8) {
    buffer_.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
  }
  return *this;
}

CanonicalWriter& CanonicalWriter::u32(std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    buffer_.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
  }
  return *this;
}

CanonicalWriter& CanonicalWriter::u64(std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    buffer_.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
  }
  return *this;
}

CanonicalWriter& CanonicalWriter::i32(std::int32_t value) {
  return u32(static_cast<std::uint32_t>(value));
}

CanonicalWriter& CanonicalWriter::i64(std::int64_t value) {
  return u64(static_cast<std::uint64_t>(value));
}

CanonicalWriter& CanonicalWriter::boolean(bool value) {
  return u8(value ? std::uint8_t{1} : std::uint8_t{0});
}

CanonicalWriter& CanonicalWriter::byte(std::byte value) {
  buffer_.push_back(value);
  return *this;
}

CanonicalWriter& CanonicalWriter::bytes(std::span<const std::byte> value) {
  u64(static_cast<std::uint64_t>(value.size()));
  return raw(value);
}

CanonicalWriter& CanonicalWriter::raw(std::span<const std::byte> value) {
  buffer_.insert(buffer_.end(), value.begin(), value.end());
  return *this;
}

CanonicalWriter& CanonicalWriter::text(std::string_view value) {
  u64(static_cast<std::uint64_t>(value.size()));
  const auto* begin = reinterpret_cast<const std::byte*>(value.data());
  buffer_.insert(buffer_.end(), begin, begin + value.size());
  return *this;
}

CanonicalWriter& CanonicalWriter::digest(const Digest& value) {
  const auto* begin = reinterpret_cast<const std::byte*>(value.bytes.data());
  buffer_.insert(buffer_.end(), begin, begin + value.bytes.size());
  return *this;
}

Digest CanonicalWriter::finish() const noexcept {
  // SHA-256 over u64(len(domain_tag)) || domain_tag || encoded_bytes.
  Sha256 hasher;
  std::uint8_t length_bytes[8] = {};
  const auto length = static_cast<std::uint64_t>(domain_tag_.size());
  for (unsigned shift = 0; shift < 64; shift += 8) {
    length_bytes[shift / 8] = static_cast<std::uint8_t>((length >> shift) & 0xffu);
  }
  hasher.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(length_bytes), 8));
  hasher.update(std::string_view(domain_tag_));
  hasher.update(std::span<const std::byte>(buffer_.data(), buffer_.size()));
  return Digest{hasher.finish()};
}

std::string CanonicalWriter::to_hex() const {
  std::string result(buffer_.size() * 2, '0');
  for (std::size_t i = 0; i < buffer_.size(); ++i) {
    const auto value = static_cast<std::uint8_t>(buffer_[i]);
    result[i * 2] = kHexDigits[value >> 4u];
    result[i * 2 + 1] = kHexDigits[value & 0x0fu];
  }
  return result;
}

} // namespace rep
