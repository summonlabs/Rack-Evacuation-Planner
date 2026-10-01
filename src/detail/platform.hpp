// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal platform services: durable file publication, exclusive writer
// locks, and strict reading.  Not installed.

#ifndef REP_SRC_DETAIL_PLATFORM_HPP
#define REP_SRC_DETAIL_PLATFORM_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "rep/status.hpp"

namespace rep::detail {

// Owns one OS file handle.  Non-copyable, movable, and releases the handle on
// destruction, including when the owning process is terminated abruptly,
// because the kernel closes every handle a dead process held.
class FileHandle {
 public:
  FileHandle() = default;
  explicit FileHandle(void* handle) noexcept : handle_(handle) {}
  ~FileHandle();

  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;

  [[nodiscard]] void* get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
  void reset() noexcept;

  // Gives up ownership of the handle.  Used where the handle outlives this
  // object, such as the writer lock owned by a longer-lived store.
  [[nodiscard]] void* release() noexcept;

 private:
  void* handle_{nullptr};
};

[[nodiscard]] std::string last_system_error();

// "pid-counter-nonce", unique enough for staging file names.
[[nodiscard]] std::string process_token();

[[nodiscard]] Result<bool> path_exists(const std::filesystem::path& path);
[[nodiscard]] Result<bool> is_directory(const std::filesystem::path& path);
// A reparse point (symlink, junction, mount point) is refused wherever the
// planner is about to trust a path, because it can redirect durable state.
[[nodiscard]] Result<bool> is_reparse_point(const std::filesystem::path& path);
[[nodiscard]] Result<void> ensure_directory(const std::filesystem::path& directory);
[[nodiscard]] Result<std::uint64_t> file_size(const std::filesystem::path& path);

// Reads a whole file, refusing anything larger than max_bytes.
[[nodiscard]] Result<std::vector<std::byte>> read_file(const std::filesystem::path& path,
                                                       std::uint64_t max_bytes);

// Creates or truncates the file, writes every byte, flushes to the device, and
// closes.  Not atomic: callers publish with replace_file_atomic.
[[nodiscard]] Result<void> write_file_durable(const std::filesystem::path& path,
                                              std::span<const std::byte> bytes);

// Atomically installs bytes at path, creating a staged file first and then
// replacing the destination in one filesystem operation.
[[nodiscard]] Result<void> write_file_atomic(const std::filesystem::path& path,
                                             std::span<const std::byte> bytes);

// Atomically replaces target with an already durable replacement file that
// lives in the same directory.
[[nodiscard]] Result<void> replace_file_atomic(const std::filesystem::path& target,
                                               const std::filesystem::path& replacement);

[[nodiscard]] Result<std::vector<std::string>> list_file_names(
    const std::filesystem::path& directory);
[[nodiscard]] Result<void> remove_file(const std::filesystem::path& path);

// Opens an exclusive whole-file lock and holds it until the handle dies.  A
// second process is refused with ErrorCode::Locked; abrupt holder death
// releases the lock in the kernel.
[[nodiscard]] Result<FileHandle> acquire_exclusive_lock(const std::filesystem::path& path);

} // namespace rep::detail

#endif // REP_SRC_DETAIL_PLATFORM_HPP
