// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Real independent OS processes: single-writer exclusion, abrupt holder death,
// crash consistency at every durable publication boundary, and epoch fencing
// across incarnations.  Nothing here shares memory with the mutator.
//
// The helper executable is located through REP_FAULT_CHILD (set by the test
// registration), then the working directory, then the test binary directory.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <rep/rep.hpp>

#include "synthetic_plan.hpp"
#include "temp_dir.hpp"
#include "testkit.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

#if defined(_WIN32)

[[nodiscard]] std::string wide_to_utf8(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
  std::string result(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(),
                        needed, nullptr, nullptr);
  return result;
}

[[nodiscard]] std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(),
                        needed);
  return result;
}

[[nodiscard]] std::string executable_directory() {
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(),
                                               static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      return std::string();
    }
    if (written < buffer.size()) {
      buffer.resize(written);
      break;
    }
    buffer.resize(buffer.size() * 2);
  }
  return wide_to_utf8(std::filesystem::path(buffer).parent_path().wstring());
}

// Resolves the helper executable, or an empty string when it cannot be found.
[[nodiscard]] std::string child_executable() {
  const char* declared = std::getenv("REP_FAULT_CHILD");
  if (declared != nullptr && *declared != '\0' && std::filesystem::exists(declared)) {
    return declared;
  }
  const std::string local = (std::filesystem::current_path() / "rep_fault_child.exe").string();
  if (std::filesystem::exists(local)) {
    return local;
  }
  const std::string beside = (std::filesystem::path(executable_directory()) /
                              "rep_fault_child.exe")
                                 .string();
  if (std::filesystem::exists(beside)) {
    return beside;
  }
  return std::string();
}

// One independent child process, observed through anonymous pipes.  Reads block
// until the child writes or closes the pipe, so there is no polling, no sleep,
// and no timeout anywhere in this harness.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() { release(); }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] bool launch(const std::string& executable,
                            const std::vector<std::string>& arguments) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    HANDLE in_read = nullptr;
    HANDLE in_write = nullptr;
    if (::CreatePipe(&out_read, &out_write, &attributes, 0) == 0) {
      return false;
    }
    if (::CreatePipe(&in_read, &in_write, &attributes, 0) == 0) {
      ::CloseHandle(out_read);
      ::CloseHandle(out_write);
      return false;
    }
    ::SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);

    std::wstring command_line = L"\"" + utf8_to_wide(executable) + L"\"";
    for (const std::string& argument : arguments) {
      command_line += L" \"" + utf8_to_wide(argument) + L"\"";
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = out_write;
    startup.hStdError = out_write;
    startup.hStdInput = in_read;

    PROCESS_INFORMATION information{};
    const BOOL created = ::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                          &information);
    ::CloseHandle(out_write);
    ::CloseHandle(in_read);
    if (created == 0) {
      ::CloseHandle(out_read);
      ::CloseHandle(in_write);
      return false;
    }
    ::CloseHandle(information.hThread);
    process_ = information.hProcess;
    output_ = out_read;
    input_ = in_write;
    return true;
  }

  // Reads one line, blocking until the child produces it or closes the pipe.
  [[nodiscard]] std::string read_line() {
    std::string line;
    while (true) {
      char character = 0;
      DWORD read = 0;
      if (::ReadFile(output_, &character, 1, &read, nullptr) == 0 || read == 0) {
        eof_ = true;
        return line;
      }
      if (character == '\n') {
        return line;
      }
      line.push_back(character);
    }
  }

  [[nodiscard]] std::string read_rest() {
    std::string rest;
    while (!eof_) {
      const std::string line = read_line();
      if (line.empty() && eof_) {
        break;
      }
      rest += line;
      rest.push_back('\n');
    }
    return rest;
  }

  [[nodiscard]] bool wait_for_exit(DWORD& exit_code) {
    if (process_ == nullptr) {
      return false;
    }
    if (::WaitForSingleObject(process_, INFINITE) != WAIT_OBJECT_0) {
      return false;
    }
    if (::GetExitCodeProcess(process_, &exit_code) == 0) {
      return false;
    }
    return true;
  }

  // Terminates the process abruptly: no cleanup, no flush, no destructor.
  void kill() {
    if (process_ != nullptr) {
      ::TerminateProcess(process_, 0xC0000001u);
    }
  }

  void release() {
    if (process_ != nullptr) {
      ::CloseHandle(process_);
      process_ = nullptr;
    }
    if (output_ != nullptr) {
      ::CloseHandle(output_);
      output_ = nullptr;
    }
    if (input_ != nullptr) {
      ::CloseHandle(input_);
      input_ = nullptr;
    }
  }

 private:
  HANDLE process_{nullptr};
  HANDLE output_{nullptr};
  HANDLE input_{nullptr};
  bool eof_{false};
};

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

#endif // _WIN32

} // namespace

#if defined(_WIN32)

REP_TEST(multiprocess, a_second_process_is_refused_and_abrupt_death_releases_the_lock) {
  const std::string executable = child_executable();
  REP_REQUIRE_MSG(!executable.empty(), "rep_fault_child executable was not found");
  reptest::TempDir directory("mp-lock");

  // Process A takes the writer lock and blocks on its stdin.
  ChildProcess holder;
  REP_REQUIRE_MSG(holder.launch(executable, {"hold-lock", directory.path()}),
                  "could not start the holder process");
  const std::string held = holder.read_line();
  REP_CHECK_MSG(contains(held, "locked"), "holder did not report the lock: " + held);

  // Process B is an independent process: the kernel must refuse it.
  ChildProcess rival;
  REP_REQUIRE_MSG(rival.launch(executable, {"hold-lock", directory.path()}),
                  "could not start the rival process");
  const std::string refusal = rival.read_line();
  const std::string refusal_detail = rival.read_rest();
  DWORD rival_exit = 0;
  REP_REQUIRE(rival.wait_for_exit(rival_exit));
  REP_CHECK(rival_exit != 0);
  REP_CHECK_MSG(contains(refusal + refusal_detail, "locked"),
                "rival was not refused: " + refusal + refusal_detail);
  rival.release();

  // Abrupt death: no cleanup handler runs, yet the kernel must release the lock.
  holder.kill();
  DWORD holder_exit = 0;
  REP_REQUIRE(holder.wait_for_exit(holder_exit));
  holder.release();

  ChildProcess successor;
  REP_REQUIRE_MSG(successor.launch(executable, {"hold-lock", directory.path()}),
                  "could not start the successor process");
  const std::string acquired = successor.read_line();
  REP_CHECK_MSG(contains(acquired, "locked"),
                "the lock did not survive the holder's death: " + acquired);
  successor.kill();
  DWORD successor_exit = 0;
  REP_REQUIRE(successor.wait_for_exit(successor_exit));
  successor.release();
}

REP_TEST(multiprocess, a_child_commit_is_recovered_by_an_independent_parent) {
  const std::string executable = child_executable();
  REP_REQUIRE_MSG(!executable.empty(), "rep_fault_child executable was not found");
  reptest::TempDir directory("mp-commit");

  ChildProcess writer;
  REP_REQUIRE(writer.launch(executable, {"commit", directory.path(), "3"}));
  const std::string result = writer.read_line();
  DWORD exit_code = 0;
  REP_REQUIRE(writer.wait_for_exit(exit_code));
  REP_CHECK_EQ(exit_code, DWORD{0});
  REP_CHECK_MSG(contains(result, "committed sequence=3"), "unexpected child result: " + result);
  writer.release();

  auto stats = rep::PlanStore::inspect(directory.path());
  REP_REQUIRE(stats.ok());
  REP_CHECK_EQ(stats.value().sequence.value(), std::uint64_t{3});
  REP_CHECK_EQ(stats.value().plan_count, std::size_t{3});

  auto store = rep::PlanStore::open(rep::PlanStoreOptions{directory.path(), 64, true});
  REP_REQUIRE(store.ok());
  REP_REQUIRE(store.value()->state().plans.size() == 3);
  for (const rep::Plan& plan : store.value()->state().plans) {
    REP_CHECK(plan.verify().ok());
  }
}

REP_TEST(multiprocess, crash_at_every_publication_boundary_leaves_one_whole_generation) {
  const std::string executable = child_executable();
  REP_REQUIRE_MSG(!executable.empty(), "rep_fault_child executable was not found");

  const std::vector<std::string> boundaries = {"after_stage_write", "after_stage_verify",
                                               "after_state_publish", "before_current_update",
                                               "after_current_update"};
  for (const std::string& boundary : boundaries) {
    reptest::TempDir directory("mp-crash-" + boundary);

    // Generation one is published normally, so the crash has a previous whole
    // generation to fall back to.
    ChildProcess seed_writer;
    REP_REQUIRE(seed_writer.launch(executable, {"commit", directory.path(), "1"}));
    const std::string seeded = seed_writer.read_line();
    DWORD seed_exit = 0;
    REP_REQUIRE(seed_writer.wait_for_exit(seed_exit));
    REP_CHECK_EQ(seed_exit, DWORD{0});
    REP_CHECK_MSG(contains(seeded, "committed sequence=1"), "seed failed: " + seeded);
    seed_writer.release();

    // The second generation dies at the requested boundary.
    ChildProcess crasher;
    REP_REQUIRE(crasher.launch(executable, {"crash-at", directory.path(), boundary}));
    const std::string crashed = crasher.read_line();
    DWORD crash_exit = 0;
    REP_REQUIRE(crasher.wait_for_exit(crash_exit));
    REP_CHECK_MSG(crash_exit == static_cast<DWORD>(rep::kFaultInjectedExitCode),
                  "boundary " + boundary + " did not inject a fault, exit=" +
                      std::to_string(crash_exit) + " output=" + crashed);
    crasher.release();

    // An independent process must recover exactly one whole generation: either
    // the one before the commit point moved, or the one after.  Never a blend.
    auto store = rep::PlanStore::open(rep::PlanStoreOptions{directory.path(), 64, true});
    REP_REQUIRE_MSG(store.ok(), "recovery failed at boundary " + boundary + ": " +
                                    store.error().to_string());
    const std::uint64_t sequence = store.value()->published_sequence().value();
    REP_CHECK_MSG(sequence == 1 || sequence == 2,
                  "boundary " + boundary + " recovered an impossible sequence " +
                      std::to_string(sequence));
    const std::size_t plans = store.value()->state().plans.size();
    REP_CHECK_MSG(plans == sequence,
                  "boundary " + boundary + " recovered " + std::to_string(plans) +
                      " plans for sequence " + std::to_string(sequence));
    for (const rep::Plan& plan : store.value()->state().plans) {
      REP_CHECK_MSG(plan.verify().ok(),
                    "boundary " + boundary + " recovered an unverifiable plan");
    }
    if (boundary == "after_current_update") {
      REP_CHECK_EQ(sequence, std::uint64_t{2});
    } else {
      REP_CHECK_EQ(sequence, std::uint64_t{1});
    }

    // And the recovered store is usable: a further generation publishes
    // through the same handle, so the lock is not the thing being tested here.
    const rep::StoreState& recovered = store.value()->state();
    std::vector<rep::Plan> next_plans = recovered.plans;
    auto plan = reptest::synthetic_plan("planner-01", sequence + 1, "lineage-1",
                                        "key-" + std::to_string(sequence + 1));
    REP_REQUIRE(plan.ok());
    next_plans.push_back(plan.take());
    auto next = rep::StoreState::make(rep::Sequence{sequence + 1}, recovered.epoch,
                                      recovered.writer, recovered.opened_at,
                                      std::move(next_plans), recovered.keys, 64);
    REP_REQUIRE(next.ok());
    REP_CHECK(store.value()->commit(next.value()).ok());
    store.value().reset();
  }
}

REP_TEST(multiprocess, epoch_fencing_survives_a_change_of_incarnation) {
  const std::string executable = child_executable();
  REP_REQUIRE_MSG(!executable.empty(), "rep_fault_child executable was not found");
  reptest::TempDir directory("mp-epoch");

  ChildProcess claimant;
  REP_REQUIRE(claimant.launch(executable, {"claim", directory.path()}));
  const std::string claimed = claimant.read_line();
  DWORD claim_exit = 0;
  REP_REQUIRE(claimant.wait_for_exit(claim_exit));
  REP_CHECK_EQ(claim_exit, DWORD{0});
  REP_CHECK_MSG(contains(claimed, "claimed epoch=1"), "unexpected claim: " + claimed);
  claimant.release();

  rep::EngineOptions options;
  options.planner = rep::PlannerId::parse("planner-01").value();
  options.store_directory = directory.path();
  auto engine = rep::PlanEngine::open(std::move(options));
  REP_REQUIRE(engine.ok());
  // The successor claims the next incarnation rather than inheriting the dead
  // one's authority.
  REP_CHECK_EQ(engine.value()->current_epoch().value(), std::uint64_t{2});
  const rep::RecoveryReport recovery = engine.value()->recovery();
  REP_CHECK(recovery.recovered);
  REP_CHECK_EQ(recovery.previous_epoch.value(), std::uint64_t{1});
  REP_CHECK_EQ(recovery.claimed_epoch.value(), std::uint64_t{2});

  // A caller that read the dead incarnation's authority is fenced out.
  auto stale = rep::PlanRequest::make(
      rep::IdempotencyKey("mp-epoch-1"), rep::AuthorityId("requester-01"), rep::Epoch{1},
      rep::UnixNanos{1000},
      rep::IsolationRequest::make(rep::RackId("rack-alpha"), rep::IsolationKind::Depower,
                                  rep::Generation{7}, {rep::ObligationKind::Workload})
          .value(),
      rep::LineageId{}, std::vector<rep::EvidenceSource>{}, rep::EvidenceBundle{});
  REP_REQUIRE(stale.ok());
  auto refused = engine.value()->submit(stale.value());
  REP_REQUIRE(refused.ok());
  REP_CHECK(refused.value().kind == rep::OutcomeKind::RejectedStaleEpoch);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{0});
}

REP_TEST(multiprocess, a_holder_that_dies_mid_publication_leaves_a_usable_store) {
  const std::string executable = child_executable();
  REP_REQUIRE_MSG(!executable.empty(), "rep_fault_child executable was not found");
  reptest::TempDir directory("mp-midkill");

  // Seed one generation, then kill a second writer while it holds the lock.
  ChildProcess seed_writer;
  REP_REQUIRE(seed_writer.launch(executable, {"commit", directory.path(), "1"}));
  (void)seed_writer.read_line();
  DWORD seed_exit = 0;
  REP_REQUIRE(seed_writer.wait_for_exit(seed_exit));
  seed_writer.release();

  ChildProcess holder;
  REP_REQUIRE(holder.launch(executable, {"hold-lock", directory.path()}));
  const std::string held = holder.read_line();
  REP_CHECK(contains(held, "locked"));
  holder.kill();
  DWORD holder_exit = 0;
  REP_REQUIRE(holder.wait_for_exit(holder_exit));
  holder.release();

  auto store = rep::PlanStore::open(rep::PlanStoreOptions{directory.path(), 64, true});
  REP_REQUIRE(store.ok());
  REP_CHECK_EQ(store.value()->published_sequence().value(), std::uint64_t{1});
  REP_REQUIRE(store.value()->state().plans.size() == 1);
  REP_CHECK(store.value()->state().plans.front().verify().ok());
  auto plan = reptest::synthetic_plan("planner-01", 2, "lineage-1", "key-2");
  REP_REQUIRE(plan.ok());
  std::vector<rep::Plan> plans = store.value()->state().plans;
  plans.push_back(plan.take());
  auto next = rep::StoreState::make(rep::Sequence{2}, store.value()->state().epoch,
                                    store.value()->state().writer,
                                    store.value()->state().opened_at, std::move(plans),
                                    store.value()->state().keys, 64);
  REP_REQUIRE(next.ok());
  REP_CHECK(store.value()->commit(next.value()).ok());
}

#else

// The crash and multiprocess proofs need process control that this build does
// not provide; the in-process exclusion contract is still checked.
REP_TEST(multiprocess, single_writer_exclusion_within_one_process) {
  reptest::TempDir directory("mp-inproc");
  rep::PlanStoreOptions options;
  options.directory = directory.path();
  auto first = rep::PlanStore::open(options);
  REP_REQUIRE(first.ok());
  auto second = rep::PlanStore::open(options);
  REP_CHECK(!second.ok());
  if (!second.ok()) {
    REP_CHECK(second.error().code == rep::ErrorCode::Locked);
  }
}

#endif

REP_TEST_MAIN()
