// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Scratch directory helper for the durable-store tests.

#ifndef REP_TESTS_TEMP_DIR_HPP
#define REP_TESTS_TEMP_DIR_HPP

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace reptest {

// A scratch directory under the system temporary directory, removed on
// destruction.  The name is deterministic per process, so a directory left
// behind by a killed run can never be mistaken for fresh state: it is removed
// before use.
class TempDir {
 public:
  explicit TempDir(std::string_view label) {
    static std::atomic<unsigned> counter{0};
    const unsigned index = counter.fetch_add(1, std::memory_order_relaxed);
    path_ = (std::filesystem::temp_directory_path() /
             ("rep-test-" + std::string(label) + "-" + std::to_string(index)))
                .string();
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
    std::filesystem::create_directories(path_, ignored);
  }

  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string child(std::string_view name) const {
    return (std::filesystem::path(path_) / std::string(name)).string();
  }

 private:
  std::string path_;
};

} // namespace reptest

#endif // REP_TESTS_TEMP_DIR_HPP
