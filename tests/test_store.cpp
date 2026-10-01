// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The durable store: transactional publication, strict decoding, single-writer
// exclusion, and refusal of every corrupt or ambiguous state.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <rep/rep.hpp>

#include "synthetic_plan.hpp"
#include "temp_dir.hpp"
#include "testkit.hpp"

namespace {

[[nodiscard]] rep::Result<std::unique_ptr<rep::PlanStore>> open_store(
    const std::string& directory, std::size_t max_plans = 64) {
  rep::PlanStoreOptions options;
  options.directory = directory;
  options.max_plans = max_plans;
  return rep::PlanStore::open(options);
}

[[nodiscard]] rep::Result<rep::StoreState> next_state(const rep::StoreState& published,
                                                      std::size_t count) {
  std::vector<rep::Plan> plans = published.plans;
  std::vector<rep::IdempotencyRecord> keys = published.keys;
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint64_t sequence = published.sequence.value() + index + 1;
    const std::string key = "key-" + std::to_string(sequence);
    auto plan = reptest::synthetic_plan("planner-01", sequence, "lineage-1", key);
    if (!plan.ok()) {
      return plan.error();
    }
    keys.push_back(rep::IdempotencyRecord{rep::IdempotencyKey(key), plan.value().request_digest,
                                          plan.value().id, plan.value().revision,
                                          plan.value().sequence, rep::OutcomeKind::Planned});
    plans.push_back(plan.take());
  }
  return rep::StoreState::make(rep::Sequence{published.sequence.value() + count}, published.epoch,
                               published.writer, published.opened_at, std::move(plans),
                               std::move(keys), 64);
}

[[nodiscard]] std::vector<std::byte> read_bytes(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::byte> bytes;
  char chunk = 0;
  while (stream.get(chunk)) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(chunk)));
  }
  return bytes;
}

void write_bytes(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  for (const std::byte value : bytes) {
    stream.put(static_cast<char>(static_cast<unsigned char>(value)));
  }
}

} // namespace

REP_TEST(store, a_fresh_directory_has_no_published_generation) {
  reptest::TempDir directory("store-fresh");
  auto store = open_store(directory.path());
  REP_REQUIRE(store.ok());
  REP_CHECK_EQ(store.value()->published_sequence().value(), std::uint64_t{0});
  REP_CHECK_EQ(store.value()->published_epoch().value(), std::uint64_t{0});
  REP_CHECK(!store.value()->writer().empty());
  REP_CHECK(store.value()->state().plans.empty());
  REP_CHECK(store.value()->state().keys.empty());

  const rep::StoreStats stats = store.value()->stats();
  REP_CHECK_EQ(stats.format_version, rep::kStoreFormatVersion);
  REP_CHECK_EQ(stats.plan_count, std::size_t{0});
  REP_CHECK(stats.to_text().find("store sequence=0") != std::string::npos);
}

REP_TEST(store, commit_publishes_one_whole_generation) {
  reptest::TempDir directory("store-commit");
  {
    auto store = open_store(directory.path());
    REP_REQUIRE(store.ok());
    auto state = next_state(store.value()->state(), 2);
    REP_REQUIRE(state.ok());
    const auto committed = store.value()->commit(state.value());
    REP_REQUIRE(committed.ok());
    REP_CHECK_EQ(store.value()->published_sequence().value(), std::uint64_t{2});
    REP_CHECK_EQ(store.value()->state().plans.size(), std::size_t{2});
  }
  // A fresh process-level open sees exactly that generation.
  auto reopened = open_store(directory.path());
  REP_REQUIRE(reopened.ok());
  REP_CHECK_EQ(reopened.value()->published_sequence().value(), std::uint64_t{2});
  REP_REQUIRE(reopened.value()->state().plans.size() == 2);
  REP_CHECK(reopened.value()->state().plans[0].verify().ok());
  REP_CHECK(reopened.value()->state().plans[1].verify().ok());
  REP_CHECK(reopened.value()->state().find_key("key-1") != nullptr);
  REP_CHECK(reopened.value()->state().find_plan("planner-01-2") != nullptr);

  // Only one whole generation is kept: the store directory holds the lock, the
  // pointer and exactly one state file.
  std::size_t state_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
    const std::string name = entry.path().filename().string();
    if (name.rfind(std::string(rep::kStoreStatePrefix), 0) == 0) {
      ++state_files;
    }
  }
  REP_CHECK_EQ(state_files, std::size_t{1});
}

REP_TEST(store, publication_must_advance_the_store) {
  reptest::TempDir directory("store-monotonic");
  auto store = open_store(directory.path());
  REP_REQUIRE(store.ok());
  auto first = next_state(store.value()->state(), 1);
  REP_REQUIRE(first.ok());
  REP_REQUIRE(store.value()->commit(first.value()).ok());

  // Re-publishing the same generation moves nothing.
  const auto repeated = store.value()->commit(first.value());
  REP_CHECK(!repeated.ok());
  if (!repeated.ok()) {
    REP_CHECK(repeated.error().code == rep::ErrorCode::PreconditionFailed);
  }

  // Rolling the sequence back is refused even when the epoch advances.
  std::vector<rep::Plan> fewer(first.value().plans.begin(), first.value().plans.begin());
  std::vector<rep::IdempotencyRecord> no_keys;
  auto regressed = rep::StoreState::make(rep::Sequence{0}, rep::Epoch{9}, first.value().writer,
                                         first.value().opened_at, std::move(fewer),
                                         std::move(no_keys), 64);
  REP_REQUIRE(regressed.ok());
  const auto refused = store.value()->commit(regressed.value());
  REP_CHECK(!refused.ok());
  if (!refused.ok()) {
    REP_CHECK(refused.error().code == rep::ErrorCode::PreconditionFailed);
  }
  REP_CHECK_EQ(store.value()->published_sequence().value(), std::uint64_t{1});
}

REP_TEST(store, state_validation_refuses_inconsistent_bodies) {
  auto plan = reptest::synthetic_plan("planner-01", 1, "lineage-1", "key-1");
  REP_REQUIRE(plan.ok());

  auto no_writer = rep::StoreState::make(rep::Sequence{1}, rep::Epoch{1}, rep::WriterId{},
                                         rep::UnixNanos{0}, {plan.value()}, {}, 64);
  REP_CHECK(!no_writer.ok());
  if (!no_writer.ok()) {
    REP_CHECK(no_writer.error().code == rep::ErrorCode::InvalidIdentifier);
  }

  auto too_many = rep::StoreState::make(rep::Sequence{1}, rep::Epoch{1},
                                        rep::WriterId("writer-1"), rep::UnixNanos{0},
                                        {plan.value()}, {}, 0);
  REP_CHECK(!too_many.ok());

  rep::IdempotencyRecord dangling{rep::IdempotencyKey("key-x"), plan.value().request_digest,
                                  rep::PlanId("planner-01-9"), rep::PlanRevision{1},
                                  rep::Sequence{9}, rep::OutcomeKind::Planned};
  auto unknown_plan = rep::StoreState::make(rep::Sequence{1}, rep::Epoch{1},
                                            rep::WriterId("writer-1"), rep::UnixNanos{0},
                                            {plan.value()}, {dangling}, 64);
  REP_CHECK(!unknown_plan.ok());
  if (!unknown_plan.ok()) {
    REP_CHECK(unknown_plan.error().code == rep::ErrorCode::NotFound);
  }

  rep::IdempotencyRecord mismatched{rep::IdempotencyKey("key-1"), plan.value().request_digest,
                                    plan.value().id, rep::PlanRevision{1}, rep::Sequence{7},
                                    rep::OutcomeKind::Planned};
  auto wrong_sequence = rep::StoreState::make(rep::Sequence{1}, rep::Epoch{1},
                                              rep::WriterId("writer-1"), rep::UnixNanos{0},
                                              {plan.value()}, {mismatched}, 64);
  REP_CHECK(!wrong_sequence.ok());
  if (!wrong_sequence.ok()) {
    REP_CHECK(wrong_sequence.error().code == rep::ErrorCode::PreconditionFailed);
  }

  auto plan_ahead = rep::StoreState::make(rep::Sequence{0}, rep::Epoch{1},
                                          rep::WriterId("writer-1"), rep::UnixNanos{0},
                                          {plan.value()}, {}, 64);
  REP_CHECK(!plan_ahead.ok());
}

REP_TEST(store, canonical_state_file_names_are_zero_padded_and_ordered) {
  std::string previous;
  for (const std::uint64_t sequence : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{42},
                                       std::uint64_t{999999999999ULL}}) {
    const std::string name = rep::store_state_file_name(rep::Sequence{sequence});
    REP_CHECK_EQ(name.size(), std::size_t{10 + 16 + 4});
    REP_CHECK(name.rfind(std::string(rep::kStoreStatePrefix), 0) == 0);
    REP_CHECK(name.size() > rep::kStoreStateSuffix.size());
    REP_CHECK(name.compare(name.size() - rep::kStoreStateSuffix.size(),
                           rep::kStoreStateSuffix.size(),
                           std::string(rep::kStoreStateSuffix)) == 0);
    // Zero padding makes lexicographic order equal numeric order, which is what
    // makes the residue scan and the format self-consistent.
    if (!previous.empty()) {
      REP_CHECK(previous < name);
    }
    previous = name;
  }
  REP_CHECK_EQ(rep::store_state_file_name(rep::Sequence{7}),
               std::string("rep-state-0000000000000007.bin"));
}

REP_TEST(store, inspect_reads_without_taking_the_writer_lock) {
  reptest::TempDir directory("store-inspect");
  reptest::TempDir empty("store-inspect-empty");
  auto store = open_store(directory.path());
  REP_REQUIRE(store.ok());
  auto state = next_state(store.value()->state(), 1);
  REP_REQUIRE(state.ok());
  REP_REQUIRE(store.value()->commit(state.value()).ok());

  auto stats = rep::PlanStore::inspect(directory.path());
  REP_REQUIRE(stats.ok());
  REP_CHECK_EQ(stats.value().sequence.value(), std::uint64_t{1});
  REP_CHECK_EQ(stats.value().plan_count, std::size_t{1});
  REP_CHECK(stats.value().state_bytes > 0);

  auto empty_stats = rep::PlanStore::inspect(empty.path());
  REP_REQUIRE(empty_stats.ok());
  REP_CHECK_EQ(empty_stats.value().sequence.value(), std::uint64_t{0});
  REP_CHECK_EQ(empty_stats.value().plan_count, std::size_t{0});
}

REP_TEST(store, a_second_writer_is_refused) {
  reptest::TempDir directory("store-lock");
  auto first = open_store(directory.path());
  REP_REQUIRE(first.ok());
  auto second = open_store(directory.path());
  REP_CHECK(!second.ok());
  if (!second.ok()) {
    REP_CHECK(second.error().code == rep::ErrorCode::Locked);
  }
}

REP_TEST(store, repeated_open_and_close_keeps_exactly_one_generation) {
  // The store is passive: it publishes what it is given and never invents an
  // epoch of its own.  Advancing the control epoch is the engine's job, so
  // reopening repeatedly must return the same whole generation every time.
  reptest::TempDir directory("store-reopen");
  for (int round = 0; round < 5; ++round) {
    auto store = open_store(directory.path());
    REP_REQUIRE(store.ok());
    if (round == 0) {
      REP_CHECK_EQ(store.value()->published_epoch().value(), std::uint64_t{0});
      auto state = next_state(store.value()->state(), 1);
      REP_REQUIRE(state.ok());
      REP_REQUIRE(store.value()->commit(state.value()).ok());
    } else {
      REP_CHECK_EQ(store.value()->published_sequence().value(), std::uint64_t{1});
      REP_CHECK_EQ(store.value()->published_epoch().value(), std::uint64_t{0});
      REP_REQUIRE(store.value()->state().plans.size() == 1);
      REP_CHECK(store.value()->state().plans.front().verify().ok());
      REP_CHECK(store.value()->state().find_key("key-1") != nullptr);
    }
  }
}

REP_TEST(store, an_unsupported_format_version_is_refused) {
  reptest::TempDir directory("store-version");
  {
    auto store = open_store(directory.path());
    REP_REQUIRE(store.ok());
    auto state = next_state(store.value()->state(), 1);
    REP_REQUIRE(state.ok());
    REP_REQUIRE(store.value()->commit(state.value()).ok());
  }
  const std::string state_path =
      directory.child(rep::store_state_file_name(rep::Sequence{1}));
  std::vector<std::byte> bytes = read_bytes(state_path);
  REP_REQUIRE(bytes.size() > 16);
  bytes[8] = std::byte{9};   // format version
  write_bytes(state_path, bytes);
  auto reopened = open_store(directory.path());
  REP_CHECK(!reopened.ok());
}

REP_TEST(store, a_corrupt_current_pointer_is_refused) {
  reptest::TempDir directory("store-current");
  {
    auto store = open_store(directory.path());
    REP_REQUIRE(store.ok());
    auto state = next_state(store.value()->state(), 1);
    REP_REQUIRE(state.ok());
    REP_REQUIRE(store.value()->commit(state.value()).ok());
  }
  const std::string current_path = directory.child("rep-current");
  const std::vector<std::byte> good = read_bytes(current_path);
  REP_REQUIRE(good.size() > 8);

  const std::vector<std::vector<std::byte>> corruptions = {
      {},                                                     // empty
      std::vector<std::byte>(good.begin(), good.begin() + 8),  // header only
      [&good] {
        std::vector<std::byte> flipped = good;
        flipped[0] = std::byte{'X'};
        return flipped;
      }(),
      [&good] {
        std::vector<std::byte> flipped = good;
        flipped[8] = std::byte{7};   // format version
        return flipped;
      }(),
      [&good] {
        std::vector<std::byte> flipped = good;
        flipped[40] ^= std::byte{0x01};   // inside the state digest
        return flipped;
      }(),
      [&good] {
        std::vector<std::byte> extended = good;
        extended.push_back(std::byte{0});
        return extended;
      }(),
  };
  for (const std::vector<std::byte>& corrupted : corruptions) {
    write_bytes(current_path, corrupted);
    auto refused = open_store(directory.path());
    REP_CHECK(!refused.ok());
  }
  write_bytes(current_path, good);
  auto accepted = open_store(directory.path());
  REP_CHECK(accepted.ok());
}

REP_TEST(store, a_truncated_state_file_is_never_loaded) {
  reptest::TempDir directory("store-truncate");
  {
    auto store = open_store(directory.path());
    REP_REQUIRE(store.ok());
    auto state = next_state(store.value()->state(), 1);
    REP_REQUIRE(state.ok());
    REP_REQUIRE(store.value()->commit(state.value()).ok());
  }
  const std::string state_path = directory.child(rep::store_state_file_name(rep::Sequence{1}));
  const std::vector<std::byte> good = read_bytes(state_path);
  REP_REQUIRE(good.size() > 200);
  for (std::size_t length = 0; length < good.size(); ++length) {
    write_bytes(state_path, std::vector<std::byte>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length)));
    auto refused = open_store(directory.path());
    REP_CHECK_MSG(!refused.ok(), "a truncated state file was accepted");
    if (refused.ok()) {
      break;
    }
  }
  write_bytes(state_path, good);
  auto accepted = open_store(directory.path());
  REP_CHECK(accepted.ok());
}

REP_TEST(store, a_single_flipped_byte_anywhere_is_detected) {
  reptest::TempDir directory("store-bitflip");
  {
    auto store = open_store(directory.path());
    REP_REQUIRE(store.ok());
    auto state = next_state(store.value()->state(), 1);
    REP_REQUIRE(state.ok());
    REP_REQUIRE(store.value()->commit(state.value()).ok());
  }
  const std::string state_path = directory.child(rep::store_state_file_name(rep::Sequence{1}));
  const std::vector<std::byte> good = read_bytes(state_path);
  REP_REQUIRE(good.size() > 8);

  // Header, payload and trailer are each covered by their own digest.
  std::vector<std::size_t> offsets = {0, 7, 8, 12, 16, 24, 32, 40, 60, 72, 103, 104, 120,
                                      good.size() / 2, good.size() - 33, good.size() - 1};
  for (const std::size_t offset : offsets) {
    if (offset >= good.size()) {
      continue;
    }
    std::vector<std::byte> flipped = good;
    flipped[offset] ^= std::byte{0x01};
    write_bytes(state_path, flipped);
    auto refused = open_store(directory.path());
    REP_CHECK_MSG(!refused.ok(), "a flipped byte was accepted at offset " + std::to_string(offset));
  }
  write_bytes(state_path, good);
  auto accepted = open_store(directory.path());
  REP_CHECK(accepted.ok());
}

REP_TEST(store, trailing_garbage_is_refused) {
  reptest::TempDir directory("store-trailing");
  {
    auto store = open_store(directory.path());
    REP_REQUIRE(store.ok());
    auto state = next_state(store.value()->state(), 1);
    REP_REQUIRE(state.ok());
    REP_REQUIRE(store.value()->commit(state.value()).ok());
  }
  const std::string state_path = directory.child(rep::store_state_file_name(rep::Sequence{1}));
  std::vector<std::byte> bytes = read_bytes(state_path);
  bytes.push_back(std::byte{0x5a});
  bytes.push_back(std::byte{0x5a});
  write_bytes(state_path, bytes);
  auto refused = open_store(directory.path());
  REP_CHECK(!refused.ok());
}

REP_TEST(store, a_state_file_that_disagrees_with_its_pointer_is_refused) {
  reptest::TempDir directory("store-substitute");
  {
    auto store = open_store(directory.path());
    REP_REQUIRE(store.ok());
    auto state = next_state(store.value()->state(), 2);
    REP_REQUIRE(state.ok());
    REP_REQUIRE(store.value()->commit(state.value()).ok());
  }
  // Overwrite the referenced generation with the bytes of another generation
  // that happens to be well formed on its own.
  const std::string current_path = directory.child("rep-current");
  const std::string state_path = directory.child(rep::store_state_file_name(rep::Sequence{2}));
  const std::vector<std::byte> substituted = read_bytes(state_path);
  REP_REQUIRE(!substituted.empty());
  write_bytes(state_path, substituted);
  // Rebuild the same bytes minus one payload byte: still refused.
  std::vector<std::byte> damaged = substituted;
  const std::size_t middle = damaged.size() / 2;
  damaged[middle] ^= std::byte{0x80};
  write_bytes(state_path, damaged);
  auto refused = open_store(directory.path());
  REP_CHECK(!refused.ok());

  // A pointer that names a different sequence than the body claims is refused.
  write_bytes(state_path, substituted);
  const std::vector<std::byte> pointer = read_bytes(current_path);
  REP_REQUIRE(pointer.size() > 16);
  std::vector<std::byte> rewritten = pointer;
  rewritten[16] = std::byte{9};   // sequence byte
  write_bytes(current_path, rewritten);
  auto mismatch = open_store(directory.path());
  REP_CHECK(!mismatch.ok());
}

REP_TEST(store, residue_from_an_interrupted_publication_is_removed) {
  reptest::TempDir directory("store-residue");
  {
    auto store = open_store(directory.path());
    REP_REQUIRE(store.ok());
    auto state = next_state(store.value()->state(), 1);
    REP_REQUIRE(state.ok());
    REP_REQUIRE(store.value()->commit(state.value()).ok());
  }
  const std::string orphan = directory.child(rep::store_state_file_name(rep::Sequence{7}));
  const std::string staged = directory.child("rep-state-0000000000000008.bin.stage-1-2");
  write_bytes(orphan, std::vector<std::byte>{std::byte{1}, std::byte{2}});
  write_bytes(staged, std::vector<std::byte>{std::byte{3}});

  auto reopened = open_store(directory.path());
  REP_REQUIRE(reopened.ok());
  REP_CHECK(!std::filesystem::exists(orphan));
  REP_CHECK(!std::filesystem::exists(staged));
  REP_CHECK(std::filesystem::exists(directory.child(rep::store_state_file_name(rep::Sequence{1}))));
}

REP_TEST(store, a_directory_with_a_space_and_a_long_path_is_supported) {
  reptest::TempDir root("store-paths");
  std::string nested = root.path() + "\\a directory with spaces";
  for (int depth = 0; depth < 4; ++depth) {
    nested += "\\segment-" + std::to_string(depth) + "-padding-padding";
  }
  std::error_code ignored;
  std::filesystem::create_directories(nested, ignored);
  REP_REQUIRE(std::filesystem::is_directory(nested));
  auto store = open_store(nested);
  REP_REQUIRE(store.ok());
  auto state = next_state(store.value()->state(), 1);
  REP_REQUIRE(state.ok());
  REP_CHECK(store.value()->commit(state.value()).ok());
  REP_CHECK(std::filesystem::exists(std::filesystem::path(nested) / "rep-current"));
}

REP_TEST(store, a_missing_directory_is_reported_when_creation_is_disabled) {
  reptest::TempDir root("store-missing");
  const std::string absent = root.child("not-there");
  rep::PlanStoreOptions options;
  options.directory = absent;
  options.create_if_missing = false;
  auto refused = rep::PlanStore::open(options);
  REP_CHECK(!refused.ok());
  if (!refused.ok()) {
    REP_CHECK(refused.error().code == rep::ErrorCode::NotFound);
  }
}

REP_TEST_MAIN()
