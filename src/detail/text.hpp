// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal strict text helpers. Not installed, not part of the public API.

#ifndef REP_SRC_DETAIL_TEXT_HPP
#define REP_SRC_DETAIL_TEXT_HPP

#include <charconv>
#include <cstdint>
#include <string_view>
#include <vector>

namespace rep::detail {

// Strict unsigned decimal: at least one digit, no sign, no whitespace, no
// trailing characters, and no wrap on overflow.
[[nodiscard]] inline bool parse_u64(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  const char* first = text.data();
  const char* last = text.data() + text.size();
  const auto result = std::from_chars(first, last, value, 10);
  if (result.ec != std::errc{} || result.ptr != last) {
    return false;
  }
  out = value;
  return true;
}

[[nodiscard]] inline bool parse_i64(std::string_view text, std::int64_t& out) noexcept {
  if (text.empty()) {
    return false;
  }
  std::int64_t value = 0;
  const char* first = text.data();
  const char* last = text.data() + text.size();
  const auto result = std::from_chars(first, last, value, 10);
  if (result.ec != std::errc{} || result.ptr != last) {
    return false;
  }
  out = value;
  return true;
}

[[nodiscard]] inline bool parse_u32(std::string_view text, std::uint32_t& out) noexcept {
  std::uint64_t wide = 0;
  if (!parse_u64(text, wide) || wide > 0xffffffffull) {
    return false;
  }
  out = static_cast<std::uint32_t>(wide);
  return true;
}

[[nodiscard]] inline bool parse_bool(std::string_view text, bool& out) noexcept {
  if (text == "true") {
    out = true;
    return true;
  }
  if (text == "false") {
    out = false;
    return true;
  }
  return false;
}

// Splits on a single delimiter; empty fields are preserved so callers can
// reject them explicitly rather than silently accepting malformed input.
[[nodiscard]] inline std::vector<std::string_view> split(std::string_view text, char delimiter) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t position = text.find(delimiter, start);
    if (position == std::string_view::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, position - start));
    start = position + 1;
  }
}

// Renders a byte as printable ASCII, escaping anything else as \xNN.
[[nodiscard]] inline std::string escape_bytes(std::string_view text, std::size_t limit = 32) {
  std::string result;
  const std::size_t count = text.size() < limit ? text.size() : limit;
  for (std::size_t i = 0; i < count; ++i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte >= 0x20 && byte <= 0x7e && byte != '\\') {
      result.push_back(static_cast<char>(byte));
    } else {
      constexpr char kHex[] = "0123456789abcdef";
      result.push_back('\\');
      result.push_back('x');
      result.push_back(kHex[byte >> 4u]);
      result.push_back(kHex[byte & 0x0fu]);
    }
  }
  if (text.size() > count) {
    result += "...";
  }
  return result;
}

} // namespace rep::detail

#endif // REP_SRC_DETAIL_TEXT_HPP
