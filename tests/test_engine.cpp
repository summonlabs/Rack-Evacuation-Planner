// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Engine lifecycle: control epoch, idempotent replay, lineage revisions,
// durability, recovery, and the fencing answers the engine owes its callers.

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rep/rep.hpp>

#include "planner_fixture.hpp"
#include "temp_dir.hpp"
#include "testkit.hpp"

using namespace repfix;

namespace {

[[nodiscard]] rep::Result<std::unique_ptr<rep::PlanEngine>> open_memory_engine(
    std::string_view planner = "planner-01") {
  rep::EngineOptions options;
  auto id = rep::PlannerId::parse(planner);
  if (!id.ok()) {
    return id.error();
  }
  options.planner = id.value();
  return rep::PlanEngine::open(std::move(options));
}

[[nodiscard]] rep::Result<std::unique_ptr<rep::PlanEngine>> open_durable_engine(
    const std::string& directory, std::string_view planner = "planner-01") {
  rep::EngineOptions options;
  auto id = rep::PlannerId::parse(planner);
  if (!id.ok()) {
    return id.error();
  }
  options.planner = id.value();
  options.store_directory = directory;
  return rep::PlanEngine::open(std::move(options));
}

// Builds a request identical in every field, keyed on the caller's choice.
[[nodiscard]] rep::Result<rep::PlanRequest> request_from(const std::string& text,
                                                        std::string_view key_suffix,
                                                        std::uint64_t expected_epoch) {
  auto document = rep::parse_scenario(text);
  if (!document.ok()) {
    return document.error();
  }
  const rep::PlanRequest& base = document.value().request;
  auto key = rep::IdempotencyKey::parse(base.idempotency_key.str() + std::string(key_suffix));
  if (!key.ok()) {
    return key.error();
  }
  return rep::PlanRequest::make(key.value(), base.requested_by, rep::Epoch{expected_epoch},
                                base.evaluation_time, base.isolation, base.lineage,
                                base.evidence_sources, base.evidence);
}

} // namespace

REP_TEST(engine, in_memory_engine_starts_a_fresh_incarnation) {
  auto engine = open_memory_engine();
  REP_REQUIRE(engine.ok());
  REP_CHECK_EQ(engine.value()->current_epoch().value(), std::uint64_t{1});
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{0});
  const rep::EngineStats stats = engine.value()->stats();
  REP_CHECK(!stats.durable);
  REP_CHECK_EQ(stats.plan_count, std::size_t{0});
  const rep::RecoveryReport recovery = engine.value()->recovery();
  REP_CHECK(!recovery.durable);
  REP_CHECK(!recovery.recovered);
  REP_CHECK_EQ(recovery.claimed_epoch.value(), std::uint64_t{1});
  REP_CHECK(engine.value()->history().empty());
}

REP_TEST(engine, durable_engine_claims_a_new_epoch_on_open) {
  reptest::TempDir directory("engine-claim");
  auto engine = open_durable_engine(directory.path());
  REP_REQUIRE(engine.ok());
  REP_CHECK_EQ(engine.value()->current_epoch().value(), std::uint64_t{1});
  const rep::RecoveryReport recovery = engine.value()->recovery();
  REP_CHECK(recovery.durable);
  REP_CHECK(!recovery.recovered);
  REP_CHECK_EQ(recovery.previous_epoch.value(), std::uint64_t{0});
  REP_CHECK_EQ(recovery.claimed_epoch.value(), std::uint64_t{1});
  REP_CHECK(engine.value()->stats().durable);
}

REP_TEST(engine, submit_commits_one_plan_with_a_derived_identity) {
  auto engine = open_memory_engine();
  REP_REQUIRE(engine.ok());
  auto request = request_from(standard().text(), "-a", 0);
  REP_REQUIRE(request.ok());
  auto outcome = engine.value()->submit(request.value());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().kind == rep::OutcomeKind::Planned);
  REP_REQUIRE(outcome.value().has_plan);
  const rep::Plan& plan = outcome.value().plan;
  REP_CHECK_EQ(plan.id.str(), std::string("planner-01-000000000001"));
  REP_CHECK_EQ(plan.sequence.value(), std::uint64_t{1});
  REP_CHECK_EQ(plan.revision.value(), std::uint32_t{1});
  REP_CHECK_EQ(plan.lineage.str(), plan.id.str());
  REP_CHECK_EQ(plan.epoch.value(), std::uint64_t{1});
  REP_CHECK(plan.verify().ok());

  auto loaded = engine.value()->plan_by_id(plan.id.str());
  REP_REQUIRE(loaded.ok());
  REP_CHECK(loaded.value() == plan);
  auto by_key = engine.value()->plan_by_idempotency_key(request.value().idempotency_key.str());
  REP_REQUIRE(by_key.ok());
  REP_CHECK(by_key.value() == plan);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{1});
}

REP_TEST(engine, replay_is_resolved_before_stale_epoch_rejection) {
  auto engine = open_memory_engine();
  REP_REQUIRE(engine.ok());
  auto first = request_from(standard().text(), "-a", 1);
  REP_REQUIRE(first.ok());
  auto planned = engine.value()->submit(first.value());
  REP_REQUIRE(planned.ok());
  REP_REQUIRE(planned.value().has_plan);
  const rep::Digest digest = planned.value().plan.plan_digest;

  // The same already-committed request, now carrying a stale epoch view: the
  // lost response must not cause a second commit and must not be refused.
  auto replay = request_from(standard().text(), "-a", 999);
  REP_REQUIRE(replay.ok());
  auto outcome = engine.value()->submit(replay.value());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().kind == rep::OutcomeKind::Replayed);
  REP_REQUIRE(outcome.value().has_plan);
  REP_CHECK(outcome.value().plan.plan_digest == digest);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{1});
  REP_CHECK_EQ(engine.value()->stats().plan_count, std::size_t{1});
}

REP_TEST(engine, a_reused_key_with_a_different_request_is_a_conflict) {
  auto engine = open_memory_engine();
  REP_REQUIRE(engine.ok());
  auto first = request_from(standard().text(), "-a", 0);
  REP_REQUIRE(first.ok());
  REP_REQUIRE(engine.value()->submit(first.value()).ok());

  Scenario changed = standard();
  changed.set_body("candidate_offers", "");
  auto second = request_from(changed.text(), "-a", 0);
  REP_REQUIRE(second.ok());
  auto outcome = engine.value()->submit(second.value());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().kind == rep::OutcomeKind::RejectedIdempotencyConflict);
  REP_CHECK(outcome.value().error.code == rep::ErrorCode::IdempotencyConflict);
  REP_CHECK(!outcome.value().has_plan);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{1});
}

REP_TEST(engine, a_stale_epoch_view_is_refused_for_a_new_request) {
  auto engine = open_memory_engine();
  REP_REQUIRE(engine.ok());
  auto stale = request_from(standard().text(), "-a", 42);
  REP_REQUIRE(stale.ok());
  auto outcome = engine.value()->submit(stale.value());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().kind == rep::OutcomeKind::RejectedStaleEpoch);
  REP_CHECK(outcome.value().error.code == rep::ErrorCode::StaleEpoch);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{0});
  REP_CHECK(engine.value()->history().empty());

  auto correct = request_from(standard().text(), "-a", 0);
  REP_REQUIRE(correct.ok());
  REP_CHECK(engine.value()->submit(correct.value()).ok());
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{1});
}

REP_TEST(engine, a_lineage_advances_its_revision_and_supersedes) {
  auto engine = open_memory_engine();
  REP_REQUIRE(engine.ok());
  auto first = request_from(standard().text(), "-a", 0);
  REP_REQUIRE(first.ok());
  auto planned_first = engine.value()->submit(first.value());
  REP_REQUIRE(planned_first.ok());
  REP_REQUIRE(planned_first.value().has_plan);
  const rep::Plan& earlier = planned_first.value().plan;

  // Second revision of the same lineage.
  auto document = rep::parse_scenario(standard().text());
  REP_REQUIRE(document.ok());
  auto lineage = rep::LineageId::parse("lineage-alpha");
  REP_REQUIRE(lineage.ok());
  auto second_request = rep::PlanRequest::make(
      rep::IdempotencyKey::parse("req-1-b").value(), document.value().request.requested_by,
      rep::Epoch{0}, document.value().request.evaluation_time, document.value().request.isolation,
      lineage.value(), document.value().request.evidence_sources, document.value().request.evidence);
  REP_REQUIRE(second_request.ok());
  auto planned_second = engine.value()->submit(second_request.value());
  REP_REQUIRE(planned_second.ok());
  REP_REQUIRE(planned_second.value().has_plan);
  const rep::Plan& later = planned_second.value().plan;
  REP_CHECK_EQ(later.revision.value(), std::uint32_t{1});
  REP_CHECK_EQ(later.lineage.str(), std::string("lineage-alpha"));

  auto third_request = rep::PlanRequest::make(
      rep::IdempotencyKey::parse("req-1-c").value(), document.value().request.requested_by,
      rep::Epoch{0}, document.value().request.evaluation_time, document.value().request.isolation,
      lineage.value(), document.value().request.evidence_sources, document.value().request.evidence);
  REP_REQUIRE(third_request.ok());
  auto planned_third = engine.value()->submit(third_request.value());
  REP_REQUIRE(planned_third.ok());
  REP_REQUIRE(planned_third.value().has_plan);
  REP_CHECK_EQ(planned_third.value().plan.revision.value(), std::uint32_t{2});

  auto superseded = engine.value()->is_superseded(later.id.str());
  REP_REQUIRE(superseded.ok());
  REP_CHECK(superseded.value());
  auto earliest = engine.value()->is_superseded(earlier.id.str());
  REP_REQUIRE(earliest.ok());
  REP_CHECK(!earliest.value());

  auto latest = engine.value()->latest_for_lineage("lineage-alpha");
  REP_REQUIRE(latest.ok());
  REP_CHECK(latest.value() == planned_third.value().plan);

  auto assessment = engine.value()->assess(later.id.str(), document.value().request.evidence);
  REP_REQUIRE(assessment.ok());
  REP_CHECK(assessment.value().superseded);
  REP_CHECK(assessment.value().verdict == rep::SafetyVerdict::Stale);
  REP_CHECK_EQ(assessment.value().latest_revision.value(), std::uint32_t{2});
}

REP_TEST(engine, reading_an_unknown_identity_is_not_found) {
  auto engine = open_memory_engine();
  REP_REQUIRE(engine.ok());
  REP_CHECK(engine.value()->plan_by_id("nope").error().code == rep::ErrorCode::NotFound);
  REP_CHECK(engine.value()->plan_by_idempotency_key("nope").error().code == rep::ErrorCode::NotFound);
  REP_CHECK(engine.value()->latest_for_lineage("nope").error().code == rep::ErrorCode::NotFound);
  REP_CHECK(engine.value()->is_superseded("nope").error().code == rep::ErrorCode::NotFound);
  REP_CHECK(engine.value()->assess("nope", rep::EvidenceBundle{}).error().code ==
            rep::ErrorCode::NotFound);
}

REP_TEST(engine, history_and_stats_describe_the_committed_work) {
  reptest::TempDir directory("engine-history");
  auto engine = open_durable_engine(directory.path());
  REP_REQUIRE(engine.ok());
  for (int index = 0; index < 3; ++index) {
    auto request = request_from(standard().text(), "-" + std::to_string(index), 0);
    REP_REQUIRE(request.ok());
    auto outcome = engine.value()->submit(request.value());
    REP_REQUIRE(outcome.ok());
    REP_REQUIRE(outcome.value().committed());
  }
  const std::vector<rep::PlanSummary> history = engine.value()->history();
  REP_REQUIRE(history.size() == 3);
  for (std::size_t index = 1; index < history.size(); ++index) {
    REP_CHECK(history[index - 1].sequence < history[index].sequence);
  }
  REP_CHECK_EQ(history.front().status, rep::PlanStatus::Complete);
  REP_CHECK_EQ(history.front().assignment_count, std::size_t{1});
  const rep::EngineStats stats = engine.value()->stats();
  REP_CHECK_EQ(stats.plan_count, std::size_t{3});
  REP_CHECK_EQ(stats.idempotency_count, std::size_t{3});
  REP_CHECK_EQ(stats.lineage_count, std::size_t{3});
  REP_CHECK_EQ(stats.sequence.value(), std::uint64_t{3});
  REP_CHECK_EQ(stats.store_directory, directory.path());
  REP_CHECK(stats.last_plan_digest == history.back().plan_digest);
}

REP_TEST(engine, restart_advances_the_epoch_and_keeps_the_plans) {
  reptest::TempDir directory("engine-restart");
  rep::Digest digest;
  std::string plan_id;
  std::string key;
  {
    auto engine = open_durable_engine(directory.path());
    REP_REQUIRE(engine.ok());
    auto request = request_from(standard().text(), "-a", 0);
    REP_REQUIRE(request.ok());
    key = request.value().idempotency_key.str();
    auto outcome = engine.value()->submit(request.value());
    REP_REQUIRE(outcome.ok());
    REP_REQUIRE(outcome.value().has_plan);
    digest = outcome.value().plan.plan_digest;
    plan_id = outcome.value().plan.id.str();
  }
  auto reopened = open_durable_engine(directory.path());
  REP_REQUIRE(reopened.ok());
  REP_CHECK_EQ(reopened.value()->current_epoch().value(), std::uint64_t{2});
  const rep::RecoveryReport recovery = reopened.value()->recovery();
  REP_CHECK(recovery.recovered);
  REP_CHECK_EQ(recovery.recovered_sequence.value(), std::uint64_t{1});
  REP_CHECK_EQ(recovery.previous_epoch.value(), std::uint64_t{1});
  REP_CHECK_EQ(recovery.claimed_epoch.value(), std::uint64_t{2});
  REP_CHECK_EQ(recovery.recovered_plan_count, std::size_t{1});
  REP_CHECK_EQ(recovery.recovered_idempotency_count, std::size_t{1});

  auto loaded = reopened.value()->plan_by_id(plan_id);
  REP_REQUIRE(loaded.ok());
  REP_CHECK(loaded.value().plan_digest == digest);
  REP_CHECK(loaded.value().verify().ok());

  // The plan was sealed by a previous incarnation, so it is stale now.
  auto assessment = reopened.value()->assess(plan_id, rep::EvidenceBundle{});
  REP_REQUIRE(assessment.ok());
  REP_CHECK(!assessment.value().same_incarnation);
  REP_CHECK(assessment.value().verdict == rep::SafetyVerdict::Stale);
}

REP_TEST(engine, replay_survives_a_restart) {
  reptest::TempDir directory("engine-replay-restart");
  rep::Digest digest;
  {
    auto engine = open_durable_engine(directory.path());
    REP_REQUIRE(engine.ok());
    auto request = request_from(standard().text(), "-a", 0);
    REP_REQUIRE(request.ok());
    auto outcome = engine.value()->submit(request.value());
    REP_REQUIRE(outcome.ok());
    REP_REQUIRE(outcome.value().has_plan);
    digest = outcome.value().plan.plan_digest;
  }
  auto reopened = open_durable_engine(directory.path());
  REP_REQUIRE(reopened.ok());
  // The same request, submitted to a successor incarnation that never saw it.
  auto replay = request_from(standard().text(), "-a", 999);
  REP_REQUIRE(replay.ok());
  auto outcome = reopened.value()->submit(replay.value());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().kind == rep::OutcomeKind::Replayed);
  REP_REQUIRE(outcome.value().has_plan);
  REP_CHECK(outcome.value().plan.plan_digest == digest);
  REP_CHECK_EQ(reopened.value()->last_sequence().value(), std::uint64_t{1});
}

REP_TEST(engine, a_refused_request_leaves_the_sequence_untouched) {
  auto engine = open_memory_engine();
  REP_REQUIRE(engine.ok());
  Scenario contradictory = standard();
  contradictory.blocks.push_back(make_block(
      "capacity", "other-capacity", "capacity-2", "4", "1",
      "destination destination=rack_slot:rack-B available=cpu_millicores:1\n"));
  auto request = request_from(contradictory.text(), "-a", 0);
  REP_REQUIRE(request.ok());
  auto outcome = engine.value()->submit(request.value());
  REP_REQUIRE(outcome.ok());
  REP_CHECK(outcome.value().kind == rep::OutcomeKind::RejectedInput);
  REP_CHECK(outcome.value().error.code == rep::ErrorCode::DuplicateIdentity);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{0});

  // The key was not consumed, so a corrected request may reuse it.
  auto corrected = request_from(standard().text(), "-a", 0);
  REP_REQUIRE(corrected.ok());
  auto second = engine.value()->submit(corrected.value());
  REP_REQUIRE(second.ok());
  REP_CHECK(second.value().kind == rep::OutcomeKind::Planned);
}

REP_TEST(engine, two_engines_cannot_share_one_store_directory) {
  reptest::TempDir directory("engine-lock");
  auto first = open_durable_engine(directory.path());
  REP_REQUIRE(first.ok());
  auto second = open_durable_engine(directory.path());
  REP_CHECK(!second.ok());
  if (!second.ok()) {
    REP_CHECK(second.error().code == rep::ErrorCode::Locked);
  }
}

REP_TEST(engine, a_store_directory_that_is_a_file_is_refused) {
  reptest::TempDir directory("engine-notdir");
  const std::string file_path = directory.child("not-a-directory");
  {
    std::ofstream handle(file_path, std::ios::binary);
    REP_REQUIRE(handle.good());
    handle << "x";
  }
  auto engine = open_durable_engine(file_path);
  REP_CHECK(!engine.ok());
  if (!engine.ok()) {
    REP_CHECK(engine.error().code == rep::ErrorCode::NotADirectory);
  }
}

REP_TEST(engine, assess_reports_moved_evidence_within_one_incarnation) {
  auto engine = open_memory_engine();
  REP_REQUIRE(engine.ok());
  auto request = request_from(standard().text(), "-a", 0);
  REP_REQUIRE(request.ok());
  auto outcome = engine.value()->submit(request.value());
  REP_REQUIRE(outcome.ok());
  REP_REQUIRE(outcome.value().has_plan);
  const std::string plan_id = outcome.value().plan.id.str();

  auto assessment = engine.value()->assess(plan_id, request.value().evidence);
  REP_REQUIRE(assessment.ok());
  REP_CHECK(assessment.value().same_incarnation);
  REP_CHECK(!assessment.value().superseded);
  REP_CHECK(assessment.value().staleness.empty());
  REP_CHECK(!assessment.value().any_stream_missing);
  REP_CHECK(assessment.value().verdict == rep::SafetyVerdict::Safe);

  Scenario moved = standard();
  for (Block& block : moved.blocks) {
    if (block.kind == "candidate_offers") {
      block.generation = "5";
    }
  }
  auto moved_request = request_from(moved.text(), "-a", 0);
  REP_REQUIRE(moved_request.ok());
  auto moved_assessment = engine.value()->assess(plan_id, moved_request.value().evidence);
  REP_REQUIRE(moved_assessment.ok());
  REP_CHECK(moved_assessment.value().verdict == rep::SafetyVerdict::Stale);
  REP_CHECK(!moved_assessment.value().staleness.empty());
}

REP_TEST(engine, an_invalid_planner_identity_is_refused) {
  rep::EngineOptions options;
  options.planner = rep::PlannerId("has space");
  auto engine = rep::PlanEngine::open(std::move(options));
  REP_CHECK(!engine.ok());
  if (!engine.ok()) {
    REP_CHECK(engine.error().code == rep::ErrorCode::InvalidIdentifier);
  }
}

REP_TEST_MAIN()
