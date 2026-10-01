// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/digest.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace rep {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::array<std::uint32_t, 8> kInitialState = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                                        0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                                        0x1f83d9abu, 0x5be0cd19u};

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

} // namespace

Sha256::Sha256() noexcept : state_(kInitialState) {}

void Sha256::process_block(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t i = 0; i < 16; ++i) {
    const std::size_t base = i * 4;
    schedule[i] = (static_cast<std::uint32_t>(block[base]) << 24u) |
                  (static_cast<std::uint32_t>(block[base + 1]) << 16u) |
                  (static_cast<std::uint32_t>(block[base + 2]) << 8u) |
                  static_cast<std::uint32_t>(block[base + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotate_right(schedule[i - 15], 7) ^
                             rotate_right(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3u);
    const std::uint32_t s1 = rotate_right(schedule[i - 2], 17) ^
                             rotate_right(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10u);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + sigma1 + choose + kRoundConstants[i] + schedule[i];
    const std::uint32_t sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sigma0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::byte> data) noexcept {
  if (finished_ || data.empty()) {
    return;
  }
  total_bytes_ += static_cast<std::uint64_t>(data.size());
  const auto* cursor = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t remaining = data.size();
  while (remaining > 0) {
    const std::size_t room = 64 - buffered_;
    const std::size_t take = remaining < room ? remaining : room;
    std::memcpy(buffer_.data() + buffered_, cursor, take);
    buffered_ += take;
    cursor += take;
    remaining -= take;
    if (buffered_ == 64) {
      process_block(buffer_.data());
      buffered_ = 0;
    }
  }
}

void Sha256::update(std::string_view data) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

std::array<std::uint8_t, kDigestBytes> Sha256::finish() noexcept {
  if (finished_) {
    return std::array<std::uint8_t, kDigestBytes>{};
  }
  finished_ = true;

  const std::uint64_t bit_length = total_bytes_ * 8u;

  buffer_[buffered_++] = 0x80u;
  if (buffered_ > 56) {
    while (buffered_ < 64) {
      buffer_[buffered_++] = 0x00u;
    }
    process_block(buffer_.data());
    buffered_ = 0;
  }
  while (buffered_ < 56) {
    buffer_[buffered_++] = 0x00u;
  }
  for (std::size_t i = 0; i < 8; ++i) {
    buffer_[56 + i] = static_cast<std::uint8_t>((bit_length >> ((7u - i) * 8u)) & 0xffu);
  }
  process_block(buffer_.data());

  std::array<std::uint8_t, kDigestBytes> result{};
  for (std::size_t i = 0; i < 8; ++i) {
    result[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24u) & 0xffu);
    result[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16u) & 0xffu);
    result[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8u) & 0xffu);
    result[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xffu);
  }
  return result;
}

bool Digest::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

namespace {
constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] constexpr int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}
} // namespace

std::string Digest::to_hex() const {
  std::string result(kDigestBytes * 2, '0');
  for (std::size_t i = 0; i < kDigestBytes; ++i) {
    result[i * 2] = kHexDigits[bytes[i] >> 4u];
    result[i * 2 + 1] = kHexDigits[bytes[i] & 0x0fu];
  }
  return result;
}

std::string_view Digest::to_hex(std::span<char, 64> scratch) const noexcept {
  for (std::size_t i = 0; i < kDigestBytes; ++i) {
    scratch[i * 2] = kHexDigits[bytes[i] >> 4u];
    scratch[i * 2 + 1] = kHexDigits[bytes[i] & 0x0fu];
  }
  return std::string_view(scratch.data(), scratch.size());
}

std::optional<Digest> Digest::from_hex(std::string_view text) noexcept {
  if (text.size() != kDigestBytes * 2) {
    return std::nullopt;
  }
  Digest result;
  for (std::size_t i = 0; i < kDigestBytes; ++i) {
    const int high = hex_value(text[i * 2]);
    const int low = hex_value(text[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    result.bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return result;
}

Digest sha256_of(std::span<const std::byte> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return Digest{hasher.finish()};
}

Digest sha256_of(std::string_view data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return Digest{hasher.finish()};
}

} // namespace rep
