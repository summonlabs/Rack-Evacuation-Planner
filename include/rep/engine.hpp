// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_ENGINE_HPP
#define REP_ENGINE_HPP

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rep/export.hpp"
#include "rep/plan.hpp"
#include "rep/request.hpp"
#include "rep/status.hpp"
#include "rep/store.hpp"
#include "rep/types.hpp"

namespace rep {

// What happened to one submission.
struct REP_API PlanOutcome {
  OutcomeKind kind{OutcomeKind::RejectedInput};
  Plan plan;             // valid for Planned and Replayed
  Error error;           // set for every rejection
  bool has_plan{false};

  [[nodiscard]] bool committed() const noexcept {
    return kind == OutcomeKind::Planned || kind == OutcomeKind::Replayed;
  }
};

struct REP_API PlanSummary {
  PlanId id;
  PlanRevision revision;
  Sequence sequence;
  RackId rack;
  IsolationKind isolation{IsolationKind::Depower};
  PlanStatus status{PlanStatus::Indeterminate};
  std::size_t assignment_count{0};
  std::size_t residual_count{0};
  std::size_t out_of_scope_count{0};
  Digest plan_digest;
};

// The complete fencing answer for one stored plan: whether it was sealed by
// the current incarnation, whether a later revision supersedes it, and whether
// any bound evidence stream has moved.
struct REP_API PlanAssessment {
  SafetyVerdict verdict{SafetyVerdict::Indeterminate};
  bool same_incarnation{false};
  bool superseded{false};
  // True when a bound stream is absent from the supplied bundle entirely,
  // which is a stronger statement than "the generation moved".
  bool any_stream_missing{false};
  PlanRevision latest_revision;
  std::vector<StalenessFinding> staleness;
};

// What opening the engine inherited, and what it explicitly did not.
struct REP_API RecoveryReport {
  bool durable{false};
  bool recovered{false};
  Sequence recovered_sequence;
  Epoch previous_epoch;
  Epoch claimed_epoch;             // the incarnation this engine now owns
  WriterId writer;
  std::size_t recovered_plan_count{0};
  std::size_t recovered_idempotency_count{0};
};

struct REP_API EngineStats {
  PlannerId planner;
  Epoch epoch;
  Sequence sequence;
  WriterId writer;
  bool durable{false};
  std::size_t plan_count{0};
  std::size_t lineage_count{0};
  std::size_t idempotency_count{0};
  std::string store_directory;
  std::uint32_t store_format_version{0};
  Digest last_plan_digest;
};

struct REP_API EngineOptions {
  PlannerId planner;
  // Empty means a non-durable in-memory engine: plans are process-local and
  // nothing survives restart.
  std::filesystem::path store_directory;
  std::size_t max_plans{4096};
  std::size_t max_obligations{100000};
  std::size_t max_candidates{100000};
  // Supplies the timestamp of the epoch claim only.  Plan evaluation always
  // uses PlanRequest::evaluation_time, never a clock.
  std::function<UnixNanos()> wall_clock;
};

// The planning engine.  It owns plan identity, revision, sequence, and the
// control epoch; it owns no rack, capacity, or workload truth.
//
// Concurrency model: one mutex guards all mutable state.  Every public method
// takes it exactly once, no public method calls another public method, no
// callback is invoked while it is held, and no reference to internal state is
// returned to a caller.
class REP_API PlanEngine {
 public:
  [[nodiscard]] static Result<std::unique_ptr<PlanEngine>> open(EngineOptions options);
  ~PlanEngine();

  PlanEngine(const PlanEngine&) = delete;
  PlanEngine& operator=(const PlanEngine&) = delete;
  PlanEngine(PlanEngine&&) = delete;
  PlanEngine& operator=(PlanEngine&&) = delete;

  // The single consequential operation.  Replay of an already committed
  // request is resolved before any stale-precondition rejection, so a lost
  // response never causes a second commit.
  [[nodiscard]] Result<PlanOutcome> submit(const PlanRequest& request);

  [[nodiscard]] Result<Plan> plan_by_id(std::string_view plan_id) const;
  [[nodiscard]] Result<Plan> plan_by_idempotency_key(std::string_view key) const;
  [[nodiscard]] Result<Plan> latest_for_lineage(std::string_view lineage) const;
  [[nodiscard]] std::vector<PlanSummary> history() const;
  // True when a later revision exists on the same lineage.
  [[nodiscard]] Result<bool> is_superseded(std::string_view plan_id) const;
  [[nodiscard]] PlanSummary summarise(const Plan& plan) const;
  // Fences a stored plan against the current incarnation and one evidence
  // bundle.  This is the only supported way to ask whether a stored plan still
  // proves anything.
  [[nodiscard]] Result<PlanAssessment> assess(std::string_view plan_id,
                                              const EvidenceBundle& current) const;

  [[nodiscard]] EngineStats stats() const;
  [[nodiscard]] RecoveryReport recovery() const;
  [[nodiscard]] Epoch current_epoch() const noexcept;
  [[nodiscard]] Sequence last_sequence() const noexcept;

 private:
  PlanEngine() = default;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace rep

#endif // REP_ENGINE_HPP
