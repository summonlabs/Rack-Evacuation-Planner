// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_STORE_HPP
#define REP_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "rep/canonical.hpp"
#include "rep/export.hpp"
#include "rep/plan.hpp"
#include "rep/status.hpp"
#include "rep/types.hpp"

namespace rep {

// The durable format identifier written into every published state file.
inline constexpr std::uint32_t kStoreFormatVersion = 1;
inline constexpr std::string_view kStoreFormatName = "rep-store-state";
inline constexpr std::string_view kStoreLockFileName = "rep-store.lock";
inline constexpr std::string_view kStoreCurrentFileName = "rep-current";
inline constexpr std::string_view kStoreStatePrefix = "rep-state-";
inline constexpr std::string_view kStoreStateSuffix = ".bin";

// Deterministic fault injection points on the publication path.  Each one
// terminates the process abruptly, exactly where a real process could die, so
// a separate process can prove what the store recovers to.  Production
// callers never set one.
enum class StoreFaultPoint : std::uint16_t {
  None = 0,
  // After the staged generation is written and flushed to the device.
  AfterStageWrite = 1,
  // After the staged generation was read back and verified.
  AfterStageVerify = 2,
  // After the generation file was atomically renamed into place, before the
  // commit point moved.
  AfterStatePublish = 3,
  // Before the CURRENT pointer is replaced: the new generation exists on disk
  // but is not yet authoritative.
  BeforeCurrentUpdate = 4,
  // After CURRENT names the new generation: the commit point has moved.
  AfterCurrentUpdate = 5,
};

// The exit code used by injected faults so a parent process can tell an
// injected death apart from an ordinary failure.
inline constexpr int kFaultInjectedExitCode = 93;

// Durable record of one committed idempotent operation.  This is what makes
// "resolve replay before stale-precondition rejection" survive a restart.
struct REP_API IdempotencyRecord {
  IdempotencyKey key;
  Digest request_digest;
  PlanId plan_id;
  PlanRevision revision;
  Sequence sequence;
  OutcomeKind outcome{OutcomeKind::Planned};

  void encode(CanonicalWriter& writer) const {
    writer.text(key.view());
    writer.digest(request_digest);
    writer.text(plan_id.view());
    revision.encode(writer);
    sequence.encode(writer);
    writer.u16(static_cast<std::uint16_t>(outcome));
  }
  friend bool operator==(const IdempotencyRecord& a, const IdempotencyRecord& b) noexcept {
    return a.key == b.key && a.request_digest == b.request_digest && a.plan_id == b.plan_id &&
           a.revision == b.revision && a.sequence == b.sequence && a.outcome == b.outcome;
  }
  friend auto operator<=>(const IdempotencyRecord& a, const IdempotencyRecord& b) noexcept {
    return a.key <=> b.key;
  }
};

// One whole authoritative generation.  Publication replaces the previous
// generation atomically; a partially written state is never referenced.
struct REP_API StoreState {
  Sequence sequence;
  Epoch epoch;
  WriterId writer;
  UnixNanos opened_at;
  std::vector<Plan> plans;                    // canonical: by sequence, unique
  std::vector<IdempotencyRecord> keys;        // canonical: by key, unique

  [[nodiscard]] static Result<StoreState> make(Sequence sequence, Epoch epoch, WriterId writer,
                                               UnixNanos opened_at, std::vector<Plan> plans,
                                               std::vector<IdempotencyRecord> keys,
                                               std::size_t max_plans = 4096);

  [[nodiscard]] Digest content_digest() const;
  void encode(CanonicalWriter& writer) const;

  [[nodiscard]] const Plan* find_plan(std::string_view plan_id) const noexcept;
  [[nodiscard]] const IdempotencyRecord* find_key(std::string_view key) const noexcept;
  [[nodiscard]] const Plan* latest_for_lineage(std::string_view lineage) const noexcept;

  friend bool operator==(const StoreState& a, const StoreState& b) noexcept {
    return a.sequence == b.sequence && a.epoch == b.epoch && a.writer == b.writer &&
           a.opened_at == b.opened_at && a.plans == b.plans && a.keys == b.keys;
  }
};

struct REP_API StoreStats {
  std::string format_name;
  std::uint32_t format_version{0};
  Sequence sequence;
  Epoch epoch;
  WriterId writer;
  UnixNanos opened_at;
  std::size_t plan_count{0};
  std::size_t idempotency_count{0};
  std::uint64_t state_bytes{0};
  std::string state_file;
  std::string directory;

  [[nodiscard]] std::string to_text() const;
};

struct REP_API PlanStoreOptions {
  std::filesystem::path directory;
  std::size_t max_plans{4096};
  // When false, open() reports a missing store directory instead of creating
  // it.
  bool create_if_missing{true};
};

// Durable, single-writer, transactional store for plan history.
//
//   * one exclusive OS-level writer lock for the whole open lifetime;
//   * staged write -> flush -> read-back verify -> atomic publish;
//   * CURRENT names exactly one whole generation and carries the digest of
//     that file, so a torn or partial state is never readable as current;
//   * refuse to open on anything corrupt, truncated, ambiguous, or from an
//     unsupported format version.
class REP_API PlanStore {
 public:
  [[nodiscard]] static Result<std::unique_ptr<PlanStore>> open(const PlanStoreOptions& options);
  ~PlanStore();

  PlanStore(const PlanStore&) = delete;
  PlanStore& operator=(const PlanStore&) = delete;
  PlanStore(PlanStore&&) = delete;
  PlanStore& operator=(PlanStore&&) = delete;

  // The generation verified during open().
  [[nodiscard]] const StoreState& state() const noexcept { return state_; }
  // True when this open inherited a published generation rather than starting
  // from an empty directory.  A generation with no plans still counts: it
  // carries the control epoch a successor must not silently reuse.
  [[nodiscard]] bool has_published_generation() const noexcept { return published_; }
  [[nodiscard]] Sequence published_sequence() const noexcept { return state_.sequence; }
  [[nodiscard]] Epoch published_epoch() const noexcept { return state_.epoch; }
  [[nodiscard]] WriterId writer() const noexcept { return state_.writer; }
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

  // Transactionally publishes one new whole generation.  Requires a strictly
  // greater sequence and a re-verified state body.
  [[nodiscard]] Result<void> commit(const StoreState& state);

  [[nodiscard]] StoreStats stats() const;

  // Read-only inspection that never takes the writer lock and never mutates.
  // Only guaranteed race-free against a live writer for the purposes of
  // reporting; a torn reference is reported as an error, never guessed at.
  [[nodiscard]] static Result<StoreStats> inspect(const std::filesystem::path& directory);

  // Test-only deterministic fault injection.
  void set_fault_point(StoreFaultPoint point) noexcept { fault_point_ = point; }
  [[nodiscard]] StoreFaultPoint fault_point() const noexcept { return fault_point_; }

 private:
  PlanStore() = default;

  std::filesystem::path directory_;
  StoreState state_;
  void* lock_handle_{nullptr};
  StoreFaultPoint fault_point_{StoreFaultPoint::None};
  std::size_t max_plans_{4096};
  bool published_{false};
};

// Canonical state file name for a sequence: "rep-state-<16 digits>.bin".
[[nodiscard]] REP_API std::string store_state_file_name(Sequence sequence);

} // namespace rep

#endif // REP_STORE_HPP
