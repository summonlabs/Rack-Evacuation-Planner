// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal durable format for one published store generation. Not installed.
//
// Layout of a state file (all integers little endian):
//
//   header (104 bytes)
//     magic            8   "REPSTOR1"
//     format_version   4   kStoreFormatVersion
//     header_size      4   104
//     sequence         8
//     epoch            8
//     payload_length   8
//     payload_digest  32   SHA-256 of the payload bytes
//     header_digest   32   SHA-256 of the preceding 72 header bytes
//   payload           N   canonical encoding of StoreStateContent
//   trailer_digest   32   SHA-256 of header || payload
//
// Decoding is strict: the file length must be exactly
// 104 + payload_length + 32, both digests must verify, the canonical payload
// must consume every payload byte, and the version must be supported.

#ifndef REP_SRC_DETAIL_STORE_FORMAT_HPP
#define REP_SRC_DETAIL_STORE_FORMAT_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rep/status.hpp"
#include "rep/store.hpp"
#include "rep/types.hpp"

namespace rep::detail {

// The pointer file CURRENT.  Layout (all integers little endian):
//
//   magic            8   "REPCURNT"
//   format_version   4
//   header_size      4   96
//   sequence         8
//   name_length      8
//   state_digest    32   SHA-256 of the entire referenced state file
//   header_digest   32   SHA-256 of the preceding 64 header bytes
//   name             N   "rep-state-<16 digits>.bin"
//
// CURRENT is the single commit point of the store: a generation exists only
// when CURRENT names it and carries the digest of its whole file.
struct StoreCurrent {
  Sequence sequence;
  Digest state_digest;
  std::string state_file_name;
};

[[nodiscard]] std::vector<std::byte> encode_store_state(const StoreState& state);
[[nodiscard]] Result<StoreState> decode_store_state(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_store_current(const StoreCurrent& current);
[[nodiscard]] Result<StoreCurrent> decode_store_current(std::span<const std::byte> bytes);

// Parses "rep-state-<16 digits>.bin" and returns the sequence it names.
[[nodiscard]] Result<Sequence> parse_state_file_name(std::string_view name);

// Sequence numbers whose canonical name is shorter than 16 digits are still
// accepted when reading, but every name this library writes is zero padded to
// 16 digits so that lexicographic order equals numeric order.
[[nodiscard]] std::string store_state_file_name(Sequence sequence);

} // namespace rep::detail

#endif // REP_SRC_DETAIL_STORE_FORMAT_HPP
