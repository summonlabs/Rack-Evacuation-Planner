// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "detail/platform.hpp"
#include "detail/store_format.hpp"
#include "rep/canonical.hpp"

namespace rep {
namespace {

// Refuses any state that is not internally consistent.  Publication and
// loading both go through here, so a corrupt body cannot become authoritative
// even if it is produced in-process.
[[nodiscard]] Result<void> validate_state(const StoreState& state, std::size_t max_plans) {
  if (state.writer.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "store state has no writer identity",
                      "writer");
  }
  if (state.plans.size() > max_plans) {
    return make_error(ErrorCode::LimitExceeded,
                      "store state holds " + std::to_string(state.plans.size()) +
                          " plans, limit is " + std::to_string(max_plans),
                      "plans");
  }
  if (state.keys.size() > max_plans) {
    return make_error(ErrorCode::LimitExceeded,
                      "store state holds " + std::to_string(state.keys.size()) +
                          " idempotency records, limit is " + std::to_string(max_plans),
                      "keys");
  }
  for (std::size_t index = 0; index < state.plans.size(); ++index) {
    const auto verified = state.plans[index].verify();
    if (!verified.ok()) {
      return verified.error();
    }
    if (state.plans[index].sequence.value() > state.sequence.value()) {
      return make_error(ErrorCode::PreconditionFailed,
                        "plan sequence is ahead of the store sequence", "plans");
    }
    if (index > 0 && !(state.plans[index - 1].sequence < state.plans[index].sequence)) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "store plans are not strictly ordered by sequence", "plans");
    }
  }
  std::set<std::string> plan_ids;
  std::map<std::string, Sequence> sequence_of;
  for (const Plan& plan : state.plans) {
    if (!plan_ids.insert(plan.id.str()).second) {
      return make_error(ErrorCode::DuplicateIdentity, "duplicate plan id " + plan.id.str(),
                        "plans");
    }
    sequence_of.emplace(plan.id.str(), plan.sequence);
  }
  for (std::size_t index = 0; index < state.keys.size(); ++index) {
    const IdempotencyRecord& record = state.keys[index];
    if (index > 0 && !(state.keys[index - 1].key < record.key)) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "store idempotency records are not strictly ordered", "keys");
    }
    const auto known = sequence_of.find(record.plan_id.str());
    if (known == sequence_of.end()) {
      return make_error(ErrorCode::NotFound,
                        "idempotency record names unknown plan " + record.plan_id.str(), "keys");
    }
    if (!(known->second == record.sequence)) {
      return make_error(ErrorCode::PreconditionFailed,
                        "idempotency record disagrees with the plan sequence", "keys");
    }
  }
  return {};
}

[[noreturn]] void inject_fault() { std::_Exit(kFaultInjectedExitCode); }

} // namespace

// ---------------------------------------------------------------------------
// StoreState
// ---------------------------------------------------------------------------

Result<StoreState> StoreState::make(Sequence sequence, Epoch epoch, WriterId writer,
                                    UnixNanos opened_at, std::vector<Plan> plans,
                                    std::vector<IdempotencyRecord> keys, std::size_t max_plans) {
  std::sort(plans.begin(), plans.end(),
            [](const Plan& a, const Plan& b) { return a.sequence < b.sequence; });
  std::sort(keys.begin(), keys.end(),
            [](const IdempotencyRecord& a, const IdempotencyRecord& b) { return a.key < b.key; });
  StoreState state;
  state.sequence = sequence;
  state.epoch = epoch;
  state.writer = std::move(writer);
  state.opened_at = opened_at;
  state.plans = std::move(plans);
  state.keys = std::move(keys);
  const auto valid = validate_state(state, max_plans);
  if (!valid.ok()) {
    return valid.error();
  }
  return state;
}

Digest StoreState::content_digest() const { return digest_of(domains::kStoreState, *this); }

void StoreState::encode(CanonicalWriter& out) const {
  sequence.encode(out);
  epoch.encode(out);
  out.text(writer.view());
  opened_at.encode(out);
  encode_sequence(out, plans);
  encode_sequence(out, keys);
}

const Plan* StoreState::find_plan(std::string_view plan_id) const noexcept {
  for (const Plan& plan : plans) {
    if (plan.id.view() == plan_id) {
      return &plan;
    }
  }
  return nullptr;
}

const IdempotencyRecord* StoreState::find_key(std::string_view key) const noexcept {
  for (const IdempotencyRecord& record : keys) {
    if (record.key.view() == key) {
      return &record;
    }
  }
  return nullptr;
}

const Plan* StoreState::latest_for_lineage(std::string_view lineage) const noexcept {
  const Plan* latest = nullptr;
  for (const Plan& plan : plans) {
    if (plan.lineage.view() != lineage) {
      continue;
    }
    if (latest == nullptr || latest->revision < plan.revision) {
      latest = &plan;
    }
  }
  return latest;
}

std::string StoreStats::to_text() const {
  std::string out;
  out += "store format=" + format_name + " version=" + std::to_string(format_version) + "\n";
  out += "store directory=" + directory + " state_file=" + state_file + "\n";
  out += "store sequence=" + std::to_string(sequence.value()) +
         " epoch=" + std::to_string(epoch.value()) + " writer=" + writer.str() + "\n";
  out += "store plans=" + std::to_string(plan_count) +
         " idempotency_records=" + std::to_string(idempotency_count) +
         " state_bytes=" + std::to_string(state_bytes) + "\n";
  return out;
}

// ---------------------------------------------------------------------------
// PlanStore
// ---------------------------------------------------------------------------

PlanStore::~PlanStore() {
  if (lock_handle_ != nullptr) {
    const detail::FileHandle owner(lock_handle_);
    // The destructor releases the OS lock.  Abrupt process death does the same
    // thing in the kernel, which is what makes the exclusion crash-safe.
  }
  lock_handle_ = nullptr;
}

Result<std::unique_ptr<PlanStore>> PlanStore::open(const PlanStoreOptions& options) {
  if (options.directory.empty()) {
    return make_error(ErrorCode::InvalidArgument, "store directory must not be empty",
                      "directory");
  }
  const auto directory_reparse = detail::is_reparse_point(options.directory);
  if (!directory_reparse.ok()) {
    return directory_reparse.error();
  }
  if (directory_reparse.value()) {
    return make_error(ErrorCode::ReparsePoint,
                      "store directory is a reparse point, which can redirect durable state",
                      options.directory.string());
  }
  const auto present = detail::path_exists(options.directory);
  if (!present.ok()) {
    return present.error();
  }
  if (!present.value()) {
    if (!options.create_if_missing) {
      return make_error(ErrorCode::NotFound, "store directory does not exist",
                        options.directory.string());
    }
    const auto created = detail::ensure_directory(options.directory);
    if (!created.ok()) {
      return created.error();
    }
  }
  const auto is_dir = detail::is_directory(options.directory);
  if (!is_dir.ok()) {
    return is_dir.error();
  }
  if (!is_dir.value()) {
    return make_error(ErrorCode::NotADirectory, "store path is not a directory",
                      options.directory.string());
  }

  std::unique_ptr<PlanStore> store(new PlanStore());
  store->directory_ = options.directory;
  store->max_plans_ = options.max_plans;

  auto lock = detail::acquire_exclusive_lock(options.directory / std::string(kStoreLockFileName));
  if (!lock.ok()) {
    return lock.error();
  }
  detail::FileHandle lock_owner = lock.take();
  store->lock_handle_ = lock_owner.release();

  // None of the files this store trusts may be a reparse point: a junction or
  // symlink in the store directory could redirect durable state.
  for (const std::string_view name :
       {kStoreLockFileName, kStoreCurrentFileName, kStoreStatePrefix}) {
    if (name == kStoreStatePrefix) {
      continue;
    }
    const std::filesystem::path candidate = options.directory / std::string(name);
    const auto reparse = detail::is_reparse_point(candidate);
    if (!reparse.ok()) {
      return reparse.error();
    }
    if (reparse.value()) {
      return make_error(ErrorCode::ReparsePoint,
                        "store file is a reparse point: " + candidate.string(),
                        candidate.string());
    }
  }

  const std::filesystem::path current_path = options.directory / std::string(kStoreCurrentFileName);
  const auto current_present = detail::path_exists(current_path);
  if (!current_present.ok()) {
    return current_present.error();
  }

  if (!current_present.value()) {
    store->state_.sequence = Sequence{0};
    store->state_.epoch = Epoch{0};
    store->state_.writer = WriterId(detail::process_token());
    store->state_.opened_at = UnixNanos{0};
    return store;
  }

  const auto current_bytes = detail::read_file(current_path, 4096);
  if (!current_bytes.ok()) {
    return current_bytes.error();
  }
  const auto current = detail::decode_store_current(current_bytes.value());
  if (!current.ok()) {
    return current.error();
  }
  const std::filesystem::path state_path = options.directory / current.value().state_file_name;
  const auto state_bytes = detail::read_file(state_path, 512u * 1024u * 1024u);
  if (!state_bytes.ok()) {
    return state_bytes.error();
  }
  Sha256 hasher;
  hasher.update(state_bytes.value());
  const Digest file_digest{hasher.finish()};
  if (!(file_digest == current.value().state_digest)) {
    return make_error(ErrorCode::DigestMismatch,
                      "the referenced state file does not match its published digest",
                      state_path.string());
  }
  const auto state = detail::decode_store_state(state_bytes.value());
  if (!state.ok()) {
    return state.error();
  }
  if (!(state.value().sequence == current.value().sequence)) {
    return make_error(ErrorCode::Corrupt,
                      "the current pointer and the state file disagree about the sequence");
  }
  const auto valid = validate_state(state.value(), options.max_plans);
  if (!valid.ok()) {
    return valid.error();
  }
  store->state_ = state.value();
  store->published_ = true;

  // Only one whole generation is authoritative.  Anything else in the store
  // directory is residue from an interrupted publication and is removed.
  const auto names = detail::list_file_names(options.directory);
  if (!names.ok()) {
    return names.error();
  }
  for (const std::string& name : names.value()) {
    if (name == kStoreCurrentFileName || name == kStoreLockFileName) {
      continue;
    }
    const bool is_state = name.rfind(std::string(kStoreStatePrefix), 0) == 0;
    const bool is_residue = name.find(".stage-") != std::string::npos;
    if (!is_state && !is_residue) {
      continue;
    }
    if (is_state && name == current.value().state_file_name) {
      continue;
    }
    const auto removed = detail::remove_file(options.directory / name);
    if (!removed.ok()) {
      return removed.error();
    }
  }
  return store;
}

Result<void> PlanStore::commit(const StoreState& state) {
  // A published generation must never move a counter backwards, and it must
  // move the store forward.  Either the plan sequence or the control epoch may
  // advance: the first plan of a new incarnation advances both.
  const bool sequence_regressed = state.sequence < state_.sequence;
  const bool epoch_regressed = state.epoch < state_.epoch;
  const bool advanced = (state_.sequence < state.sequence) || (state_.epoch < state.epoch);
  if (sequence_regressed || epoch_regressed || !advanced) {
    return make_error(ErrorCode::PreconditionFailed,
                      "a published generation must advance the store without moving any "
                      "counter backwards",
                      "sequence");
  }
  const auto valid = validate_state(state, max_plans_);
  if (!valid.ok()) {
    return valid.error();
  }

  const std::string file_name = detail::store_state_file_name(state.sequence);
  const std::filesystem::path final_path = directory_ / file_name;
  const std::filesystem::path staged_path =
      directory_ / (file_name + ".stage-" + detail::process_token());

  const std::vector<std::byte> bytes = detail::encode_store_state(state);
  const auto written = detail::write_file_durable(staged_path, bytes);
  if (!written.ok()) {
    return written.error();
  }
  if (fault_point_ == StoreFaultPoint::AfterStageWrite) {
    inject_fault();
  }

  // Read the staged generation back and verify it before it can become
  // authoritative.  A short write, a torn write, or a bad digest stops here.
  const auto read_back = detail::read_file(staged_path, 512u * 1024u * 1024u);
  if (!read_back.ok()) {
    const auto ignored = detail::remove_file(staged_path);
    (void)ignored;
    return read_back.error();
  }
  if (!(read_back.value() == bytes)) {
    const auto ignored = detail::remove_file(staged_path);
    (void)ignored;
    return make_error(ErrorCode::Corrupt, "the staged generation did not read back identically",
                      staged_path.string());
  }
  const auto decoded = detail::decode_store_state(read_back.value());
  if (!decoded.ok()) {
    const auto ignored = detail::remove_file(staged_path);
    (void)ignored;
    return decoded.error();
  }
  if (!(decoded.value() == state)) {
    const auto ignored = detail::remove_file(staged_path);
    (void)ignored;
    return make_error(ErrorCode::Corrupt,
                      "the staged generation decoded to different state than it was given",
                      staged_path.string());
  }
  if (fault_point_ == StoreFaultPoint::AfterStageVerify) {
    inject_fault();
  }

  const auto published = detail::replace_file_atomic(final_path, staged_path);
  if (!published.ok()) {
    const auto ignored = detail::remove_file(staged_path);
    (void)ignored;
    return published.error();
  }
  if (fault_point_ == StoreFaultPoint::AfterStatePublish) {
    inject_fault();
  }

  Sha256 hasher;
  hasher.update(std::span<const std::byte>(bytes.data(), bytes.size()));
  const detail::StoreCurrent current{state.sequence, Digest{hasher.finish()}, file_name};

  if (fault_point_ == StoreFaultPoint::BeforeCurrentUpdate) {
    inject_fault();
  }
  const auto pointer = detail::write_file_atomic(
      directory_ / std::string(kStoreCurrentFileName), detail::encode_store_current(current));
  if (!pointer.ok()) {
    return pointer.error();
  }
  if (fault_point_ == StoreFaultPoint::AfterCurrentUpdate) {
    inject_fault();
  }

  // The previous generation is no longer referenced by anything.
  const std::string previous_name = detail::store_state_file_name(state_.sequence);
  if (previous_name != file_name) {
    const auto ignored = detail::remove_file(directory_ / previous_name);
    (void)ignored;
  }
  state_ = state;
  return {};
}

StoreStats PlanStore::stats() const {
  StoreStats stats;
  stats.format_name = std::string(kStoreFormatName);
  stats.format_version = kStoreFormatVersion;
  stats.sequence = state_.sequence;
  stats.epoch = state_.epoch;
  stats.writer = state_.writer;
  stats.opened_at = state_.opened_at;
  stats.plan_count = state_.plans.size();
  stats.idempotency_count = state_.keys.size();
  stats.directory = directory_.string();
  stats.state_file = detail::store_state_file_name(state_.sequence);
  const auto size = detail::file_size(directory_ / stats.state_file);
  stats.state_bytes = size.ok() ? size.value() : 0;
  return stats;
}

Result<StoreStats> PlanStore::inspect(const std::filesystem::path& directory) {
  const auto reparse = detail::is_reparse_point(directory);
  if (!reparse.ok()) {
    return reparse.error();
  }
  if (reparse.value()) {
    return make_error(ErrorCode::ReparsePoint,
                      "store directory is a reparse point, which can redirect durable state",
                      directory.string());
  }
  const auto is_dir = detail::is_directory(directory);
  if (!is_dir.ok()) {
    return is_dir.error();
  }
  if (!is_dir.value()) {
    return make_error(ErrorCode::NotADirectory,
                      "store path is not a directory: " + directory.string(),
                      directory.string());
  }
  const std::filesystem::path current_path = directory / std::string(kStoreCurrentFileName);
  const auto present = detail::path_exists(current_path);
  if (!present.ok()) {
    return present.error();
  }
  if (!present.value()) {
    StoreStats stats;
    stats.format_name = std::string(kStoreFormatName);
    stats.format_version = kStoreFormatVersion;
    stats.directory = directory.string();
    return stats;
  }
  const auto current_bytes = detail::read_file(current_path, 4096);
  if (!current_bytes.ok()) {
    return current_bytes.error();
  }
  const auto current = detail::decode_store_current(current_bytes.value());
  if (!current.ok()) {
    return current.error();
  }
  const std::filesystem::path state_path = directory / current.value().state_file_name;
  const auto state_bytes = detail::read_file(state_path, 512u * 1024u * 1024u);
  if (!state_bytes.ok()) {
    return state_bytes.error();
  }
  Sha256 inspector;
  inspector.update(std::span<const std::byte>(state_bytes.value().data(), state_bytes.value().size()));
  if (!(Digest{inspector.finish()} == current.value().state_digest)) {
    return make_error(ErrorCode::DigestMismatch,
                      "the referenced state file does not match its published digest",
                      state_path.string());
  }
  const auto state = detail::decode_store_state(state_bytes.value());
  if (!state.ok()) {
    return state.error();
  }
  StoreStats stats;
  stats.format_name = std::string(kStoreFormatName);
  stats.format_version = kStoreFormatVersion;
  stats.sequence = state.value().sequence;
  stats.epoch = state.value().epoch;
  stats.writer = state.value().writer;
  stats.opened_at = state.value().opened_at;
  stats.plan_count = state.value().plans.size();
  stats.idempotency_count = state.value().keys.size();
  stats.state_bytes = static_cast<std::uint64_t>(state_bytes.value().size());
  stats.state_file = current.value().state_file_name;
  stats.directory = directory.string();
  return stats;
}

std::string store_state_file_name(Sequence sequence) {
  return detail::store_state_file_name(sequence);
}

} // namespace rep
