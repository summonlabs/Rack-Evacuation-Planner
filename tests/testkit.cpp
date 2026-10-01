// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "testkit.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace reptest {
namespace detail {
namespace {

// The registry is the only mutable state in the framework.  Entries are
// appended during static initialization and only read afterwards, so the order
// is the order the REP_TEST macros appear in the translation unit.
struct Registry {
  struct Entry {
    std::string_view suite;
    std::string_view name;
    TestBody body;
  };

  std::vector<Entry> tests;
  std::string_view suite{};     // suite of the test now running
  std::string_view name{};      // name of the test now running
  std::size_t checks{0};        // checks the test now running has executed
  std::size_t total_checks{0};  // checks every test so far has executed
  bool failed{false};           // the test now running recorded a failure

  [[nodiscard]] std::string full_name() const {
    return std::string(suite) + '.' + std::string(name);
  }
};

[[nodiscard]] Registry& registry() {
  static Registry instance;
  return instance;
}

void report_failure(std::string_view file, int line, std::string_view label,
                    std::string_view expression, std::string_view detail) {
  Registry& state = registry();
  ++state.checks;
  state.failed = true;
  std::cout << state.full_name() << " (" << file << ':' << line << "): " << label << ": "
            << expression << " [checks run: " << state.checks << "]\n";
  if (!detail.empty()) {
    std::cout << "  " << detail << '\n';
  }
}

} // namespace

int register_test(std::string_view suite, std::string_view name, TestBody body) noexcept {
  Registry& state = registry();
  state.tests.push_back(Registry::Entry{suite, name, body});
  return static_cast<int>(state.tests.size());
}

void note_check() noexcept { ++registry().checks; }

void note_failure(std::string_view file, int line, std::string_view expression,
                  std::string_view detail) noexcept {
  report_failure(file, line, "FAILED", expression, detail);
}

void require_failed(std::string_view file, int line, std::string_view expression,
                    std::string_view detail) {
  report_failure(file, line, "REQUIRED", expression, detail);
  throw TestAbort{};
}

void note_info(std::string_view message) noexcept {
  std::cout << "# " << registry().full_name() << ": " << message << '\n';
}

} // namespace detail

int run_all(int argc, char** argv) {
  bool list_only = false;
  std::string filter;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--list") {
      list_only = true;
      continue;
    }
    if (argument == "--filter") {
      if (index + 1 >= argc) {
        std::cout << "run_all: --filter needs a substring\n";
        return 2;
      }
      ++index;
      filter = argv[index];
      continue;
    }
    std::cout << "run_all: unknown argument '" << argument << "'\n";
    return 2;
  }

  detail::Registry& state = detail::registry();
  if (list_only) {
    for (const detail::Registry::Entry& entry : state.tests) {
      std::cout << entry.suite << '.' << entry.name << '\n';
    }
    return 0;
  }

  std::size_t selected = 0;
  std::size_t failed = 0;
  for (const detail::Registry::Entry& entry : state.tests) {
    const std::string full = std::string(entry.suite) + '.' + std::string(entry.name);
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }

    ++selected;
    state.suite = entry.suite;
    state.name = entry.name;
    state.checks = 0;
    state.failed = false;
    try {
      entry.body();
    } catch (const detail::TestAbort&) {
      // REP_REQUIRE already recorded the failure: this test ends here.
    } catch (const std::exception& error) {
      state.failed = true;
      std::cout << full << ": FAILED: uncaught exception: " << error.what() << '\n';
    } catch (...) {
      state.failed = true;
      std::cout << full << ": FAILED: uncaught exception of unknown type\n";
    }
    state.total_checks += state.checks;
    std::cout << full << " ... " << (state.failed ? "FAILED" : "ok") << '\n';
    if (state.failed) {
      ++failed;
    }
  }

  if (selected == 0 && !filter.empty()) {
    std::cout << "# no test matches the filter '" << filter << "'\n";
  }
  std::cout << "tests=" << selected << " failed=" << failed << " checks=" << state.total_checks
            << '\n';
  return failed == 0 ? 0 : 1;
}

} // namespace reptest
