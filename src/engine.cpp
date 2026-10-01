// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "detail/planner.hpp"
#include "detail/platform.hpp"
#include "rep/status.hpp"
#include "rep/version.hpp"

namespace rep {
namespace {

constexpr std::uint32_t kSequenceDigits = 12;

[[nodiscard]] std::string zero_pad(std::uint64_t value, std::uint32_t width) {
  std::string digits = std::to_string(value);
  if (digits.size() < width) {
    digits.insert(0, width - digits.size(), '0');
  }
  return digits;
}

[[nodiscard]] Result<void> validate_options(const EngineOptions& options) {
  if (!is_valid_identifier(options.planner.view())) {
    return make_error(ErrorCode::InvalidIdentifier,
                      "planner identity is not a valid identifier: " +
                          describe_identifier_problem(options.planner.view()),
                      "planner");
  }
  if (options.max_plans == 0) {
    return make_error(ErrorCode::InvalidArgument, "max_plans must be at least one", "max_plans");
  }
  if (options.max_obligations == 0 || options.max_candidates == 0) {
    return make_error(ErrorCode::InvalidArgument,
                      "obligation and candidate limits must be at least one", "max_obligations");
  }
  // Wave numbers are 32-bit, so the obligation limit must fit in 32 bits for
  // the wave computation to be provably overflow free.
  if (options.max_obligations > (std::numeric_limits<std::uint32_t>::max)()) {
    return make_error(ErrorCode::InvalidArgument,
                      "max_obligations must fit in 32 bits", "max_obligations");
  }
  return {};
}

} // namespace

struct PlanEngine::Impl {
  mutable std::mutex mutex;
  PlannerId planner;
  StoreState state;
  std::unique_ptr<PlanStore> store;
  RecoveryReport recovery;
  detail::PlanLimits limits;
  std::size_t max_plans{4096};
  bool durable{false};

  [[nodiscard]] Epoch epoch() const noexcept { return state.epoch; }
  [[nodiscard]] Sequence sequence() const noexcept { return state.sequence; }
};

PlanEngine::~PlanEngine() = default;

Result<std::unique_ptr<PlanEngine>> PlanEngine::open(EngineOptions options) {
  if (!version_is_consistent()) {
    return make_error(ErrorCode::VersionMismatch,
                      "the headers this caller compiled against are version " +
                          std::string(version_string()) + ", but the linked library is version " +
                          std::string(build_version_string()),
                      "version");
  }
  const auto valid = validate_options(options);
  if (!valid.ok()) {
    return valid.error();
  }

  const UnixNanos opened_at =
      options.wall_clock ? options.wall_clock() : UnixNanos{0};

  std::unique_ptr<PlanEngine> engine(new PlanEngine());
  auto impl = std::make_unique<Impl>();
  impl->planner = options.planner;
  impl->limits.max_obligations = options.max_obligations;
  impl->limits.max_candidates = options.max_candidates;
  impl->max_plans = options.max_plans;

  if (options.store_directory.empty()) {
    // Non-durable: plans live in this process only, and nothing survives
    // restart.  The incarnation still starts at one so that plan epochs are
    // meaningful across both modes.
    impl->state.sequence = Sequence{0};
    impl->state.epoch = Epoch{1};
    impl->state.writer = WriterId(detail::process_token());
    impl->state.opened_at = opened_at;
    impl->recovery.durable = false;
    impl->recovery.recovered = false;
    impl->recovery.recovered_sequence = Sequence{0};
    impl->recovery.previous_epoch = Epoch{0};
    impl->recovery.claimed_epoch = Epoch{1};
    impl->recovery.writer = impl->state.writer;
  } else {
    PlanStoreOptions store_options;
    store_options.directory = options.store_directory;
    store_options.max_plans = options.max_plans;
    auto store = PlanStore::open(store_options);
    if (!store.ok()) {
      return store.error();
    }
    impl->store = store.take();
    impl->durable = true;

    const StoreState& loaded = impl->store->state();
    const Sequence inherited_sequence = loaded.sequence;
    const Epoch inherited_epoch = loaded.epoch;
    const std::size_t inherited_plans = loaded.plans.size();
    const std::size_t inherited_keys = loaded.keys.size();

    const auto claimed = checked_increment(inherited_epoch);
    if (!claimed.has_value()) {
      return make_error(ErrorCode::Overflow,
                        "the store control epoch cannot advance without wrapping", "epoch");
    }
    impl->state.sequence = inherited_sequence;
    impl->state.epoch = claimed.value();
    impl->state.writer = WriterId(detail::process_token());
    impl->state.opened_at = opened_at;
    impl->state.plans = loaded.plans;
    impl->state.keys = loaded.keys;

    // Claiming the incarnation is a durable publication in its own right: a
    // successor that dies here leaves the epoch already advanced, so no later
    // process can be mistaken for this one.
    auto claim = StoreState::make(impl->state.sequence, impl->state.epoch,
                                          impl->state.writer, impl->state.opened_at,
                                          impl->state.plans, impl->state.keys, options.max_plans);
    if (!claim.ok()) {
      return claim.error();
    }
    const auto committed = impl->store->commit(claim.value());
    if (!committed.ok()) {
      return committed.error();
    }
    impl->recovery.durable = true;
    impl->recovery.recovered = impl->store->has_published_generation();
    impl->recovery.recovered_sequence = inherited_sequence;
    impl->recovery.previous_epoch = inherited_epoch;
    impl->recovery.claimed_epoch = impl->state.epoch;
    impl->recovery.writer = impl->state.writer;
    impl->recovery.recovered_plan_count = inherited_plans;
    impl->recovery.recovered_idempotency_count = inherited_keys;
  }

  engine->impl_ = std::move(impl);
  return engine;
}

Result<PlanOutcome> PlanEngine::submit(const PlanRequest& request) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);

  PlanOutcome outcome;
  const Digest request_digest = request.digest();

  // Replay is resolved before any stale-precondition check.  The request is
  // demonstrably the same already-committed operation, so a lost response must
  // not cause a second commit, even when the caller's epoch view has moved on.
  if (const IdempotencyRecord* record =
          impl_->state.find_key(request.idempotency_key.view());
      record != nullptr) {
    if (!(record->request_digest == request_digest)) {
      outcome.kind = OutcomeKind::RejectedIdempotencyConflict;
      outcome.error = make_error(
          ErrorCode::IdempotencyConflict,
          "idempotency key " + request.idempotency_key.str() +
              " is already committed for a different request",
          "idempotency_key");
      return outcome;
    }
    const Plan* committed = impl_->state.find_plan(record->plan_id.view());
    if (committed == nullptr) {
      return make_error(ErrorCode::Internal,
                        "the store records a committed key whose plan is missing", "keys");
    }
    outcome.kind = OutcomeKind::Replayed;
    outcome.plan = *committed;
    outcome.has_plan = true;
    return outcome;
  }

  if (request.expected_epoch.value() != 0 && !(request.expected_epoch == impl_->state.epoch)) {
    outcome.kind = OutcomeKind::RejectedStaleEpoch;
    outcome.error = make_error(ErrorCode::StaleEpoch,
                               "request expects control epoch " +
                                   std::to_string(request.expected_epoch.value()) +
                                   " but the current incarnation is " +
                                   std::to_string(impl_->state.epoch.value()),
                               "expected_epoch");
    return outcome;
  }

  const auto next_sequence = checked_increment(impl_->state.sequence);
  if (!next_sequence.has_value()) {
    return make_error(ErrorCode::Overflow, "the plan sequence cannot advance without wrapping",
                      "sequence");
  }

  detail::PlanSealContext context;
  context.planner = impl_->planner;
  context.sequence = next_sequence.value();
  context.epoch = impl_->state.epoch;
  context.plan_id = PlanId(impl_->planner.str() + "-" +
                           zero_pad(next_sequence.value().value(), kSequenceDigits));
  context.lineage = request.lineage;

  PlanRevision revision{1};
  if (!request.lineage.empty()) {
    if (const Plan* latest =
            impl_->state.latest_for_lineage(request.lineage.view());
        latest != nullptr) {
      const auto next_revision = checked_increment(latest->revision);
      if (!next_revision.has_value()) {
        return make_error(ErrorCode::Overflow, "the plan revision cannot advance without wrapping",
                          "revision");
      }
      revision = next_revision.value();
    }
  }
  context.revision = revision;
  if (context.lineage.empty()) {
    context.lineage = LineageId(context.plan_id.str());
  }

  auto planned = detail::plan_evacuation(request, context, impl_->limits);
  if (!planned.ok()) {
    outcome.kind = OutcomeKind::RejectedInput;
    outcome.error = planned.error();
    return outcome;
  }
  Plan plan = planned.take();

  std::vector<Plan> plans = impl_->state.plans;
  plans.push_back(plan);
  std::vector<IdempotencyRecord> keys = impl_->state.keys;
  keys.push_back(IdempotencyRecord{request.idempotency_key, request_digest, plan.id,
                                   plan.revision, plan.sequence, OutcomeKind::Planned});

  auto next = StoreState::make(plan.sequence, impl_->state.epoch, impl_->state.writer,
                                       impl_->state.opened_at, std::move(plans), std::move(keys),
                                       impl_->max_plans);
  if (!next.ok()) {
    return next.error();
  }

  if (impl_->store != nullptr) {
    const auto committed = impl_->store->commit(next.value());
    if (!committed.ok()) {
      return committed.error();
    }
  }
  impl_->state = next.take();

  outcome.kind = OutcomeKind::Planned;
  outcome.plan = std::move(plan);
  outcome.has_plan = true;
  return outcome;
}

Result<Plan> PlanEngine::plan_by_id(std::string_view plan_id) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const Plan* plan = impl_->state.find_plan(plan_id);
  if (plan == nullptr) {
    return make_error(ErrorCode::NotFound, "no plan with that identity", std::string(plan_id));
  }
  return *plan;
}

Result<Plan> PlanEngine::plan_by_idempotency_key(std::string_view key) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const IdempotencyRecord* record = impl_->state.find_key(key);
  if (record == nullptr) {
    return make_error(ErrorCode::NotFound, "no committed request with that idempotency key",
                      std::string(key));
  }
  const Plan* plan = impl_->state.find_plan(record->plan_id.view());
  if (plan == nullptr) {
    return make_error(ErrorCode::Internal,
                      "the store records a committed key whose plan is missing", "keys");
  }
  return *plan;
}

Result<Plan> PlanEngine::latest_for_lineage(std::string_view lineage) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const Plan* plan = impl_->state.latest_for_lineage(lineage);
  if (plan == nullptr) {
    return make_error(ErrorCode::NotFound, "no plan on that lineage", std::string(lineage));
  }
  return *plan;
}

std::vector<PlanSummary> PlanEngine::history() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<PlanSummary> summaries;
  summaries.reserve(impl_->state.plans.size());
  for (const Plan& plan : impl_->state.plans) {
    summaries.push_back(summarise(plan));
  }
  return summaries;
}

Result<bool> PlanEngine::is_superseded(std::string_view plan_id) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const Plan* plan = impl_->state.find_plan(plan_id);
  if (plan == nullptr) {
    return make_error(ErrorCode::NotFound, "no plan with that identity", std::string(plan_id));
  }
  const Plan* latest = impl_->state.latest_for_lineage(plan->lineage.view());
  if (latest == nullptr) {
    return false;
  }
  return plan->revision < latest->revision;
}

PlanSummary PlanEngine::summarise(const Plan& plan) const {
  PlanSummary summary;
  summary.id = plan.id;
  summary.revision = plan.revision;
  summary.sequence = plan.sequence;
  summary.rack = plan.source_rack;
  summary.isolation = plan.isolation;
  summary.status = plan.status;
  summary.assignment_count = plan.assignments.size();
  summary.residual_count = plan.residuals.size();
  summary.out_of_scope_count = plan.out_of_scope.size();
  summary.plan_digest = plan.plan_digest;
  return summary;
}

Result<PlanAssessment> PlanEngine::assess(std::string_view plan_id,
                                         const EvidenceBundle& current) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const Plan* plan = impl_->state.find_plan(plan_id);
  if (plan == nullptr) {
    return make_error(ErrorCode::NotFound, "no plan with that identity", std::string(plan_id));
  }
  PlanAssessment assessment;
  assessment.same_incarnation = plan->epoch == impl_->state.epoch;
  assessment.latest_revision = plan->revision;
  if (const Plan* latest = impl_->state.latest_for_lineage(plan->lineage.view());
      latest != nullptr) {
    assessment.latest_revision = latest->revision;
    assessment.superseded = plan->revision < latest->revision;
  }
  assessment.staleness = plan->staleness(current, assessment.any_stream_missing);
  if (!assessment.same_incarnation || assessment.superseded) {
    assessment.verdict = SafetyVerdict::Stale;
  } else {
    assessment.verdict = plan->verdict(assessment.staleness);
  }
  return assessment;
}

EngineStats PlanEngine::stats() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  EngineStats stats;
  stats.planner = impl_->planner;
  stats.epoch = impl_->state.epoch;
  stats.sequence = impl_->state.sequence;
  stats.writer = impl_->state.writer;
  stats.durable = impl_->durable;
  stats.plan_count = impl_->state.plans.size();
  stats.idempotency_count = impl_->state.keys.size();
  std::set<std::string> lineages;
  for (const Plan& plan : impl_->state.plans) {
    lineages.insert(plan.lineage.str());
  }
  stats.lineage_count = lineages.size();
  if (!impl_->state.plans.empty()) {
    stats.last_plan_digest = impl_->state.plans.back().plan_digest;
  }
  if (impl_->store != nullptr) {
    const StoreStats store_stats = impl_->store->stats();
    stats.store_directory = store_stats.directory;
    stats.store_format_version = store_stats.format_version;
  }
  return stats;
}

RecoveryReport PlanEngine::recovery() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->recovery;
}

Epoch PlanEngine::current_epoch() const noexcept {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.epoch;
}

Sequence PlanEngine::last_sequence() const noexcept {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.sequence;
}

} // namespace rep
