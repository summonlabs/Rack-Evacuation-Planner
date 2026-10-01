// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial and boundary tests against the public API: limits, conflicting
// facts, stale evidence, epoch fencing, idempotency, integer extremes,
// corrupted durable state, path handling, and repeated recovery.

#include "testkit.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "rep/adjacent.hpp"
#include "rep/candidate.hpp"
#include "rep/engine.hpp"
#include "rep/evidence.hpp"
#include "rep/obligation.hpp"
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

const char* const kRequestBase =
    "request key=k1 authority=requester-01 epoch=0 time=1000 lineage=- rack=rack-A "
    "isolation=depower composition_revision=7 kinds=workload";

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

// The scenario every durable test commits: one workload that leaves rack-A for
// rack-B, fully proven.
std::string complete_scenario() {
  return scenario_text(kRequestBase, standard_sources(), standard_blocks());
}

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------

std::filesystem::path fresh_directory(std::string_view name) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / std::filesystem::path(std::string(name));
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::filesystem::create_directories(directory, ignored);
  return directory;
}

void copy_tree(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code ignored;
  std::filesystem::remove_all(to, ignored);
  std::filesystem::copy(from, to, std::filesystem::copy_options::recursive, ignored);
}

std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
  std::vector<unsigned char> bytes;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return bytes;
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size > 0) {
    bytes.resize(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
  }
  return bytes;
}

bool write_bytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  if (!bytes.empty()) {
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  return stream.good();
}

bool write_text(const std::filesystem::path& path, std::string_view text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  return stream.good();
}

// ---------------------------------------------------------------------------
// Engine helpers
// ---------------------------------------------------------------------------

Result<PlanOutcome> submit_scenario(PlanEngine& engine, std::string_view text) {
  const Result<ScenarioDocument> parsed = parse_scenario(text);
  if (!parsed.ok()) {
    return parsed.error();
  }
  return engine.submit(parsed.value().request);
}

Result<std::unique_ptr<PlanEngine>> open_engine(const PlannerId& planner,
                                                const std::filesystem::path& directory) {
  EngineOptions options;
  options.planner = planner;
  options.store_directory = directory;
  options.max_plans = 32;
  return PlanEngine::open(options);
}

const PlannerId& store_planner() {
  static const PlannerId planner{std::string("planner-store")};
  return planner;
}

} // namespace

// ---------------------------------------------------------------------------
// Absurd sizes
// ---------------------------------------------------------------------------

REP_TEST(Hardening, declaration_limits_are_refused_at_the_offending_line) {
  const std::string three_obligations = scenario_text(
      kRequestBase, standard_sources(),
      {kBlockComposition, kBlockEnumeration,
       "evidence obligation_catalog stream=rack-A.cat generation=7 epoch=2\n"
       "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 protected=false "
       "depends=-\n"
       "obligation id=w2 kind=workload rack=rack-A demand=cpu_millicores:1000 protected=false "
       "depends=-\n"
       "obligation id=w3 kind=workload rack=rack-A demand=cpu_millicores:1000 protected=false "
       "depends=-\n"
       "end\n"});

  ScenarioLimits limits;
  limits.max_obligations = 2;
  const Result<ScenarioDocument> refused = parse_scenario(three_obligations, limits);
  REP_REQUIRE(!refused.ok());
  REP_CHECK(refused.error().code == ErrorCode::InvalidEncoding);
  REP_CHECK(refused.error().message.rfind("line 25 column 1: ", 0) == 0);
  REP_CHECK(refused.error().message.find("more obligations than the limit") !=
            std::string::npos);

  // Exactly at the limit the very same text parses, so the refusal is the
  // limit and nothing else.
  ScenarioLimits boundary;
  boundary.max_obligations = 3;
  const Result<ScenarioDocument> accepted = parse_scenario(three_obligations, boundary);
  REP_REQUIRE_OK(accepted);
  const EvidenceRecord* catalog = nullptr;
  for (const EvidenceRecord& record : accepted.value().request.evidence.records()) {
    if (record.kind == EvidenceKind::ObligationCatalog) {
      catalog = &record;
    }
  }
  REP_REQUIRE(catalog != nullptr);
  REP_CHECK_EQ(std::get<ObligationCatalogPayload>(catalog->payload).obligations.size(),
               std::size_t{3});

  ScenarioLimits candidate_limits;
  candidate_limits.max_candidates = 0;
  const Result<ScenarioDocument> no_candidates =
      parse_scenario(complete_scenario(), candidate_limits);
  REP_REQUIRE(!no_candidates.ok());
  REP_CHECK(no_candidates.error().code == ErrorCode::InvalidEncoding);
  REP_CHECK(no_candidates.error().message.find("more candidates than the limit") !=
            std::string::npos);
}

REP_TEST(Hardening, evidence_bundle_refuses_more_records_than_its_limit) {
  const Result<ScenarioDocument> parsed = parse_scenario(complete_scenario());
  REP_REQUIRE_OK(parsed);
  const std::vector<EvidenceRecord> records(parsed.value().request.evidence.records().begin(),
                                            parsed.value().request.evidence.records().end());
  REP_REQUIRE(records.size() > std::size_t{2});

  const Result<EvidenceBundle> too_many = EvidenceBundle::make(records, records.size() - 1);
  REP_REQUIRE(!too_many.ok());
  REP_CHECK(too_many.error().code == ErrorCode::LimitExceeded);
  REP_CHECK(too_many.error().message.find("limit is") != std::string::npos);

  const Result<EvidenceBundle> exact = EvidenceBundle::make(records, records.size());
  REP_REQUIRE_OK(exact);
  REP_CHECK_EQ(exact.value().size(), records.size());

  const Result<EvidenceBundle> none = EvidenceBundle::make(records, 0);
  REP_REQUIRE(!none.ok());
  REP_CHECK(none.error().code == ErrorCode::LimitExceeded);
}

REP_TEST(Hardening, identifiers_longer_than_the_maximum_are_refused) {
  const std::string maximum(kMaxIdentifierLength, 'a');
  const std::string over(kMaxIdentifierLength + 1, 'a');

  const auto accepted = RackId::parse(maximum);
  REP_REQUIRE_OK(accepted);
  REP_CHECK_EQ(accepted.value().value().size(), kMaxIdentifierLength);

  const auto refused = RackId::parse(over);
  REP_REQUIRE(!refused.ok());
  REP_CHECK(refused.error().code == ErrorCode::InvalidIdentifier);
  REP_CHECK(refused.error().message.find("exceeds 128 characters") != std::string::npos);

  const Result<ScenarioDocument> long_planner =
      parse_scenario("version 1\nplanner " + over + "\n" +
                     scenario_text(kRequestBase, standard_sources(), standard_blocks()));
  REP_REQUIRE(!long_planner.ok());
  REP_CHECK(long_planner.error().code == ErrorCode::InvalidEncoding);
  REP_CHECK(long_planner.error().message.find("identifier exceeds 128 characters") !=
            std::string::npos);

  const Result<ScenarioDocument> long_obligation = parse_scenario(scenario_text(
      kRequestBase, standard_sources(),
      {kBlockComposition, kBlockEnumeration,
       "evidence obligation_catalog stream=rack-A.cat generation=7 epoch=2\n"
       "obligation id=" + over +
           " kind=workload rack=rack-A demand=cpu_millicores:1000 protected=false depends=-\n"
           "end\n"}));
  REP_REQUIRE(!long_obligation.ok());
  REP_CHECK(long_obligation.error().code == ErrorCode::InvalidEncoding);
  REP_CHECK(long_obligation.error().message.find("identifier exceeds 128 characters") !=
            std::string::npos);
}

// ---------------------------------------------------------------------------
// Duplicate and conflicting facts
// ---------------------------------------------------------------------------

REP_TEST(Hardening, duplicate_evidence_sources_are_refused_by_the_request) {
  const Result<ScenarioDocument> parsed = parse_scenario(complete_scenario());
  REP_REQUIRE_OK(parsed);
  const PlanRequest& original = parsed.value().request;

  std::vector<EvidenceSource> sources = original.evidence_sources;
  REP_REQUIRE(!sources.empty());
  sources.push_back(sources.front());
  const auto made = PlanRequest::make(original.idempotency_key, original.requested_by,
                                      original.expected_epoch, original.evaluation_time,
                                      original.isolation, original.lineage, std::move(sources),
                                      original.evidence);
  REP_REQUIRE(!made.ok());
  REP_CHECK(made.error().code == ErrorCode::DuplicateIdentity);

  // The same bundle may not publish one stream twice either.
  std::vector<EvidenceRecord> records(original.evidence.records().begin(),
                                      original.evidence.records().end());
  records.push_back(records.front());
  const Result<EvidenceBundle> duplicated = EvidenceBundle::make(std::move(records));
  REP_REQUIRE(!duplicated.ok());
  REP_CHECK(duplicated.error().code == ErrorCode::AlreadyExists);
}

REP_TEST(Hardening, conflicting_capacity_streams_are_refused_not_resolved) {
  EngineOptions options;
  options.planner = PlannerId{std::string("planner-conflict")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);

  const std::string text = scenario_text(
      kRequestBase,
      {kSourceComposition, kSourceEnumeration, kSourceCatalog, kSourceCapacity,
       "evidence=capacity authority=facility-capacity-b stream=capacity-b", kSourceDomains,
       kSourceAsi, kSourceOffers},
      {kBlockComposition, kBlockEnumeration, kBlockCatalog,
       "evidence capacity stream=capacity generation=4 epoch=1\n"
       "destination destination=rack_slot:rack-B available=cpu_millicores:8000\n"
       "end\n",
       "evidence capacity stream=capacity-b generation=4 epoch=1\n"
       "destination destination=rack_slot:rack-B available=cpu_millicores:4000\n"
       "end\n",
       kBlockDomains, kBlockAsi, kBlockOffers});

  const auto parsed = parse_scenario(text);
  REP_REQUIRE_OK(parsed);
  REP_CHECK_EQ(parsed.value().request.evidence_sources.size(), std::size_t{8});

  const auto outcome = engine.value()->submit(parsed.value().request);
  REP_REQUIRE_OK(outcome);
  REP_CHECK(outcome.value().kind == OutcomeKind::RejectedInput);
  REP_CHECK(outcome.value().error.code == ErrorCode::DuplicateIdentity);
  REP_CHECK(!outcome.value().has_plan);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{0});
  REP_CHECK_EQ(engine.value()->stats().plan_count, std::size_t{0});
}

REP_TEST(Hardening, conflicting_workload_state_streams_are_refused) {
  EngineOptions options;
  options.planner = PlannerId{std::string("planner-conflict")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);

  const std::string text = scenario_text(
      kRequestBase,
      {kSourceComposition, kSourceEnumeration, kSourceCatalog, kSourceCapacity, kSourceDomains,
       kSourceAsi, "evidence=asi_workload_state authority=agent-scheduler-b stream=asi-b",
       kSourceOffers},
      {kBlockComposition, kBlockEnumeration, kBlockCatalog, kBlockCapacity, kBlockDomains,
       kBlockAsi,
       "evidence asi_workload_state stream=asi-b generation=4 epoch=1\n"
       "record obligation=w1 lifecycle=paused migration=cold_only attachment=local_state\n"
       "end\n",
       kBlockOffers});

  const auto outcome = submit_scenario(*engine.value(), text);
  REP_REQUIRE_OK(outcome);
  REP_CHECK(outcome.value().kind == OutcomeKind::RejectedInput);
  REP_CHECK(outcome.value().error.code == ErrorCode::DuplicateIdentity);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{0});
}

REP_TEST(Hardening, conflicting_candidate_streams_are_refused) {
  EngineOptions options;
  options.planner = PlannerId{std::string("planner-conflict")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);

  const std::string text = scenario_text(
      kRequestBase,
      {kSourceComposition, kSourceEnumeration, kSourceCatalog, kSourceCapacity, kSourceDomains,
       kSourceAsi, kSourceOffers,
       "evidence=candidate_offers authority=agent-scheduler-b stream=offers-b"},
      {kBlockComposition, kBlockEnumeration, kBlockCatalog, kBlockCapacity, kBlockDomains,
       kBlockAsi, kBlockOffers,
       "evidence candidate_offers stream=offers-b generation=4 epoch=1\n"
       "candidate id=c1 obligation=w1 action=live_migrate destination=rack_slot:rack-C "
       "authority=agent-scheduler-b generation=4 epoch=1 "
       "provision=cpu_millicores:1000 cost=99 window=false\n"
       "end\n"});

  const auto outcome = submit_scenario(*engine.value(), text);
  REP_REQUIRE_OK(outcome);
  REP_CHECK(outcome.value().kind == OutcomeKind::RejectedInput);
  REP_CHECK(outcome.value().error.code == ErrorCode::DuplicateIdentity);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{0});
}

REP_TEST(Hardening, conflicting_failure_domain_members_are_refused) {
  EngineOptions options;
  options.planner = PlannerId{std::string("planner-conflict")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);

  const std::string text = scenario_text(
      kRequestBase,
      {kSourceComposition, kSourceEnumeration, kSourceCatalog, kSourceCapacity, kSourceDomains,
       "evidence=failure_domain authority=facility-capacity-b stream=domains-b", kSourceAsi,
       kSourceOffers},
      {kBlockComposition, kBlockEnumeration, kBlockCatalog, kBlockCapacity, kBlockDomains,
       "evidence failure_domain stream=domains-b generation=4 epoch=1\n"
       "member kind=rack rack=rack-A domain=fd-9\n"
       "end\n",
       kBlockAsi, kBlockOffers});

  const auto outcome = submit_scenario(*engine.value(), text);
  REP_REQUIRE_OK(outcome);
  REP_CHECK(outcome.value().kind == OutcomeKind::RejectedInput);
  REP_CHECK(outcome.value().error.code == ErrorCode::PreconditionFailed);
  REP_CHECK(outcome.value().error.message.find("two domains") != std::string::npos);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{0});
}

// ---------------------------------------------------------------------------
// Stale and missing evidence
// ---------------------------------------------------------------------------

REP_TEST(Hardening, a_bound_stream_with_no_record_makes_the_plan_indeterminate) {
  const Result<ScenarioDocument> parsed = parse_scenario(complete_scenario());
  REP_REQUIRE_OK(parsed);
  const PlanRequest& original = parsed.value().request;

  std::vector<EvidenceSource> sources = original.evidence_sources;
  for (EvidenceSource& source : sources) {
    if (source.kind == EvidenceKind::RackComposition) {
      source.source.stream = StreamId{std::string("rack-A.absent")};
    }
  }
  const auto modified =
      PlanRequest::make(IdempotencyKey{std::string("k-absent-composition")},
                        original.requested_by, original.expected_epoch, original.evaluation_time,
                        original.isolation, original.lineage, std::move(sources),
                        original.evidence);
  REP_REQUIRE_OK(modified);

  EngineOptions options;
  options.planner = PlannerId{std::string("planner-stale")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);
  const auto outcome = engine.value()->submit(modified.value());
  REP_REQUIRE_OK(outcome);
  REP_CHECK(outcome.value().kind == OutcomeKind::Planned);
  REP_CHECK(outcome.value().plan.status != PlanStatus::Complete);
  REP_CHECK(outcome.value().plan.status == PlanStatus::Indeterminate);
  REP_CHECK(outcome.value().plan.assignments.empty());
  REP_CHECK(outcome.value().plan.residuals.empty());
  const std::vector<IndeterminacyReason> expected = {IndeterminacyReason::RackCompositionMissing};
  REP_CHECK(outcome.value().plan.indeterminacy_reasons == expected);
  REP_CHECK(outcome.value().plan.verdict() == SafetyVerdict::Indeterminate);
  REP_CHECK(outcome.value().plan.verify().ok());

  // The plan binds only the streams that were actually present.
  for (const PlanBinding& binding : outcome.value().plan.bindings) {
    REP_CHECK(binding.stream.stream.view() != std::string_view("rack-A.absent"));
  }
  // The evidence it did consume is still auditable against the request.
  REP_CHECK(audit_plan(outcome.value().plan, modified.value()).empty());
}

REP_TEST(Hardening, an_enumeration_for_another_revision_is_indeterminate) {
  const std::string text = scenario_text(
      "request key=k-rev authority=requester-01 epoch=0 time=1000 lineage=- rack=rack-A "
      "isolation=depower composition_revision=8 kinds=workload",
      standard_sources(), standard_blocks());
  EngineOptions options;
  options.planner = PlannerId{std::string("planner-stale")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);
  const auto outcome = submit_scenario(*engine.value(), text);
  REP_REQUIRE_OK(outcome);
  REP_CHECK(outcome.value().kind == OutcomeKind::Planned);
  REP_CHECK(outcome.value().plan.status == PlanStatus::Indeterminate);
  const std::vector<IndeterminacyReason> expected = {
      IndeterminacyReason::RackCompositionRevisionMismatch};
  REP_CHECK(outcome.value().plan.indeterminacy_reasons == expected);
}

REP_TEST(Hardening, a_plan_whose_evidence_moved_is_stale) {
  const Result<ScenarioDocument> parsed = parse_scenario(complete_scenario());
  REP_REQUIRE_OK(parsed);
  EngineOptions options;
  options.planner = PlannerId{std::string("planner-stale")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);
  const auto outcome = engine.value()->submit(parsed.value().request);
  REP_REQUIRE_OK(outcome);
  REP_REQUIRE(outcome.value().kind == OutcomeKind::Planned);
  const Plan plan = outcome.value().plan;

  // Unchanged evidence: the plan still proves something.
  const auto fresh = engine.value()->assess(plan.id.view(), parsed.value().request.evidence);
  REP_REQUIRE_OK(fresh);
  REP_CHECK(fresh.value().verdict == SafetyVerdict::Safe);
  REP_CHECK(fresh.value().staleness.empty());
  REP_CHECK(!fresh.value().any_stream_missing);
  REP_CHECK(fresh.value().same_incarnation);

  // The composition authority republished: same payload, newer generation.
  std::vector<EvidenceRecord> records(parsed.value().request.evidence.records().begin(),
                                      parsed.value().request.evidence.records().end());
  for (EvidenceRecord& record : records) {
    if (record.kind == EvidenceKind::RackComposition) {
      record.stamp.generation = Generation{8};
    }
  }
  const Result<EvidenceBundle> moved = EvidenceBundle::make(std::move(records));
  REP_REQUIRE_OK(moved);

  bool any_missing = false;
  const std::vector<StalenessFinding> findings = plan.staleness(moved.value(), any_missing);
  REP_CHECK(!any_missing);
  REP_CHECK_EQ(findings.size(), std::size_t{1});
  if (!findings.empty()) {
    REP_CHECK(findings[0].kind == StalenessKind::GenerationMoved);
    REP_CHECK_EQ(findings[0].plan_generation.value(), std::uint64_t{7});
    REP_CHECK_EQ(findings[0].current_generation.value(), std::uint64_t{8});
  }
  REP_CHECK(plan.verdict(findings) == SafetyVerdict::Stale);

  const auto assessment = engine.value()->assess(plan.id.view(), moved.value());
  REP_REQUIRE_OK(assessment);
  REP_CHECK(assessment.value().verdict == SafetyVerdict::Stale);
  REP_CHECK(assessment.value().same_incarnation);
  REP_CHECK(!assessment.value().superseded);
  REP_CHECK(!assessment.value().any_stream_missing);

  // An empty bundle is worse: the bound stream is gone entirely.
  const EvidenceBundle empty;
  const auto absent = engine.value()->assess(plan.id.view(), empty);
  REP_REQUIRE_OK(absent);
  REP_CHECK(absent.value().verdict == SafetyVerdict::Stale);
  REP_CHECK(absent.value().any_stream_missing);
  REP_CHECK_EQ(absent.value().staleness.size(), plan.bindings.size());
}

// ---------------------------------------------------------------------------
// Epoch fencing
// ---------------------------------------------------------------------------

REP_TEST(Hardening, a_wrong_expected_epoch_is_refused_and_commits_nothing) {
  EngineOptions options;
  options.planner = PlannerId{std::string("planner-epoch")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);
  const Epoch current = engine.value()->current_epoch();
  REP_CHECK_EQ(current.value(), std::uint64_t{1});

  const Result<ScenarioDocument> parsed = parse_scenario(complete_scenario());
  REP_REQUIRE_OK(parsed);
  const PlanRequest& original = parsed.value().request;

  const auto wrong = PlanRequest::make(
      IdempotencyKey{std::string("k-wrong-epoch")}, original.requested_by, Epoch{current.value() + 5},
      original.evaluation_time, original.isolation, original.lineage, original.evidence_sources,
      original.evidence);
  REP_REQUIRE_OK(wrong);
  const auto refused = engine.value()->submit(wrong.value());
  REP_REQUIRE_OK(refused);
  REP_CHECK(refused.value().kind == OutcomeKind::RejectedStaleEpoch);
  REP_CHECK(refused.value().error.code == ErrorCode::StaleEpoch);
  REP_CHECK(!refused.value().has_plan);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{0});
  REP_CHECK_EQ(engine.value()->stats().plan_count, std::size_t{0});
  const auto not_committed =
      engine.value()->plan_by_idempotency_key("k-wrong-epoch");
  REP_CHECK(!not_committed.ok());
  REP_CHECK(not_committed.error().code == ErrorCode::NotFound);

  const auto right = PlanRequest::make(
      IdempotencyKey{std::string("k-right-epoch")}, original.requested_by, current,
      original.evaluation_time, original.isolation, original.lineage, original.evidence_sources,
      original.evidence);
  REP_REQUIRE_OK(right);
  const auto committed = engine.value()->submit(right.value());
  REP_REQUIRE_OK(committed);
  REP_CHECK(committed.value().kind == OutcomeKind::Planned);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{1});
  REP_CHECK_EQ(committed.value().plan.epoch.value(), current.value());
}

REP_TEST(Hardening, a_reopened_store_rejects_the_previous_incarnation) {
  const std::filesystem::path root = fresh_directory("rep-hardening-epoch");
  const Result<ScenarioDocument> parsed = parse_scenario(complete_scenario());
  REP_REQUIRE_OK(parsed);

  Epoch first_epoch;
  {
    const auto engine = open_engine(store_planner(), root);
    REP_REQUIRE_OK(engine);
    first_epoch = engine.value()->current_epoch();
    const auto outcome = engine.value()->submit(parsed.value().request);
    REP_REQUIRE_OK(outcome);
    REP_CHECK(outcome.value().kind == OutcomeKind::Planned);
    REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{1});
  }
  {
    const auto engine = open_engine(store_planner(), root);
    REP_REQUIRE_OK(engine);
    const Epoch second_epoch = engine.value()->current_epoch();
    REP_CHECK(second_epoch.value() > first_epoch.value());
    REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{1});
    REP_CHECK(engine.value()->recovery().recovered);
    REP_CHECK_EQ(engine.value()->recovery().recovered_plan_count, std::size_t{1});

    const PlanRequest& original = parsed.value().request;
    const auto stale = PlanRequest::make(
        IdempotencyKey{std::string("k-second")}, original.requested_by, first_epoch,
        UnixNanos{original.evaluation_time.value() + 1}, original.isolation, original.lineage,
        original.evidence_sources, original.evidence);
    REP_REQUIRE_OK(stale);
    const auto refused = engine.value()->submit(stale.value());
    REP_REQUIRE_OK(refused);
    REP_CHECK(refused.value().kind == OutcomeKind::RejectedStaleEpoch);
    REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{1});

    const auto current = PlanRequest::make(
        IdempotencyKey{std::string("k-second")}, original.requested_by, second_epoch,
        UnixNanos{original.evaluation_time.value() + 1}, original.isolation, original.lineage,
        original.evidence_sources, original.evidence);
    REP_REQUIRE_OK(current);
    const auto accepted = engine.value()->submit(current.value());
    REP_REQUIRE_OK(accepted);
    REP_CHECK(accepted.value().kind == OutcomeKind::Planned);
    REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{2});
  }
}

// ---------------------------------------------------------------------------
// Idempotency
// ---------------------------------------------------------------------------

REP_TEST(Hardening, a_repeated_request_replays_and_a_changed_one_conflicts) {
  EngineOptions options;
  options.planner = PlannerId{std::string("planner-idem")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);

  const std::string text = complete_scenario();
  const auto first = submit_scenario(*engine.value(), text);
  REP_REQUIRE_OK(first);
  REP_REQUIRE(first.value().kind == OutcomeKind::Planned);
  const Plan committed = first.value().plan;

  const auto replay = submit_scenario(*engine.value(), text);
  REP_REQUIRE_OK(replay);
  REP_CHECK(replay.value().kind == OutcomeKind::Replayed);
  REP_CHECK(replay.value().has_plan);
  REP_CHECK(replay.value().plan.id == committed.id);
  REP_CHECK(replay.value().plan.plan_digest == committed.plan_digest);
  REP_CHECK(replay.value().plan.sequence == committed.sequence);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{1});
  REP_CHECK_EQ(engine.value()->stats().plan_count, std::size_t{1});
  REP_CHECK_EQ(engine.value()->stats().idempotency_count, std::size_t{1});

  const auto from_key = engine.value()->plan_by_idempotency_key("k1");
  REP_REQUIRE_OK(from_key);
  REP_CHECK(from_key.value().plan_digest == committed.plan_digest);

  // The same key with a different request is a conflict, and commits nothing.
  std::string changed = text;
  const std::string needle = "time=1000";
  const std::size_t position = changed.find(needle);
  REP_REQUIRE(position != std::string::npos);
  changed.replace(position, needle.size(), "time=2000");
  const auto conflict = submit_scenario(*engine.value(), changed);
  REP_REQUIRE_OK(conflict);
  REP_CHECK(conflict.value().kind == OutcomeKind::RejectedIdempotencyConflict);
  REP_CHECK(conflict.value().error.code == ErrorCode::IdempotencyConflict);
  REP_CHECK(!conflict.value().has_plan);
  REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{1});
  REP_CHECK_EQ(engine.value()->stats().plan_count, std::size_t{1});
}

// ---------------------------------------------------------------------------
// Integer extremes
// ---------------------------------------------------------------------------

REP_TEST(Hardening, resource_sums_overflow_rather_than_wrap) {
  const auto huge = ResourceVector::parse("cpu_millicores:18446744073709551615");
  REP_REQUIRE_OK(huge);
  REP_CHECK_EQ(huge.value().get(ResourceClass::CpuMillicores),
               std::uint64_t{18446744073709551615ull});

  const auto one = ResourceVector::parse("cpu_millicores:1");
  REP_REQUIRE_OK(one);
  const auto sum = huge.value().add(one.value());
  REP_REQUIRE(!sum.ok());
  REP_CHECK(sum.error().code == ErrorCode::Overflow);

  // A different class does not overflow: the sum is per class.
  const auto memory = ResourceVector::parse("memory_bytes:1");
  REP_REQUIRE_OK(memory);
  const auto mixed = huge.value().add(memory.value());
  REP_REQUIRE_OK(mixed);
  REP_CHECK_EQ(mixed.value().get(ResourceClass::MemoryBytes), std::uint64_t{1});
  REP_CHECK_EQ(mixed.value().get(ResourceClass::CpuMillicores),
               std::uint64_t{18446744073709551615ull});

  const std::string maximum = "18446744073709551615";
  const std::string text = scenario_text(
      "request key=k-max authority=requester-01 epoch=0 time=9223372036854775807 lineage=- "
      "rack=rack-A isolation=depower composition_revision=" + maximum + " kinds=workload",
      standard_sources(),
      {"evidence rack_composition stream=rack-A.comp generation=" + maximum + " epoch=" + maximum +
           "\nrack rack-A\nrevision " + maximum + "\noccupants w1,w2\nend\n",
       "evidence enumeration stream=rack-A.enum generation=" + maximum + " epoch=" + maximum +
           "\nrack rack-A\nrevision " + maximum + "\ncomplete true\nenumerated w1,w2\nend\n",
       "evidence obligation_catalog stream=rack-A.cat generation=" + maximum + " epoch=" +
           maximum +
           "\nobligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:" + maximum +
           " protected=false depends=-\n"
           "obligation id=w2 kind=workload rack=rack-A demand=cpu_millicores:1 protected=false "
           "depends=-\nend\n",
       "evidence capacity stream=capacity generation=" + maximum + " epoch=" + maximum +
           "\ndestination destination=rack_slot:rack-B available=cpu_millicores:" + maximum +
           "\nend\n",
       "evidence failure_domain stream=domains generation=" + maximum + " epoch=" + maximum +
           "\nmember kind=rack rack=rack-A domain=fd-1\n"
           "member kind=destination destination=rack_slot:rack-B domain=fd-2\nend\n",
       "evidence asi_workload_state stream=asi generation=" + maximum + " epoch=" + maximum +
           "\nrecord obligation=w1 lifecycle=running migration=live_allowed "
           "attachment=stateless\n"
           "record obligation=w2 lifecycle=running migration=live_allowed "
           "attachment=stateless\nend\n",
       "evidence candidate_offers stream=offers generation=" + maximum + " epoch=" + maximum +
           "\ncandidate id=c2 obligation=w2 action=live_migrate destination=rack_slot:rack-B "
           "authority=agent-scheduler generation=" + maximum + " epoch=" + maximum +
           " provision=cpu_millicores:1 cost=4294967295 window=false\n"
           "candidate id=c1 obligation=w1 action=live_migrate destination=rack_slot:rack-B "
           "authority=agent-scheduler generation=" + maximum + " epoch=" + maximum +
           " provision=cpu_millicores:" + maximum + " cost=4294967295 window=false\nend\n"});

  const Result<ScenarioDocument> parsed = parse_scenario(text);
  REP_REQUIRE_OK(parsed);
  REP_CHECK_EQ(parsed.value().request.evaluation_time.value(),
               std::int64_t{9223372036854775807ll});
  REP_CHECK_EQ(parsed.value().request.isolation.composition_revision.value(),
               std::uint64_t{18446744073709551615ull});

  EngineOptions options;
  options.planner = PlannerId{std::string("planner-extremes")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);
  const auto outcome = engine.value()->submit(parsed.value().request);
  REP_REQUIRE_OK(outcome);
  REP_REQUIRE(outcome.value().kind == OutcomeKind::Planned);
  const Plan& plan = outcome.value().plan;

  // The first obligation consumes every last unit; the second cannot be served
  // by a wrapped-around remainder, so it stays residual.
  REP_CHECK(plan.status == PlanStatus::Partial);
  REP_CHECK_EQ(plan.assignments.size(), std::size_t{1});
  REP_REQUIRE(!plan.assignments.empty());
  REP_CHECK_EQ(plan.assignments[0].obligation.str(), std::string("w1"));
  REP_CHECK_EQ(plan.assignments[0].candidate.str(), std::string("c1"));
  REP_CHECK_EQ(plan.residuals.size(), std::size_t{1});
  REP_REQUIRE(!plan.residuals.empty());
  REP_CHECK_EQ(plan.residuals[0].obligation.str(), std::string("w2"));
  REP_CHECK(plan.residuals[0].reason == ResidualReason::CapacityExhausted);
  REP_CHECK(plan.verify().ok());
  REP_CHECK(plan.verdict() == SafetyVerdict::NotProven);
  REP_CHECK(audit_plan(plan, parsed.value().request).empty());
  for (const PlanBinding& binding : plan.bindings) {
    REP_CHECK_EQ(binding.generation.value(), std::uint64_t{18446744073709551615ull});
    REP_CHECK_EQ(binding.epoch.value(), std::uint64_t{18446744073709551615ull});
  }
}

REP_TEST(Hardening, the_cheapest_candidate_wins_and_a_short_provision_is_rejected) {
  const std::string base = scenario_text(
      kRequestBase, standard_sources(),
      {kBlockComposition, kBlockEnumeration, kBlockCatalog, kBlockCapacity, kBlockDomains,
       kBlockAsi,
       "evidence candidate_offers stream=offers generation=4 epoch=1\n"
       "candidate id=c-expensive obligation=w1 action=live_migrate "
       "destination=rack_slot:rack-B authority=agent-scheduler generation=4 epoch=1 "
       "provision=cpu_millicores:1000 cost=4294967295 window=false\n"
       "candidate id=c-cheap obligation=w1 action=live_migrate destination=rack_slot:rack-B "
       "authority=agent-scheduler generation=4 epoch=1 provision=cpu_millicores:1000 cost=1 "
       "window=false\n"
       "end\n"});
  EngineOptions options;
  options.planner = PlannerId{std::string("planner-cost")};
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);
  const auto outcome = submit_scenario(*engine.value(), base);
  REP_REQUIRE_OK(outcome);
  REP_REQUIRE(outcome.value().kind == OutcomeKind::Planned);
  REP_CHECK(outcome.value().plan.status == PlanStatus::Complete);
  REP_REQUIRE(!outcome.value().plan.assignments.empty());
  REP_CHECK_EQ(outcome.value().plan.assignments[0].candidate.str(), std::string("c-cheap"));

  const std::string short_provision = scenario_text(
      "request key=k-short authority=requester-01 epoch=0 time=1000 lineage=- rack=rack-A "
      "isolation=depower composition_revision=7 kinds=workload",
      standard_sources(),
      {kBlockComposition, kBlockEnumeration, kBlockCatalog, kBlockCapacity, kBlockDomains,
       kBlockAsi,
       "evidence candidate_offers stream=offers generation=4 epoch=1\n"
       "candidate id=c-small obligation=w1 action=live_migrate destination=rack_slot:rack-B "
       "authority=agent-scheduler generation=4 epoch=1 provision=cpu_millicores:999 cost=1 "
       "window=false\n"
       "end\n"});
  const auto short_outcome = submit_scenario(*engine.value(), short_provision);
  REP_REQUIRE_OK(short_outcome);
  REP_REQUIRE(short_outcome.value().kind == OutcomeKind::Planned);
  const Plan& plan = short_outcome.value().plan;
  REP_CHECK(plan.status != PlanStatus::Complete);
  REP_CHECK(plan.status == PlanStatus::Partial);
  REP_CHECK(plan.assignments.empty());
  REP_CHECK_EQ(plan.residuals.size(), std::size_t{1});
  REP_REQUIRE(!plan.residuals.empty());
  REP_CHECK(plan.residuals[0].reason == ResidualReason::CandidatesRejected);
  REP_CHECK_EQ(plan.residuals[0].rejected.size(), std::size_t{1});
  REP_REQUIRE(!plan.residuals[0].rejected.empty());
  REP_CHECK(plan.residuals[0].rejected[0].reason == RejectionReason::CapacityClaimMismatch);
  REP_CHECK(plan.verify().ok());
}

// ---------------------------------------------------------------------------
// Malformed durable state
// ---------------------------------------------------------------------------

REP_TEST(Hardening, malformed_store_state_is_never_loaded) {
  const std::filesystem::path root = fresh_directory("rep-hardening-store");
  const std::string text = complete_scenario();
  PlanId committed_id;
  {
    const auto engine = open_engine(store_planner(), root);
    REP_REQUIRE_OK(engine);
    const auto outcome = submit_scenario(*engine.value(), text);
    REP_REQUIRE_OK(outcome);
    REP_REQUIRE(outcome.value().kind == OutcomeKind::Planned);
    committed_id = outcome.value().plan.id;
  }

  const auto stats = PlanStore::inspect(root);
  REP_REQUIRE_OK(stats);
  REP_CHECK_EQ(stats.value().plan_count, std::size_t{1});
  REP_CHECK_EQ(stats.value().sequence.value(), std::uint64_t{1});
  const std::filesystem::path state_path = root / stats.value().state_file;
  const std::vector<unsigned char> good = read_bytes(state_path);
  REP_REQUIRE(good.size() > std::size_t{136});
  const std::filesystem::path current_path = root / std::string(kStoreCurrentFileName);
  const std::vector<unsigned char> good_current = read_bytes(current_path);
  REP_REQUIRE(good_current.size() > std::size_t{96});

  // (a) Every strict prefix of the state file fails to load.
  std::size_t prefixes_refused = 0;
  bool prefix_failed = false;
  for (std::size_t length = 0; length < good.size(); ++length) {
    const std::vector<unsigned char> prefix(
        good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length));
    if (!write_bytes(state_path, prefix)) {
      REP_FAIL("could not write a truncated state file");
      prefix_failed = true;
      break;
    }
    const auto inspected = PlanStore::inspect(root);
    if (inspected.ok()) {
      REP_FAIL("a " + std::to_string(length) + "-byte prefix of the state file loaded");
      prefix_failed = true;
      break;
    }
    ++prefixes_refused;
  }
  if (!prefix_failed) {
    REP_CHECK_EQ(prefixes_refused, good.size());
  }
  REP_REQUIRE(write_bytes(state_path, good));
  REP_CHECK(PlanStore::inspect(root).ok());

  // (b) A single flipped byte anywhere - header, payload, or trailer - is
  // refused.  The offsets cover every distinct header field, the first, middle
  // and last payload byte, and both ends of the trailer.
  const std::vector<std::size_t> offsets = {0,
                                            7,
                                            8,
                                            11,
                                            12,
                                            15,
                                            16,
                                            23,
                                            24,
                                            31,
                                            32,
                                            39,
                                            40,
                                            71,
                                            72,
                                            103,
                                            104,
                                            good.size() / 2,
                                            good.size() - 33,
                                            good.size() - 32,
                                            good.size() - 1};
  for (const std::size_t offset : offsets) {
    std::vector<unsigned char> damaged = good;
    damaged[offset] = static_cast<unsigned char>(damaged[offset] ^ 0xffu);
    REP_REQUIRE(write_bytes(state_path, damaged));
    const auto inspected = PlanStore::inspect(root);
    REP_CHECK_MSG(!inspected.ok(),
                  "a flipped byte at offset " + std::to_string(offset) + " was accepted");
  }
  REP_REQUIRE(write_bytes(state_path, good));
  REP_CHECK(PlanStore::inspect(root).ok());

  // The same mutations are refused by a whole engine opening a fresh copy of
  // the store, not merely by the read-only inspector.
  const std::vector<std::size_t> engine_offsets = {0, 40, 104, good.size() - 1};
  for (const std::size_t offset : engine_offsets) {
    const std::filesystem::path copy =
        root.parent_path() / ("rep-hardening-store-flip-" + std::to_string(offset));
    copy_tree(root, copy);
    std::vector<unsigned char> damaged = good;
    damaged[offset] = static_cast<unsigned char>(damaged[offset] ^ 0xffu);
    REP_REQUIRE(write_bytes(copy / stats.value().state_file, damaged));
    const auto engine = open_engine(store_planner(), copy);
    REP_CHECK_MSG(!engine.ok(), "an engine opened a store whose state file was damaged at " +
                                    std::to_string(offset));
  }

  // Truncated state files are refused by a whole engine too.
  for (const std::size_t length : {std::size_t{0}, std::size_t{8}, std::size_t{64},
                                   std::size_t{120}, good.size() - 1}) {
    const std::filesystem::path copy = root.parent_path() /
                                       ("rep-hardening-store-trunc-" + std::to_string(length));
    copy_tree(root, copy);
    const std::vector<unsigned char> prefix(
        good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length));
    REP_REQUIRE(write_bytes(copy / stats.value().state_file, prefix));
    const auto engine = open_engine(store_planner(), copy);
    REP_CHECK_MSG(!engine.ok(), "an engine opened a " + std::to_string(length) +
                                    "-byte prefix of the state file");
  }

  // (c) Trailing bytes make the declared length disagree with the file.
  for (const std::size_t extra : {std::size_t{1}, std::size_t{4}}) {
    std::vector<unsigned char> appended = good;
    appended.insert(appended.end(), extra, static_cast<unsigned char>(0x5a));
    REP_REQUIRE(write_bytes(state_path, appended));
    REP_CHECK_MSG(!PlanStore::inspect(root).ok(),
                  "a state file with " + std::to_string(extra) +
                      " trailing bytes was accepted");
  }
  REP_REQUIRE(write_bytes(state_path, good));
  REP_CHECK(PlanStore::inspect(root).ok());

  // (d) The commit point itself: every byte of CURRENT matters.
  const std::vector<std::size_t> current_offsets = {0,  8,  12, 16, 24,
                                                    32, 64, 95, good_current.size() - 1};
  for (const std::size_t offset : current_offsets) {
    std::vector<unsigned char> damaged = good_current;
    damaged[offset] = static_cast<unsigned char>(damaged[offset] ^ 0xffu);
    REP_REQUIRE(write_bytes(current_path, damaged));
    const auto inspected = PlanStore::inspect(root);
    REP_CHECK_MSG(!inspected.ok(),
                  "a CURRENT pointer damaged at offset " + std::to_string(offset) +
                      " was accepted");
  }
  {
    const std::vector<unsigned char> truncated(good_current.begin(), good_current.begin() + 40);
    REP_REQUIRE(write_bytes(current_path, truncated));
    REP_CHECK(!PlanStore::inspect(root).ok());
    std::vector<unsigned char> appended = good_current;
    appended.push_back(static_cast<unsigned char>(0x00));
    REP_REQUIRE(write_bytes(current_path, appended));
    REP_CHECK(!PlanStore::inspect(root).ok());
  }
  REP_REQUIRE(write_bytes(current_path, good_current));
  REP_CHECK(PlanStore::inspect(root).ok());

  // A whole engine refuses the same damage, not merely the read-only inspector.
  const std::filesystem::path engine_copy = root.parent_path() / "rep-hardening-store-current";
  copy_tree(root, engine_copy);
  {
    std::vector<unsigned char> damaged = good_current;
    damaged[32] = static_cast<unsigned char>(damaged[32] ^ 0xffu);
    REP_REQUIRE(write_bytes(engine_copy / std::string(kStoreCurrentFileName), damaged));
    const auto engine = open_engine(store_planner(), engine_copy);
    REP_CHECK(!engine.ok());
  }

  // The undamaged store still opens, still holds the plan, and still verifies.
  const auto inspected = PlanStore::inspect(root);
  REP_REQUIRE_OK(inspected);
  REP_CHECK_EQ(inspected.value().plan_count, std::size_t{1});
  {
    const auto engine = open_engine(store_planner(), root);
    REP_REQUIRE_OK(engine);
    const auto loaded = engine.value()->plan_by_id(committed_id.view());
    REP_REQUIRE_OK(loaded);
    REP_CHECK(loaded.value().verify().ok());
    REP_CHECK(loaded.value().status == PlanStatus::Complete);
  }
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

REP_TEST(Hardening, store_paths_with_spaces_and_long_paths_work) {
  const std::filesystem::path spaced = fresh_directory("rep hardening store with spaces");
  REP_REQUIRE(spaced.string().find(' ') != std::string::npos);
  const std::string text = complete_scenario();
  {
    const auto engine = open_engine(store_planner(), spaced);
    REP_REQUIRE_OK(engine);
    const auto outcome = submit_scenario(*engine.value(), text);
    REP_REQUIRE_OK(outcome);
    REP_CHECK(outcome.value().kind == OutcomeKind::Planned);
    REP_CHECK_EQ(engine.value()->stats().store_directory, spaced.string());
  }
  {
    const auto engine = open_engine(store_planner(), spaced);
    REP_REQUIRE_OK(engine);
    REP_CHECK_EQ(engine.value()->stats().plan_count, std::size_t{1});
    REP_CHECK_EQ(engine.value()->recovery().recovered_plan_count, std::size_t{1});
  }

  const std::filesystem::path base = fresh_directory("rep-hardening-long");
  std::filesystem::path nested = base;
  for (int index = 0; index < 6; ++index) {
    nested /= "segment-" + std::to_string(index) + "-0123456789";
  }
  REP_CHECK(nested.string().size() > std::size_t{150});
  {
    const auto engine = open_engine(store_planner(), nested);
    REP_REQUIRE_OK(engine);
    const auto outcome = submit_scenario(*engine.value(), text);
    REP_REQUIRE_OK(outcome);
    REP_CHECK(outcome.value().kind == OutcomeKind::Planned);
  }
  {
    const auto engine = open_engine(store_planner(), nested);
    REP_REQUIRE_OK(engine);
    REP_CHECK_EQ(engine.value()->stats().plan_count, std::size_t{1});
  }
}

REP_TEST(Hardening, a_store_on_a_regular_file_is_not_a_directory) {
  const std::filesystem::path root = fresh_directory("rep-hardening-notadir");
  const std::filesystem::path file = root / "regular-file.bin";
  const std::string marker = "this is a file, not a store directory\n";
  REP_REQUIRE(write_text(file, marker));
  REP_REQUIRE(std::filesystem::is_regular_file(file));

  PlanStoreOptions store_options;
  store_options.directory = file;
  store_options.create_if_missing = true;
  const auto store = PlanStore::open(store_options);
  REP_REQUIRE(!store.ok());
  REP_CHECK(store.error().code == ErrorCode::NotADirectory);

  store_options.create_if_missing = false;
  const auto closed = PlanStore::open(store_options);
  REP_REQUIRE(!closed.ok());
  REP_CHECK(closed.error().code == ErrorCode::NotADirectory);

  const auto engine = open_engine(store_planner(), file);
  REP_REQUIRE(!engine.ok());
  REP_CHECK(engine.error().code == ErrorCode::NotADirectory);

  // None of the refused opens touched the file it was pointed at.
  const std::vector<unsigned char> afterwards = read_bytes(file);
  REP_CHECK_EQ(afterwards.size(), marker.size());
}

// ---------------------------------------------------------------------------
// Repeated lifecycle
// ---------------------------------------------------------------------------

REP_TEST(Hardening, repeated_reopen_advances_the_epoch_and_keeps_the_plan) {
  const std::filesystem::path root = fresh_directory("rep-hardening-lifecycle");
  const std::string text = complete_scenario();
  PlanId committed_id;
  Digest committed_digest;
  std::uint64_t previous_epoch = 0;
  constexpr int kRounds = 8;
  for (int round = 0; round < kRounds; ++round) {
    const auto engine = open_engine(store_planner(), root);
    if (!engine.ok()) {
      REP_FAIL("round " + std::to_string(round) + ": open failed: " + engine.error().message);
      return;
    }
    REP_CHECK(engine.value()->stats().durable);
    const Epoch epoch = engine.value()->current_epoch();
    REP_CHECK_MSG(epoch.value() > previous_epoch,
                  "round " + std::to_string(round) + ": epoch did not advance");
    previous_epoch = epoch.value();
    if (round == 0) {
      REP_CHECK(!engine.value()->recovery().recovered);
      REP_CHECK_EQ(engine.value()->recovery().previous_epoch.value(), std::uint64_t{0});
      const auto outcome = submit_scenario(*engine.value(), text);
      REP_REQUIRE_OK(outcome);
      if (outcome.value().kind != OutcomeKind::Planned) {
        REP_FAIL("the first round did not commit a plan");
        return;
      }
      committed_id = outcome.value().plan.id;
      committed_digest = outcome.value().plan.plan_digest;
    } else {
      REP_CHECK(engine.value()->recovery().recovered);
      REP_CHECK_EQ(engine.value()->recovery().previous_epoch.value(), previous_epoch - 1);
      REP_CHECK_EQ(engine.value()->recovery().recovered_plan_count, std::size_t{1});
      REP_CHECK_EQ(engine.value()->recovery().recovered_idempotency_count, std::size_t{1});
      REP_CHECK_EQ(engine.value()->last_sequence().value(), std::uint64_t{1});
      const auto loaded = engine.value()->plan_by_id(committed_id.view());
      if (!loaded.ok()) {
        REP_FAIL("round " + std::to_string(round) + ": the committed plan is gone: " +
                 loaded.error().message);
        return;
      }
      REP_CHECK_EQ(loaded.value().plan_digest, committed_digest);
      REP_CHECK(loaded.value().verify().ok());
      REP_CHECK(loaded.value().status == PlanStatus::Complete);
      const std::vector<PlanSummary> history = engine.value()->history();
      REP_REQUIRE(history.size() == std::size_t{1});
      REP_CHECK_EQ(history[0].plan_digest, committed_digest);
    }
  }
  REP_CHECK_EQ(previous_epoch, std::uint64_t{kRounds});
}

REP_TEST_MAIN()
