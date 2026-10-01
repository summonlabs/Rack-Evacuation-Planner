// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Independent child process used by the multiprocess and crash-consistency
// tests. It is a real executable: the parent spawns it with CreateProcess and
// observes it through a pipe, so nothing here shares memory with the parent.
//
// Usage:
//   rep_fault_child hold-lock  <store-dir>
//       Opens the durable store (taking the exclusive kernel writer lock) and
//       blocks reading stdin until it is closed or the process is killed.
//       Prints "locked epoch=<n>" once the lock is held.
//   rep_fault_child claim      <store-dir>
//       Opens a durable PlanEngine, prints "claimed epoch=<n>", exits.
//   rep_fault_child commit     <store-dir> <count>
//       Appends <count> synthetic generations and publishes each one.
//   rep_fault_child crash-at   <store-dir> <point>
//       Appends one synthetic generation with a fault armed at <point>, so the
//       process dies abruptly exactly there (exit code 93).
//   rep_fault_child stats      <store-dir>
//       Reads the store without taking the lock and prints its statistics.
//
// Every mode writes one line per fact to stdout and exits 0, or prints
// "error code=<n> message=<text>" to stderr and exits 1.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include <rep/rep.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

[[nodiscard]] int report_error(const rep::Error& error) {
  std::fprintf(stderr, "error code=%s message=%s\n", std::string(rep::to_string(error.code)).c_str(),
               error.to_string().c_str());
  return 1;
}

[[nodiscard]] rep::Result<rep::Plan> synthetic_plan(const std::string& planner_id,
                                                    std::uint64_t sequence,
                                                    const std::string& lineage,
                                                    const std::string& key) {
  auto planner = rep::PlannerId::parse(planner_id);
  if (!planner.ok()) {
    return planner.error();
  }
  auto plan_id = rep::PlanId::parse(planner_id + "-" + std::to_string(sequence));
  if (!plan_id.ok()) {
    return plan_id.error();
  }
  auto lineage_id = rep::LineageId::parse(lineage);
  if (!lineage_id.ok()) {
    return lineage_id.error();
  }
  auto idempotency = rep::IdempotencyKey::parse(key);
  if (!idempotency.ok()) {
    return idempotency.error();
  }
  auto rack = rep::RackId::parse("rack-alpha");
  if (!rack.ok()) {
    return rack.error();
  }

  rep::Plan plan;
  plan.id = plan_id.value();
  plan.planner = planner.value();
  plan.revision = rep::PlanRevision{1};
  plan.sequence = rep::Sequence{sequence};
  plan.epoch = rep::Epoch{1};
  plan.lineage = lineage_id.value();
  plan.idempotency_key = idempotency.value();
  auto isolation = rep::IsolationRequest::make(
      rack.value(), rep::IsolationKind::Depower, rep::Generation{7},
      {rep::ObligationKind::Workload});
  if (!isolation.ok()) {
    return isolation.error();
  }
  plan.request_digest = isolation.value().digest();
  plan.source_rack = rack.value();
  plan.composition_revision = rep::Generation{7};
  plan.isolation = rep::IsolationKind::Depower;
  plan.evacuate_kinds = {rep::ObligationKind::Workload};
  plan.status = rep::PlanStatus::EmptySafe;
  return rep::Plan::seal(std::move(plan));
}

[[nodiscard]] rep::Result<rep::StoreState> make_next_state(const rep::StoreState& published,
                                                           std::size_t count) {
  std::vector<rep::Plan> plans = published.plans;
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint64_t sequence = published.sequence.value() + index + 1;
    const std::string key = "fault-key-" + std::to_string(sequence);
    auto plan = synthetic_plan("fault-child", sequence, "fault-lineage", key);
    if (!plan.ok()) {
      return plan.error();
    }
    plans.push_back(plan.take());
  }
  const std::uint64_t next_sequence = published.sequence.value() + count;
  return rep::StoreState::make(rep::Sequence{next_sequence}, published.epoch, published.writer,
                               published.opened_at, std::move(plans), published.keys, 4096);
}

[[nodiscard]] std::optional<rep::StoreFaultPoint> fault_point_from(std::string_view text) {
  if (text == "after_stage_write") {
    return rep::StoreFaultPoint::AfterStageWrite;
  }
  if (text == "after_stage_verify") {
    return rep::StoreFaultPoint::AfterStageVerify;
  }
  if (text == "after_state_publish") {
    return rep::StoreFaultPoint::AfterStatePublish;
  }
  if (text == "before_current_update") {
    return rep::StoreFaultPoint::BeforeCurrentUpdate;
  }
  if (text == "after_current_update") {
    return rep::StoreFaultPoint::AfterCurrentUpdate;
  }
  return std::nullopt;
}

[[nodiscard]] int hold_lock(const std::string& directory) {
  rep::PlanStoreOptions options;
  options.directory = directory;
  auto store = rep::PlanStore::open(options);
  if (!store.ok()) {
    return report_error(store.error());
  }
  std::printf("locked epoch=%llu sequence=%llu\n",
              static_cast<unsigned long long>(store.value()->published_epoch().value()),
              static_cast<unsigned long long>(store.value()->published_sequence().value()));
  std::fflush(stdout);

  // Block until the parent closes the pipe or kills this process. Nothing here
  // is time based: the process simply waits for input that never arrives.
  char buffer[1] = {};
  while (true) {
#if defined(_WIN32)
    DWORD read = 0;
    if (::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), buffer, 1, &read, nullptr) == 0 || read == 0) {
      break;
    }
#else
    if (::read(STDIN_FILENO, buffer, 1) <= 0) {
      break;
    }
#endif
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: rep_fault_child <mode> <store-dir> [argument]\n");
    return 2;
  }
  const std::string mode = argv[1];
  const std::string directory = argv[2];

  if (mode == "hold-lock") {
    return hold_lock(directory);
  }

  if (mode == "claim") {
    rep::EngineOptions options;
    auto planner = rep::PlannerId::parse("fault-child");
    if (!planner.ok()) {
      return report_error(planner.error());
    }
    options.planner = planner.value();
    options.store_directory = directory;
    auto engine = rep::PlanEngine::open(std::move(options));
    if (!engine.ok()) {
      return report_error(engine.error());
    }
    std::printf("claimed epoch=%llu sequence=%llu\n",
                static_cast<unsigned long long>(engine.value()->current_epoch().value()),
                static_cast<unsigned long long>(engine.value()->last_sequence().value()));
    return 0;
  }

  if (mode == "stats") {
    auto stats = rep::PlanStore::inspect(directory);
    if (!stats.ok()) {
      return report_error(stats.error());
    }
    std::printf("sequence=%llu epoch=%llu plans=%llu keys=%llu bytes=%llu\n",
                static_cast<unsigned long long>(stats.value().sequence.value()),
                static_cast<unsigned long long>(stats.value().epoch.value()),
                static_cast<unsigned long long>(stats.value().plan_count),
                static_cast<unsigned long long>(stats.value().idempotency_count),
                static_cast<unsigned long long>(stats.value().state_bytes));
    return 0;
  }

  if (mode == "commit" || mode == "crash-at") {
    if (argc < 4) {
      std::fprintf(stderr, "mode %s needs a fourth argument\n", mode.c_str());
      return 2;
    }
    std::size_t count = 1;
    std::optional<rep::StoreFaultPoint> fault;
    if (mode == "commit") {
      count = std::strtoull(argv[3], nullptr, 10);
      if (count == 0) {
        std::fprintf(stderr, "commit count must be at least one\n");
        return 2;
      }
    } else {
      fault = fault_point_from(argv[3]);
      if (!fault.has_value()) {
        std::fprintf(stderr, "unknown fault point %s\n", argv[3]);
        return 2;
      }
    }

    rep::PlanStoreOptions options;
    options.directory = directory;
    auto store = rep::PlanStore::open(options);
    if (!store.ok()) {
      return report_error(store.error());
    }
    auto next = make_next_state(store.value()->state(), count);
    if (!next.ok()) {
      return report_error(next.error());
    }
    if (fault.has_value()) {
      store.value()->set_fault_point(*fault);
    }
    const auto committed = store.value()->commit(next.value());
    if (!committed.ok()) {
      return report_error(committed.error());
    }
    std::printf("committed sequence=%llu epoch=%llu plans=%llu\n",
                static_cast<unsigned long long>(store.value()->published_sequence().value()),
                static_cast<unsigned long long>(store.value()->published_epoch().value()),
                static_cast<unsigned long long>(store.value()->state().plans.size()));
    return 0;
  }

  std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
  return 2;
}
