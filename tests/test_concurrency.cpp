// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Concurrency: many threads against one engine, one idempotency key submitted
// concurrently, readers racing writers, and concurrent store lifecycles.
//
// Every test synchronises with a std::latch or std::barrier with a fixed
// participant count and joins every thread.  Nothing here waits on a clock,
// so a hang is a defect rather than a slow test.  The testkit registry is not
// thread safe, so a worker thread never records a check: it records what it
// observed, and the joined main thread asserts on that record.

#include "testkit.hpp"

#include <barrier>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <latch>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "rep/engine.hpp"
#include "rep/evidence.hpp"
#include "rep/plan.hpp"
#include "rep/request.hpp"
#include "rep/scenario.hpp"
#include "rep/status.hpp"
#include "rep/store.hpp"
#include "rep/types.hpp"

using namespace rep;

// A requirement that names the underlying error when a Result is not ok.  The
// body is abandoned, so nothing after it runs against a value that was never
// produced.
#define REP_REQUIRE_OK(expression)                                                         \
  do {                                                                                     \
    const auto& reptest_result = (expression);                                             \
    if (!reptest_result.ok()) {                                                            \
      REP_FAIL(std::string("unexpected error: ") + reptest_result.error().message + " [" + \
               std::string(to_string(reptest_result.error().code)) + "]");                 \
      return;                                                                              \
    }                                                                                      \
  } while (false)

namespace {

// ---------------------------------------------------------------------------
// Scenario assembly
// ---------------------------------------------------------------------------

const char* const kSourceComposition =
    "evidence=rack_composition authority=rack-authority stream=rack-A.comp";
const char* const kSourceEnumeration =
    "evidence=enumeration authority=rack-authority stream=rack-A.enum";
const char* const kSourceCatalog =
    "evidence=obligation_catalog authority=rack-authority stream=rack-A.cat";
const char* const kSourceCapacity =
    "evidence=capacity authority=facility-capacity stream=capacity";
const char* const kSourceDomains =
    "evidence=failure_domain authority=facility-capacity stream=domains";
const char* const kSourceAsi =
    "evidence=asi_workload_state authority=agent-scheduler stream=asi";
const char* const kSourceOffers =
    "evidence=candidate_offers authority=agent-scheduler stream=offers";

const char* const kBlockComposition =
    "evidence rack_composition stream=rack-A.comp generation=7 epoch=2\n"
    "rack rack-A\n"
    "revision 7\n"
    "occupants w1\n"
    "end\n";
const char* const kBlockEnumeration =
    "evidence enumeration stream=rack-A.enum generation=7 epoch=2\n"
    "rack rack-A\n"
    "revision 7\n"
    "complete true\n"
    "enumerated w1\n"
    "end\n";
const char* const kBlockCatalog =
    "evidence obligation_catalog stream=rack-A.cat generation=7 epoch=2\n"
    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 protected=false "
    "depends=-\n"
    "end\n";
const char* const kBlockCapacity =
    "evidence capacity stream=capacity generation=4 epoch=1\n"
    "destination destination=rack_slot:rack-B available=cpu_millicores:8000\n"
    "end\n";
const char* const kBlockDomains =
    "evidence failure_domain stream=domains generation=4 epoch=1\n"
    "member kind=rack rack=rack-A domain=fd-1\n"
    "member kind=destination destination=rack_slot:rack-B domain=fd-2\n"
    "end\n";
const char* const kBlockAsi =
    "evidence asi_workload_state stream=asi generation=4 epoch=1\n"
    "record obligation=w1 lifecycle=running migration=live_allowed attachment=stateless\n"
    "end\n";
const char* const kBlockOffers =
    "evidence candidate_offers stream=offers generation=4 epoch=1\n"
    "candidate id=c1 obligation=w1 action=live_migrate destination=rack_slot:rack-B "
    "authority=agent-scheduler generation=4 epoch=1 provision=cpu_millicores:1000 cost=10 "
    "window=false\n"
    "end\n";

std::string scenario_text(const std::string& request_line,
                          const std::vector<std::string>& sources,
                          const std::vector<std::string>& blocks) {
  std::string out = "version 1\nplanner planner-01\n";
  out += request_line;
  out += '\n';
  for (const std::string& source : sources) {
    out += "source ";
    out += source;
    out += '\n';
  }
  for (const std::string& block : blocks) {
    out += block;
  }
  return out;
}

std::vector<std::string> standard_sources() {
  return {kSourceComposition, kSourceEnumeration, kSourceCatalog, kSourceCapacity,
          kSourceDomains,     kSourceAsi,         kSourceOffers};
}

std::vector<std::string> standard_blocks() {
  return {kBlockComposition, kBlockEnumeration, kBlockCatalog, kBlockCapacity,
          kBlockDomains,     kBlockAsi,         kBlockOffers};
}

// Every request is the same proven evacuation with a different idempotency
// key, so a distinct digest is the only thing distinguishing them.
std::string scenario_for(const std::string& key) {
  return scenario_text(
      "request key=" + key +
          " authority=requester-01 epoch=0 time=1000 lineage=- rack=rack-A "
          "isolation=depower composition_revision=7 kinds=workload",
      standard_sources(), standard_blocks());
}

Result<PlanRequest> request_for(const std::string& key) {
  const Result<ScenarioDocument> parsed = parse_scenario(scenario_for(key));
  if (!parsed.ok()) {
    return parsed.error();
  }
  return parsed.value().request;
}

std::string key_for(std::size_t index) { return "k-" + std::to_string(index); }

// ---------------------------------------------------------------------------
// Worker bookkeeping
// ---------------------------------------------------------------------------

// One worker's private record.  A worker writes only its own slot, so no lock
// is needed and no check is recorded off the main thread.
struct WorkerLog {
  std::string failure;
  std::size_t submissions{0};
  std::size_t planned{0};
  std::size_t replayed{0};
  std::size_t conflicts{0};
  std::size_t rejections{0};
  std::size_t verified{0};
  std::size_t assessed{0};
  PlanId plan_id;
  Digest plan_digest;
  Sequence sequence;
  std::uint64_t final_epoch{0};

  void fail(std::string message) {
    if (failure.empty()) {
      failure = std::move(message);
    }
  }
};

void report_failures(const std::vector<WorkerLog>& logs) {
  for (std::size_t index = 0; index < logs.size(); ++index) {
    REP_CHECK_MSG(logs[index].failure.empty(),
                  "worker " + std::to_string(index) + ": " + logs[index].failure);
  }
}

void install_plan(WorkerLog& log, const Plan& plan) {
  log.plan_id = plan.id;
  log.plan_digest = plan.plan_digest;
  log.sequence = plan.sequence;
}

// ---------------------------------------------------------------------------
// Engine helpers
// ---------------------------------------------------------------------------

Result<std::unique_ptr<PlanEngine>> open_engine(const PlannerId& planner,
                                                const std::filesystem::path& directory) {
  EngineOptions options;
  options.planner = planner;
  options.store_directory = directory;
  options.max_plans = 64;
  return PlanEngine::open(options);
}

std::filesystem::path fresh_directory(std::string_view name) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / std::filesystem::path(std::string(name));
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::filesystem::create_directories(directory, ignored);
  return directory;
}

} // namespace

// ---------------------------------------------------------------------------
// Distinct requests, one engine
// ---------------------------------------------------------------------------

REP_TEST(Concurrency, distinct_requests_commit_exactly_once_each) {
  constexpr std::size_t kThreads = 8;
  EngineOptions options;
  options.planner = PlannerId{std::string("planner-concurrent")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);

  std::vector<PlanRequest> requests;
  requests.reserve(kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    const auto request = request_for(key_for(index));
    REP_REQUIRE_OK(request);
    requests.push_back(request.value());
  }

  std::vector<WorkerLog> logs(kThreads);
  std::latch start{static_cast<std::ptrdiff_t>(kThreads)};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    threads.emplace_back([&engine, &requests, &logs, &start, index]() {
      WorkerLog& log = logs[index];
      start.arrive_and_wait();
      try {
        const auto outcome = engine.value()->submit(requests[index]);
        if (!outcome.ok()) {
          log.fail("submit failed: " + outcome.error().message);
          return;
        }
        const PlanOutcome& value = outcome.value();
        ++log.submissions;
        if (value.error.code == ErrorCode::Internal) {
          log.fail("submit reported an internal error");
          return;
        }
        switch (value.kind) {
          case OutcomeKind::Planned:
            ++log.planned;
            break;
          case OutcomeKind::Replayed:
            ++log.replayed;
            break;
          default:
            ++log.rejections;
            log.fail("submit rejected a valid request: " + value.error.message);
            return;
        }
        if (!value.has_plan) {
          log.fail("a committed outcome carried no plan");
          return;
        }
        if (!value.plan.verify().ok()) {
          log.fail("the returned plan does not verify");
          return;
        }
        if (value.plan.status != PlanStatus::Complete) {
          log.fail("the returned plan is not complete");
          return;
        }
        install_plan(log, value.plan);
      } catch (const std::exception& error) {
        log.fail(std::string("exception: ") + error.what());
      } catch (...) {
        log.fail("unknown exception");
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  report_failures(logs);
  std::size_t planned = 0;
  std::size_t replayed = 0;
  std::size_t submissions = 0;
  std::set<std::uint64_t> sequences;
  std::set<std::string> plan_ids;
  for (const WorkerLog& log : logs) {
    planned += log.planned;
    replayed += log.replayed;
    submissions += log.submissions;
    sequences.insert(log.sequence.value());
    plan_ids.insert(log.plan_id.str());
  }
  REP_CHECK_EQ(submissions, kThreads);
  REP_CHECK_EQ(planned, kThreads);
  REP_CHECK_EQ(replayed, std::size_t{0});
  REP_CHECK_EQ(plan_ids.size(), kThreads);
  REP_CHECK_EQ(sequences.size(), kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    REP_CHECK_MSG(sequences.count(index + 1) == 1,
                  "sequence " + std::to_string(index + 1) + " was not handed out exactly once");
  }
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{kThreads});
  const EngineStats stats = engine.value()->stats();
  REP_CHECK_EQ(stats.plan_count, kThreads);
  REP_CHECK_EQ(stats.idempotency_count, kThreads);
  REP_CHECK(!stats.durable);

  // Every key is committed exactly once, and every committed plan loads back
  // and still verifies.
  for (std::size_t index = 0; index < kThreads; ++index) {
    const auto by_key = engine.value()->plan_by_idempotency_key(key_for(index));
    REP_REQUIRE_OK(by_key);
    REP_CHECK(by_key.value().verify().ok());
    REP_CHECK_EQ(by_key.value().plan_digest, logs[index].plan_digest);
    REP_CHECK(by_key.value().id == logs[index].plan_id);
    const auto by_id = engine.value()->plan_by_id(logs[index].plan_id.view());
    REP_REQUIRE_OK(by_id);
    REP_CHECK(by_id.value().verify().ok());
    REP_CHECK(by_id.value().assignments.size() == std::size_t{1});
  }
  REP_CHECK_EQ(engine.value()->history().size(), kThreads);
}

// ---------------------------------------------------------------------------
// One key, many threads
// ---------------------------------------------------------------------------

REP_TEST(Concurrency, one_key_submitted_concurrently_commits_once) {
  constexpr std::size_t kThreads = 8;
  EngineOptions options;
  options.planner = PlannerId{std::string("planner-concurrent")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);

  const auto shared = request_for("k-shared");
  REP_REQUIRE_OK(shared);
  const PlanRequest& request = shared.value();

  std::vector<WorkerLog> logs(kThreads);
  std::latch start{static_cast<std::ptrdiff_t>(kThreads)};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    threads.emplace_back([&engine, &request, &logs, &start, index]() {
      WorkerLog& log = logs[index];
      start.arrive_and_wait();
      try {
        const auto outcome = engine.value()->submit(request);
        if (!outcome.ok()) {
          log.fail("submit failed: " + outcome.error().message);
          return;
        }
        const PlanOutcome& value = outcome.value();
        ++log.submissions;
        if (value.error.code == ErrorCode::Internal) {
          log.fail("submit reported an internal error");
          return;
        }
        switch (value.kind) {
          case OutcomeKind::Planned:
            ++log.planned;
            break;
          case OutcomeKind::Replayed:
            ++log.replayed;
            break;
          case OutcomeKind::RejectedIdempotencyConflict:
            ++log.conflicts;
            return;
          default:
            ++log.rejections;
            log.fail("submit rejected a valid request: " + value.error.message);
            return;
        }
        if (!value.has_plan) {
          log.fail("a committed outcome carried no plan");
          return;
        }
        if (!value.plan.verify().ok()) {
          log.fail("the returned plan does not verify");
          return;
        }
        install_plan(log, value.plan);
      } catch (const std::exception& error) {
        log.fail(std::string("exception: ") + error.what());
      } catch (...) {
        log.fail("unknown exception");
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  report_failures(logs);
  std::size_t planned = 0;
  std::size_t replayed = 0;
  std::size_t conflicts = 0;
  std::set<std::string> plan_ids;
  std::set<std::string> digests;
  for (const WorkerLog& log : logs) {
    planned += log.planned;
    replayed += log.replayed;
    conflicts += log.conflicts;
    if (log.planned == 1 || log.replayed == 1) {
      plan_ids.insert(log.plan_id.str());
      digests.insert(log.plan_digest.to_hex());
    }
  }
  REP_CHECK_EQ(planned, std::size_t{1});
  REP_CHECK_EQ(replayed, kThreads - 1);
  REP_CHECK_EQ(conflicts, std::size_t{0});
  REP_CHECK_EQ(plan_ids.size(), std::size_t{1});
  REP_CHECK_EQ(digests.size(), std::size_t{1});
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{1});
  REP_CHECK_EQ(engine.value()->stats().plan_count, std::size_t{1});
  REP_CHECK_EQ(engine.value()->stats().idempotency_count, std::size_t{1});
  const auto committed = engine.value()->plan_by_idempotency_key("k-shared");
  REP_REQUIRE_OK(committed);
  REP_CHECK(committed.value().verify().ok());

  // The same key with two different digests: whichever request wins the race
  // commits exactly once, its siblings replay, and every submission of the
  // other request is a typed conflict.  No interleaving can produce a second
  // commit for the key.
  const auto variant_a = request_for("k-race");
  const auto variant_b = request_for("k-race");
  REP_REQUIRE_OK(variant_a);
  REP_REQUIRE_OK(variant_b);
  std::string changed = scenario_for("k-race");
  const std::string needle = "time=1000";
  const std::size_t position = changed.find(needle);
  REP_REQUIRE(position != std::string::npos);
  changed.replace(position, needle.size(), "time=3000");
  const Result<ScenarioDocument> alternative = parse_scenario(changed);
  REP_REQUIRE_OK(alternative);
  REP_CHECK(!(alternative.value().request.digest() == variant_a.value().digest()));

  const PlanRequest* choices[2] = {&variant_a.value(), &alternative.value().request};
  std::vector<WorkerLog> race_logs(kThreads);
  std::latch race_start{static_cast<std::ptrdiff_t>(kThreads)};
  std::vector<std::thread> racers;
  racers.reserve(kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    const PlanRequest* choice = choices[index % 2];
    racers.emplace_back([&engine, &race_logs, &race_start, choice, index]() {
      WorkerLog& log = race_logs[index];
      race_start.arrive_and_wait();
      try {
        const auto outcome = engine.value()->submit(*choice);
        if (!outcome.ok()) {
          log.fail("submit failed: " + outcome.error().message);
          return;
        }
        ++log.submissions;
        switch (outcome.value().kind) {
          case OutcomeKind::Planned:
            ++log.planned;
            break;
          case OutcomeKind::Replayed:
            ++log.replayed;
            break;
          case OutcomeKind::RejectedIdempotencyConflict:
            ++log.conflicts;
            break;
          default:
            log.fail("unexpected outcome " +
                     std::string(to_string(outcome.value().kind)));
            break;
        }
      } catch (const std::exception& error) {
        log.fail(std::string("exception: ") + error.what());
      } catch (...) {
        log.fail("unknown exception");
      }
    });
  }
  for (std::thread& racer : racers) {
    racer.join();
  }
  report_failures(race_logs);
  std::size_t race_planned = 0;
  std::size_t race_replayed = 0;
  std::size_t race_conflicts = 0;
  for (const WorkerLog& log : race_logs) {
    race_planned += log.planned;
    race_replayed += log.replayed;
    race_conflicts += log.conflicts;
  }
  REP_CHECK_EQ(race_planned, std::size_t{1});
  REP_CHECK_EQ(race_replayed, std::size_t{3});
  REP_CHECK_EQ(race_conflicts, std::size_t{4});
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{2});
  REP_CHECK_EQ(engine.value()->stats().plan_count, std::size_t{2});
}

// ---------------------------------------------------------------------------
// Readers racing writers
// ---------------------------------------------------------------------------

REP_TEST(Concurrency, readers_never_observe_a_torn_plan) {
  constexpr std::size_t kWorkers = 8;
  constexpr std::size_t kWriters = 4;
  constexpr std::size_t kRounds = 6;
  EngineOptions options;
  options.planner = PlannerId{std::string("planner-concurrent")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);

  std::vector<std::vector<PlanRequest>> requests;
  requests.reserve(kWorkers);
  for (std::size_t worker = 0; worker < kWorkers; ++worker) {
    std::vector<PlanRequest> per_round;
    per_round.reserve(kRounds);
    for (std::size_t round = 0; round < kRounds; ++round) {
      const auto request = request_for("k-" + std::to_string(worker) + "-" + std::to_string(round));
      REP_REQUIRE_OK(request);
      per_round.push_back(request.value());
    }
    requests.push_back(std::move(per_round));
  }

  std::vector<WorkerLog> logs(kWorkers);
  std::barrier sync{static_cast<std::ptrdiff_t>(kWorkers)};
  std::vector<std::thread> threads;
  threads.reserve(kWorkers);
  for (std::size_t index = 0; index < kWorkers; ++index) {
    threads.emplace_back([&engine, &requests, &logs, &sync, index]() {
      WorkerLog& log = logs[index];
      const PlanRequest& own = requests[index][0];
      for (std::size_t round = 0; round < kRounds; ++round) {
        sync.arrive_and_wait();
        try {
          if (round == 0 || index < kWriters) {
            const auto outcome = engine.value()->submit(requests[index][round]);
            if (!outcome.ok()) {
              log.fail("submit failed: " + outcome.error().message);
              continue;
            }
            ++log.submissions;
            if (outcome.value().kind != OutcomeKind::Planned) {
              log.fail("a distinct request did not plan");
              continue;
            }
            if (!outcome.value().plan.verify().ok()) {
              log.fail("the returned plan does not verify");
              continue;
            }
            ++log.planned;
            install_plan(log, outcome.value().plan);
            continue;
          }

          // A reader: everything it can observe must be a whole, verified
          // plan, and the engine's own counters must stay consistent.
          //
          // The history is taken first: writers keep committing while this
          // reader runs, so the only sound expectation is that the later
          // statistics never report fewer plans than that snapshot.
          const std::vector<PlanSummary> history = engine.value()->history();
          const EngineStats stats = engine.value()->stats();
          if (stats.durable) {
            log.fail("a non-durable engine reported a durable store");
          }
          if (stats.plan_count != stats.idempotency_count) {
            log.fail("plan and idempotency counts diverged");
          }
          if (stats.sequence.value() < stats.plan_count) {
            log.fail("the sequence is behind the plan count");
          }
          if (!(stats.epoch == engine.value()->current_epoch())) {
            log.fail("stats reported a different epoch than current_epoch");
          }
          if (stats.planner.view() != std::string_view("planner-concurrent")) {
            log.fail("stats reported a different planner");
          }
          if (history.size() > stats.plan_count) {
            log.fail("history holds more plans than stats reports");
          }
          for (const PlanSummary& summary : history) {
            const auto loaded = engine.value()->plan_by_id(summary.id.view());
            if (!loaded.ok()) {
              log.fail("history names a plan that does not load: " + loaded.error().message);
              break;
            }
            if (!loaded.value().verify().ok()) {
              log.fail("a plan returned by plan_by_id does not verify");
              break;
            }
            if (!(loaded.value().plan_digest == summary.plan_digest)) {
              log.fail("a plan disagrees with its own history summary");
              break;
            }
            ++log.verified;
            const auto assessment = engine.value()->assess(summary.id.view(), own.evidence);
            if (!assessment.ok()) {
              log.fail("assess failed: " + assessment.error().message);
              break;
            }
            ++log.assessed;
            if (summary.id == log.plan_id) {
              if (assessment.value().verdict != SafetyVerdict::Safe) {
                log.fail("a plan assessed against its own evidence is not safe");
              }
              if (!assessment.value().staleness.empty()) {
                log.fail("a plan assessed against its own evidence reported staleness");
              }
              if (!assessment.value().same_incarnation) {
                log.fail("a plan of the current incarnation was reported as another's");
              }
            }
          }
          const auto by_key =
              engine.value()->plan_by_idempotency_key(requests[index][0].idempotency_key.view());
          if (!by_key.ok() || !by_key.value().verify().ok()) {
            log.fail("the reader's own idempotency key did not resolve to a verified plan");
          }
          const auto latest = engine.value()->latest_for_lineage(log.plan_id.str());
          if (!latest.ok()) {
            log.fail("latest_for_lineage failed for the reader's own lineage");
          }
          const auto superseded = engine.value()->is_superseded(log.plan_id.view());
          if (!superseded.ok()) {
            log.fail("is_superseded failed for a committed plan");
          }
          if (superseded.ok() && superseded.value()) {
            log.fail("a plan on its own lineage reported itself superseded");
          }
        } catch (const std::exception& error) {
          log.fail(std::string("exception: ") + error.what());
        } catch (...) {
          log.fail("unknown exception");
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  report_failures(logs);
  const std::size_t expected_plans = kWorkers + kWriters * (kRounds - 1);
  std::size_t planned = 0;
  for (const WorkerLog& log : logs) {
    planned += log.planned;
    REP_CHECK(!log.plan_id.empty());
  }
  REP_CHECK_EQ(planned, expected_plans);
  REP_CHECK_EQ(engine.value()->last_sequence().value(),
               static_cast<std::uint64_t>(expected_plans));
  for (std::size_t index = kWriters; index < kWorkers; ++index) {
    REP_CHECK_MSG(logs[index].verified > 0,
                  "reader " + std::to_string(index) + " never observed a plan");
    REP_CHECK_MSG(logs[index].assessed > 0,
                  "reader " + std::to_string(index) + " never assessed a plan");
  }

  // Nothing was lost or duplicated, and every plan in the final history is
  // whole.
  const std::vector<PlanSummary> history = engine.value()->history();
  REP_REQUIRE(history.size() == expected_plans);
  std::set<std::uint64_t> sequences;
  for (const PlanSummary& summary : history) {
    const auto loaded = engine.value()->plan_by_id(summary.id.view());
    REP_REQUIRE_OK(loaded);
    REP_CHECK(loaded.value().verify().ok());
    REP_CHECK_EQ(loaded.value().plan_digest, summary.plan_digest);
    sequences.insert(summary.sequence.value());
  }
  REP_CHECK_EQ(sequences.size(), expected_plans);
  REP_CHECK_EQ(*sequences.begin(), std::uint64_t{1});
  REP_CHECK_EQ(*sequences.rbegin(), static_cast<std::uint64_t>(expected_plans));
  const EngineStats final_stats = engine.value()->stats();
  REP_CHECK_EQ(final_stats.plan_count, expected_plans);
  REP_CHECK_EQ(final_stats.idempotency_count, expected_plans);
  REP_CHECK_EQ(final_stats.lineage_count, expected_plans);
}

// ---------------------------------------------------------------------------
// Concurrent store lifecycles
// ---------------------------------------------------------------------------

REP_TEST(Concurrency, concurrent_open_close_on_distinct_stores) {
  constexpr std::size_t kThreads = 4;
  constexpr std::size_t kRounds = 6;
  const std::filesystem::path root = fresh_directory("rep-concurrency-stores");

  std::vector<std::filesystem::path> directories;
  std::vector<std::vector<PlanRequest>> requests;
  for (std::size_t index = 0; index < kThreads; ++index) {
    directories.push_back(root / ("store-" + std::to_string(index)));
    std::vector<PlanRequest> per_round;
    per_round.reserve(kRounds);
    for (std::size_t round = 0; round < kRounds; ++round) {
      const auto request =
          request_for("k-store-" + std::to_string(index) + "-" + std::to_string(round));
      REP_REQUIRE_OK(request);
      per_round.push_back(request.value());
    }
    requests.push_back(std::move(per_round));
  }

  std::vector<WorkerLog> logs(kThreads);
  std::latch start{static_cast<std::ptrdiff_t>(kThreads)};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    threads.emplace_back([&directories, &requests, &logs, &start, index]() {
      WorkerLog& log = logs[index];
      const PlannerId planner{std::string("planner-store-") + std::to_string(index)};
      PlanId first_plan;
      std::size_t committed = 0;
      start.arrive_and_wait();
      for (std::size_t round = 0; round < kRounds; ++round) {
        try {
          const auto engine = open_engine(planner, directories[index]);
          if (!engine.ok()) {
            log.fail("open failed: " + engine.error().message);
            return;
          }
          const std::uint64_t epoch = engine.value()->current_epoch().value();
          if (!(epoch > log.final_epoch)) {
            log.fail("the incarnation epoch did not advance on reopen");
            return;
          }
          log.final_epoch = epoch;
          const RecoveryReport recovery = engine.value()->recovery();
          if (round == 0) {
            if (recovery.recovered) {
              log.fail("a fresh store reported recovery");
              return;
            }
            if (recovery.previous_epoch.value() != 0) {
              log.fail("a fresh store reported a previous epoch");
              return;
            }
          } else {
            if (!recovery.recovered) {
              log.fail("a reopened store did not report recovery");
              return;
            }
            if (recovery.recovered_plan_count != committed) {
              log.fail("the recovered plan count is wrong");
              return;
            }
            if (recovery.recovered_idempotency_count != committed) {
              log.fail("the recovered idempotency count is wrong");
              return;
            }
            if (!(recovery.previous_epoch.value() == log.final_epoch - 1)) {
              log.fail("the reported previous epoch is wrong");
              return;
            }
            const auto previous = engine.value()->plan_by_id(first_plan.view());
            if (!previous.ok() || !previous.value().verify().ok()) {
              log.fail("a plan committed by an earlier incarnation no longer loads");
              return;
            }
          }
          const auto outcome = engine.value()->submit(requests[index][round]);
          if (!outcome.ok()) {
            log.fail("submit failed: " + outcome.error().message);
            return;
          }
          if (outcome.value().kind != OutcomeKind::Planned) {
            log.fail("a distinct request did not plan");
            return;
          }
          if (!outcome.value().plan.verify().ok()) {
            log.fail("the committed plan does not verify");
            return;
          }
          ++committed;
          ++log.submissions;
          ++log.planned;
          if (round == 0) {
            first_plan = outcome.value().plan.id;
          } else if (!(outcome.value().plan.sequence.value() == committed)) {
            log.fail("the plan sequence is not dense across incarnations");
            return;
          }
          if (!(recovery.claimed_epoch.value() == epoch)) {
            log.fail("the recovery report disagrees with the epoch in force");
            return;
          }
          if (!(engine.value()->recovery().claimed_epoch.value() == epoch)) {
            log.fail("the stored recovery report disagrees with the epoch in force");
            return;
          }
          // The engine's own counters agree with what this thread has done.
          const EngineStats stats = engine.value()->stats();
          if (!(stats.sequence.value() == committed)) {
            log.fail("the store sequence is not dense");
            return;
          }
          if (stats.plan_count != committed || stats.idempotency_count != committed) {
            log.fail("the reopened store lost a committed plan");
            return;
          }
        } catch (const std::exception& error) {
          log.fail(std::string("exception: ") + error.what());
          return;
        } catch (...) {
          log.fail("unknown exception");
          return;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  report_failures(logs);
  for (std::size_t index = 0; index < kThreads; ++index) {
    REP_CHECK_EQ(logs[index].planned, kRounds);
    REP_CHECK_EQ(logs[index].final_epoch, static_cast<std::uint64_t>(kRounds));
    const auto stats = PlanStore::inspect(directories[index]);
    REP_REQUIRE_OK(stats);
    REP_CHECK_EQ(stats.value().plan_count, kRounds);
    REP_CHECK_EQ(stats.value().idempotency_count, kRounds);
    REP_CHECK_EQ(stats.value().sequence.value(), static_cast<std::uint64_t>(kRounds));
    REP_CHECK_EQ(stats.value().epoch.value(), static_cast<std::uint64_t>(kRounds));
  }
}

REP_TEST_MAIN()
