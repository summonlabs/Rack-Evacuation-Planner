// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_DIGEST_HPP
#define REP_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "rep/export.hpp"

namespace rep {

inline constexpr std::size_t kDigestBytes = 32;

// SHA-256 (FIPS 180-4).  Implemented here so the planner has no third-party
// dependency and every digest in the system is reproducible by an independent
// implementation.  This is an integrity primitive, not an authentication
// primitive: it detects corruption and accidental substitution, and it is not
// used to authenticate an untrusted peer.
class REP_API Sha256 {
 public:
  Sha256() noexcept;

  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void update(std::span<const std::byte> data) noexcept;
  void update(std::string_view data) noexcept;

  // Consumes the hash state; a second call is a programming error and is
  // reported with an all-zero digest in release builds.
  [[nodiscard]] std::array<std::uint8_t, kDigestBytes> finish() noexcept;

 private:
  void process_block(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_{0};
  std::size_t buffered_{0};
  bool finished_{false};
};

// A 256-bit content digest.
struct Digest {
  std::array<std::uint8_t, kDigestBytes> bytes{};

  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] std::string_view to_hex(std::span<char, 64> scratch) const noexcept;

  // Strict hex decoding: exactly 64 hexadecimal characters, nothing else.
  [[nodiscard]] static std::optional<Digest> from_hex(std::string_view text) noexcept;

  friend bool operator==(const Digest& a, const Digest& b) noexcept {
    return a.bytes == b.bytes;
  }
  friend auto operator<=>(const Digest& a, const Digest& b) noexcept {
    return a.bytes <=> b.bytes;
  }
};

// Convenience: digest of a byte span under no domain separation.
[[nodiscard]] REP_API Digest sha256_of(std::span<const std::byte> data) noexcept;
[[nodiscard]] REP_API Digest sha256_of(std::string_view data) noexcept;

} // namespace rep

#endif // REP_DIGEST_HPP
