// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The canonical scenario text format: acceptance, canonical rendering, round
// trips, exact diagnostics, byte-level strictness, and parser limits.

#include "testkit.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "rep/adjacent.hpp"
#include "rep/candidate.hpp"
#include "rep/engine.hpp"
#include "rep/evidence.hpp"
#include "rep/obligation.hpp"
#include "rep/plan.hpp"
#include "rep/policy.hpp"
#include "rep/request.hpp"
#include "rep/scenario.hpp"
#include "rep/status.hpp"
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
// Scenario text fixtures
// ---------------------------------------------------------------------------

// The smallest document the format admits: header, request, one source, one
// evidence block, one occupant-free rack composition.
const char* const kMinimalScenario =
    "version 1\n"
    "planner planner-min\n"
    "request key=k-min authority=requester-min epoch=0 time=0 lineage=- rack=rack-A "
    "isolation=depower composition_revision=1 kinds=workload\n"
    "source evidence=rack_composition authority=rack-authority stream=rack-A.comp\n"
    "evidence rack_composition stream=rack-A.comp generation=1 epoch=1\n"
    "rack rack-A\n"
    "revision 1\n"
    "occupants -\n"
    "end\n";

// The complete example: every binding needed to prove a whole evacuation,
// and a single workload that can leave rack-A for rack-B.
const char* const kFullScenario =
    "version 1\n"
    "planner planner-01\n"
    "request key=k1 authority=requester-01 epoch=0 time=1000 lineage=- rack=rack-A "
    "isolation=depower composition_revision=7 kinds=workload\n"
    "source evidence=rack_composition authority=rack-authority stream=rack-A.comp\n"
    "source evidence=enumeration authority=rack-authority stream=rack-A.enum\n"
    "source evidence=obligation_catalog authority=rack-authority stream=rack-A.cat\n"
    "source evidence=capacity authority=facility-capacity stream=capacity\n"
    "source evidence=failure_domain authority=facility-capacity stream=domains\n"
    "source evidence=asi_workload_state authority=agent-scheduler stream=asi\n"
    "source evidence=candidate_offers authority=agent-scheduler stream=offers\n"
    "evidence rack_composition stream=rack-A.comp generation=7 epoch=2\n"
    "rack rack-A\n"
    "revision 7\n"
    "occupants w1\n"
    "end\n"
    "evidence enumeration stream=rack-A.enum generation=7 epoch=2\n"
    "rack rack-A\n"
    "revision 7\n"
    "complete true\n"
    "enumerated w1\n"
    "end\n"
    "evidence obligation_catalog stream=rack-A.cat generation=7 epoch=2\n"
    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 protected=false "
    "depends=-\n"
    "end\n"
    "evidence capacity stream=capacity generation=4 epoch=1\n"
    "destination destination=rack_slot:rack-B available=cpu_millicores:8000\n"
    "end\n"
    "evidence failure_domain stream=domains generation=4 epoch=1\n"
    "member kind=rack rack=rack-A domain=fd-1\n"
    "member kind=destination destination=rack_slot:rack-B domain=fd-2\n"
    "end\n"
    "evidence asi_workload_state stream=asi generation=4 epoch=1\n"
    "record obligation=w1 lifecycle=running migration=live_allowed attachment=stateless\n"
    "end\n"
    "evidence candidate_offers stream=offers generation=4 epoch=1\n"
    "candidate id=c1 obligation=w1 action=live_migrate destination=rack_slot:rack-B "
    "authority=agent-scheduler generation=4 epoch=1 provision=cpu_millicores:1000 cost=10 "
    "window=false\n"
    "end\n";

// A document whose rack is proven empty for the requested revision.
const char* const kEmptyScenario =
    "version 1\n"
    "planner planner-03\n"
    "request key=k3 authority=requester-03 epoch=0 time=-1 lineage=lin-1 rack=rack-A "
    "isolation=network_isolation composition_revision=1 kinds=network_path,service_endpoint\n"
    "source evidence=rack_composition authority=rack-authority stream=rack-A.comp\n"
    "source evidence=enumeration authority=rack-authority stream=rack-A.enum\n"
    "evidence rack_composition stream=rack-A.comp generation=1 epoch=1\n"
    "rack rack-A\n"
    "revision 1\n"
    "occupants -\n"
    "end\n"
    "evidence enumeration stream=rack-A.enum generation=1 epoch=1\n"
    "rack rack-A\n"
    "revision 1\n"
    "complete true\n"
    "enumerated -\n"
    "end\n";

// Several evacuate_kinds, a dependency between two occupants, and both of the
// kinds whose adjacent state comes from the data fabric authority.
const char* const kMultiKindScenario =
    "version 1\n"
    "planner planner-02\n"
    "request key=k2 authority=requester-02 epoch=0 time=2500 lineage=- rack=rack-A "
    "isolation=physical_service composition_revision=3 kinds=storage_replica,workload\n"
    "source evidence=rack_composition authority=rack-authority stream=rack-A.comp\n"
    "source evidence=enumeration authority=rack-authority stream=rack-A.enum\n"
    "source evidence=obligation_catalog authority=rack-authority stream=rack-A.cat\n"
    "evidence rack_composition stream=rack-A.comp generation=3 epoch=1\n"
    "rack rack-A\n"
    "revision 3\n"
    "occupants net1,w1\n"
    "end\n"
    "evidence enumeration stream=rack-A.enum generation=3 epoch=1\n"
    "rack rack-A\n"
    "revision 3\n"
    "complete true\n"
    "enumerated net1,w1\n"
    "end\n"
    "evidence obligation_catalog stream=rack-A.cat generation=3 epoch=1\n"
    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:500 protected=false "
    "depends=net1\n"
    "obligation id=net1 kind=network_path rack=rack-A demand=network_kib:32 protected=false "
    "depends=-\n"
    "end\n";

// Every directive, every key, every record field, and every enum spelling the
// format has, in one document.
const char* const kRichScenario =
    "version 1\n"
    "planner planner-09\n"
    "request key=rich-01 authority=requester-09 epoch=9 time=-5000 lineage=lin-7 rack=rack-A "
    "isolation=thermal_constraint composition_revision=11 "
    "kinds=appliance,network_path,reservation,service_endpoint,storage_replica,workload\n"
    "source evidence=rack_composition authority=rack-authority stream=rack-A.comp\n"
    "source evidence=enumeration authority=rack-authority stream=rack-A.enum\n"
    "source evidence=obligation_catalog authority=rack-authority stream=rack-A.cat\n"
    "source evidence=capacity authority=facility-capacity stream=capacity\n"
    "source evidence=placement_policy authority=policy-authority stream=policy\n"
    "source evidence=maintenance authority=maintenance-authority stream=maint\n"
    "source evidence=failure_domain authority=facility-capacity stream=domains\n"
    "source evidence=asi_workload_state authority=agent-scheduler stream=asi\n"
    "source evidence=dfi_obligation authority=fabric-authority stream=dfi\n"
    "source evidence=candidate_offers authority=agent-scheduler stream=offers\n"
    "evidence rack_composition stream=rack-A.comp generation=11 epoch=3\n"
    "rack rack-A\n"
    "revision 11\n"
    "occupants w1,net1,res1,svc1,st1\n"
    "end\n"
    "evidence enumeration stream=rack-A.enum generation=11 epoch=3\n"
    "rack rack-A\n"
    "revision 11\n"
    "complete true\n"
    "enumerated net1,res1,st1,svc1,w1\n"
    "end\n"
    "evidence obligation_catalog stream=rack-A.cat generation=11 epoch=3\n"
    "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000;memory_bytes:2048 "
    "protected=false depends=-\n"
    "obligation id=st1 kind=storage_replica rack=rack-A demand=storage_bytes:4096 "
    "protected=true depends=w1\n"
    "obligation id=net1 kind=network_path rack=rack-A demand=network_kib:64 protected=false "
    "depends=-\n"
    "obligation id=svc1 kind=service_endpoint rack=rack-A demand=power_watts:120 protected=false "
    "depends=-\n"
    "obligation id=res1 kind=reservation rack=rack-A demand=rack_units:2 protected=false "
    "depends=-\n"
    "end\n"
    "evidence capacity stream=capacity generation=8 epoch=3\n"
    "destination destination=rack_slot:rack-B available=cpu_millicores:16000;memory_bytes:32768\n"
    "destination destination=fabric_endpoint:fab-1 available=network_kib:512\n"
    "destination destination=storage_target:vol-1 available=storage_bytes:65536\n"
    "destination destination=service_endpoint:ep-1 available=power_watts:400\n"
    "destination destination=rack_slot:rack-C available=rack_units:8\n"
    "end\n"
    "evidence placement_policy stream=policy generation=2 epoch=1\n"
    "rule kind=deny_action obligation=network_path action=detach destination=fabric_endpoint "
    "domain=-\n"
    "rule kind=allow_action obligation=workload action=live_migrate destination=rack_slot "
    "domain=-\n"
    "rule kind=require_domain_spread obligation=storage_replica action=rebind "
    "destination=storage_target domain=fd-3\n"
    "rule kind=allow_protected_move obligation=storage_replica action=rebind "
    "destination=storage_target domain=-\n"
    "end\n"
    "evidence maintenance stream=maint generation=5 epoch=3\n"
    "window domain=- destination=- action=live_migrate start=-100000 end=100000 permits=true\n"
    "window domain=fd-2 destination=- action=rebind start=0 end=200000 permits=true\n"
    "window domain=- destination=storage_target:vol-1 action=rebind start=500 end=900 "
    "permits=false\n"
    "window domain=fd-3 destination=fabric_endpoint:fab-1 action=detach start=0 end=50 "
    "permits=false\n"
    "incident domain=fd-9 severity=advisory blocks=false\n"
    "incident domain=fd-8 severity=degraded blocks=true\n"
    "incident domain=fd-7 severity=critical blocks=true\n"
    "end\n"
    "evidence failure_domain stream=domains generation=5 epoch=3\n"
    "member kind=rack rack=rack-A domain=fd-1\n"
    "member kind=destination destination=rack_slot:rack-B domain=fd-2\n"
    "member kind=obligation obligation=w1 domain=fd-3\n"
    "end\n"
    "evidence asi_workload_state stream=asi generation=5 epoch=3\n"
    "record obligation=w1 lifecycle=running migration=live_allowed attachment=stateless\n"
    "record obligation=net1 lifecycle=unknown migration=unknown attachment=unknown\n"
    "record obligation=res1 lifecycle=paused migration=cold_only attachment=shared_volume\n"
    "record obligation=svc1 lifecycle=stopped migration=not_migratable attachment=local_state\n"
    "end\n"
    "evidence dfi_obligation stream=dfi generation=5 epoch=3\n"
    "record obligation=net1 kind=zoning_entry path_migration=false endpoints=- "
    "mapping_digest=fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210\n"
    "record obligation=st1 kind=replica_lease path_migration=true "
    "endpoints=storage_target:vol-1,storage_target:vol-2 "
    "mapping_digest=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
    "record obligation=svc1 kind=path_attachment path_migration=true "
    "endpoints=service_endpoint:ep-1 "
    "mapping_digest=0000000000000000000000000000000000000000000000000000000000000000\n"
    "end\n"
    "evidence candidate_offers stream=offers generation=5 epoch=3\n"
    "candidate id=c-2 obligation=w1 action=live_migrate destination=rack_slot:rack-C "
    "authority=agent-scheduler generation=5 epoch=3 "
    "provision=cpu_millicores:1200;memory_bytes:2048 cost=4294967295 window=true\n"
    "candidate id=c-1 obligation=w1 action=live_migrate destination=rack_slot:rack-B "
    "authority=agent-scheduler generation=5 epoch=3 "
    "provision=cpu_millicores:1000;memory_bytes:2048 cost=7 window=false\n"
    "end\n";

// The line numbers of the complete scenario above, so the diagnostic table can
// name exactly which line it damaged.
constexpr std::size_t kCompositionHeaderLine = 11;
constexpr std::size_t kCompositionOccupantsLine = 14;
constexpr std::size_t kEnumerationCompleteLine = 19;
constexpr std::size_t kObligationLine = 23;
constexpr std::size_t kCapacityDestinationLine = 26;
constexpr std::size_t kCandidateLine = 36;
constexpr std::size_t kLastLine = 37;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      lines.push_back(std::string(text.substr(start)));
      break;
    }
    lines.push_back(std::string(text.substr(start, end - start)));
    start = end + 1;
  }
  return lines;
}

std::string join_lines(const std::vector<std::string>& lines) {
  std::string out;
  for (const std::string& line : lines) {
    out += line;
    out += '\n';
  }
  return out;
}

std::vector<std::string> full_scenario_lines() { return split_lines(kFullScenario); }

std::string line_at(std::string_view text, std::size_t number) {
  const std::vector<std::string> lines = split_lines(text);
  if (number == 0 || number > lines.size()) {
    return std::string();
  }
  return lines[number - 1];
}

struct Location {
  std::size_t line{0};
  std::size_t column{0};
  std::string detail;
};

bool starts_with_at(std::string_view text, std::size_t index, std::string_view prefix) {
  return index + prefix.size() <= text.size() && text.compare(index, prefix.size(), prefix) == 0;
}

// Decomposes "line <N> column <C>: <detail>" exactly, so a diagnostic that is
// not shaped that way is a failure rather than a substring match.
bool extract_location(std::string_view message, Location& out) {
  if (!starts_with_at(message, 0, "line ")) {
    return false;
  }
  std::size_t index = 5;
  std::size_t line = 0;
  while (index < message.size() && message[index] >= '0' && message[index] <= '9') {
    line = line * 10 + static_cast<std::size_t>(message[index] - '0');
    ++index;
  }
  if (line == 0 || !starts_with_at(message, index, " column ")) {
    return false;
  }
  index += 8;
  std::size_t column = 0;
  while (index < message.size() && message[index] >= '0' && message[index] <= '9') {
    column = column * 10 + static_cast<std::size_t>(message[index] - '0');
    ++index;
  }
  if (column == 0 || !starts_with_at(message, index, ": ")) {
    return false;
  }
  out.line = line;
  out.column = column;
  out.detail = std::string(message.substr(index + 2));
  return true;
}

bool contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

const EvidenceRecord* find_record(const ScenarioDocument& document, EvidenceKind kind,
                                  std::string_view stream) {
  for (const EvidenceRecord& record : document.request.evidence.records()) {
    if (record.kind == kind && record.stamp.source.stream.view() == stream) {
      return &record;
    }
  }
  return nullptr;
}

std::string describe_failure(const std::string& name, const Error& error) {
  return name + ": " + error.message + " [" + std::string(to_string(error.code)) + "]";
}

struct DiagnosticCase {
  std::string name;
  std::string text;
  std::size_t line{0};
  std::string token;    // the offending token; the column must be its 1-based offset
  std::size_t column{0};   // consulted only when token is empty
  std::string detail;      // substring the message must contain
};

void check_diagnostic(const DiagnosticCase& item) {
  const Result<ScenarioDocument> parsed = parse_scenario(item.text);
  if (parsed.ok()) {
    REP_FAIL("case '" + item.name + "' parsed but must be refused");
    return;
  }
  const Error& error = parsed.error();
  REP_CHECK_MSG(error.code == ErrorCode::InvalidEncoding,
                "case '" + item.name + "' produced " + describe_failure(item.name, error));
  Location location;
  const bool shaped = extract_location(error.message, location);
  REP_CHECK_MSG(shaped, "case '" + item.name + "' message is not 'line N column C: detail': " +
                            error.message);
  if (!shaped) {
    return;
  }
  REP_CHECK_MSG(location.line == item.line,
                "case '" + item.name + "' reported line " + std::to_string(location.line) +
                    ", expected " + std::to_string(item.line) + ": " + error.message);
  std::size_t expected_column = item.column;
  if (!item.token.empty()) {
    const std::string source_line = line_at(item.text, item.line);
    const std::size_t offset = source_line.find(item.token);
    if (offset == std::string::npos) {
      REP_FAIL("case '" + item.name + "' names a token that is not on its line");
      return;
    }
    expected_column = offset + 1;
  }
  REP_CHECK_MSG(location.column == expected_column,
                "case '" + item.name + "' reported column " + std::to_string(location.column) +
                    ", expected " + std::to_string(expected_column) + ": " + error.message);
  REP_CHECK_MSG(contains(location.detail, item.detail),
                "case '" + item.name + "' detail does not contain '" + item.detail +
                    "': " + error.message);
}

} // namespace

// ---------------------------------------------------------------------------
// Acceptance
// ---------------------------------------------------------------------------

REP_TEST(Scenario, minimal_document_parses) {
  const Result<ScenarioDocument> parsed = parse_scenario(kMinimalScenario);
  REP_REQUIRE_OK(parsed);
  const ScenarioDocument& document = parsed.value();
  REP_CHECK_EQ(document.planner.str(), std::string("planner-min"));
  REP_CHECK_EQ(document.request.idempotency_key.str(), std::string("k-min"));
  REP_CHECK_EQ(document.request.requested_by.str(), std::string("requester-min"));
  REP_CHECK_EQ(document.request.isolation.rack.str(), std::string("rack-A"));
  REP_CHECK_EQ(document.request.evidence_sources.size(), std::size_t{1});
  REP_CHECK_EQ(document.request.evidence.size(), std::size_t{1});
  const EvidenceRecord* composition =
      find_record(document, EvidenceKind::RackComposition, "rack-A.comp");
  REP_REQUIRE(composition != nullptr);
  const auto& payload = std::get<RackCompositionPayload>(composition->payload);
  REP_CHECK(payload.occupants.empty());
  REP_CHECK_EQ(composition->stamp.source.authority.str(), std::string("rack-authority"));
}

REP_TEST(Scenario, complete_document_plans_to_complete) {
  const Result<ScenarioDocument> parsed = parse_scenario(kFullScenario);
  REP_REQUIRE_OK(parsed);
  const ScenarioDocument& document = parsed.value();
  REP_CHECK_EQ(document.request.evidence_sources.size(), std::size_t{7});
  REP_CHECK_EQ(document.request.evidence.size(), std::size_t{7});

  EngineOptions options;
  options.planner = document.planner;
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);

  const auto outcome = engine.value()->submit(document.request);
  REP_REQUIRE_OK(outcome);
  REP_CHECK(outcome.value().kind == OutcomeKind::Planned);
  REP_CHECK(outcome.value().has_plan);
  const Plan& plan = outcome.value().plan;
  REP_CHECK(plan.status == PlanStatus::Complete);
  REP_CHECK(plan.residuals.empty());
  REP_CHECK_EQ(plan.assignments.size(), std::size_t{1});
  REP_REQUIRE(!plan.assignments.empty());
  REP_CHECK_EQ(plan.assignments[0].obligation.str(), std::string("w1"));
  REP_CHECK_EQ(plan.assignments[0].candidate.str(), std::string("c1"));
  REP_CHECK_EQ(plan.assignments[0].destination.to_text(), std::string("rack_slot:rack-B"));
  REP_CHECK_EQ(plan.bindings.size(), std::size_t{7});
  REP_CHECK(plan.verify().ok());
  REP_CHECK(audit_plan(plan, document.request).empty());
  REP_CHECK(plan.verdict() == SafetyVerdict::Safe);
}

// ---------------------------------------------------------------------------
// Canonical rendering and round trips
// ---------------------------------------------------------------------------

REP_TEST(Scenario, rendering_is_stable_and_round_trips) {
  const std::string_view texts[] = {kMinimalScenario, kFullScenario, kEmptyScenario,
                                    kMultiKindScenario, kRichScenario};
  for (const std::string_view text : texts) {
    const Result<ScenarioDocument> first = parse_scenario(text);
    if (!first.ok()) {
      REP_FAIL(std::string("a fixture did not parse: ") + first.error().message);
      continue;
    }
    const std::string rendered_once = render_scenario(first.value());
    const std::string rendered_twice = render_scenario(first.value());
    REP_CHECK_EQ(rendered_once, rendered_twice);

    const Result<ScenarioDocument> second = parse_scenario(rendered_once);
    if (!second.ok()) {
      REP_FAIL(std::string("the canonical rendering did not parse: ") +
               second.error().message);
      continue;
    }
    REP_CHECK(second.value() == first.value());
    REP_CHECK_EQ(render_scenario(second.value()), rendered_once);
  }
}

REP_TEST(Scenario, comments_and_blank_lines_do_not_change_the_document) {
  std::vector<std::string> lines;
  lines.push_back("# leading comment");
  lines.push_back("");
  for (const std::string& line : full_scenario_lines()) {
    lines.push_back(line);
    lines.push_back("   # trailing comment");
  }
  lines.push_back("# trailing comment");

  const Result<ScenarioDocument> commented = parse_scenario(join_lines(lines));
  REP_REQUIRE_OK(commented);
  const Result<ScenarioDocument> plain = parse_scenario(kFullScenario);
  REP_REQUIRE_OK(plain);
  REP_CHECK(commented.value() == plain.value());
  REP_CHECK_EQ(render_scenario(commented.value()), render_scenario(plain.value()));
}

REP_TEST(Scenario, empty_rack_is_proven_empty_safe) {
  const Result<ScenarioDocument> parsed = parse_scenario(kEmptyScenario);
  REP_REQUIRE_OK(parsed);
  EngineOptions options;
  options.planner = parsed.value().planner;
  const auto engine = PlanEngine::open(options);
  REP_REQUIRE_OK(engine);
  const auto outcome = engine.value()->submit(parsed.value().request);
  REP_REQUIRE_OK(outcome);
  REP_CHECK(outcome.value().kind == OutcomeKind::Planned);
  REP_CHECK(outcome.value().plan.status == PlanStatus::EmptySafe);
  REP_CHECK(outcome.value().plan.assignments.empty());
  REP_CHECK(outcome.value().plan.indeterminacy_reasons.empty());
  REP_CHECK(outcome.value().plan.verify().ok());
}

// ---------------------------------------------------------------------------
// Field coverage
// ---------------------------------------------------------------------------

REP_TEST(Scenario, every_field_survives_a_round_trip) {
  const Result<ScenarioDocument> parsed = parse_scenario(kRichScenario);
  REP_REQUIRE_OK(parsed);
  const ScenarioDocument& document = parsed.value();
  const PlanRequest& request = document.request;

  REP_CHECK_EQ(document.planner.str(), std::string("planner-09"));
  REP_CHECK_EQ(request.idempotency_key.str(), std::string("rich-01"));
  REP_CHECK_EQ(request.requested_by.str(), std::string("requester-09"));
  REP_CHECK_EQ(request.expected_epoch.value(), std::uint64_t{9});
  REP_CHECK_EQ(request.evaluation_time.value(), std::int64_t{-5000});
  REP_CHECK_EQ(request.lineage.str(), std::string("lin-7"));
  REP_CHECK_EQ(request.isolation.rack.str(), std::string("rack-A"));
  REP_CHECK(request.isolation.kind == IsolationKind::ThermalConstraint);
  REP_CHECK_EQ(request.isolation.composition_revision.value(), std::uint64_t{11});
  const std::vector<ObligationKind> expected_kinds = {
      ObligationKind::Workload,        ObligationKind::StorageReplica,
      ObligationKind::NetworkPath,     ObligationKind::ServiceEndpoint,
      ObligationKind::Reservation,     ObligationKind::Appliance};
  REP_CHECK(request.isolation.evacuate_kinds == expected_kinds);

  // All ten evidence kinds, in the canonical order the bundle imposes.
  REP_CHECK_EQ(request.evidence_sources.size(), std::size_t{10});
  REP_CHECK_EQ(request.evidence.size(), std::size_t{10});
  REP_REQUIRE(request.evidence_sources.size() == std::size_t{10});
  for (std::size_t index = 0; index < request.evidence_sources.size(); ++index) {
    const auto kind = static_cast<EvidenceKind>(index + 1);
    REP_CHECK_EQ(request.evidence_sources[index].kind, kind);
    REP_CHECK_EQ(std::string(to_string(request.evidence_sources[index].kind)),
                 std::string(to_string(kind)));
  }

  const EvidenceRecord* composition =
      find_record(document, EvidenceKind::RackComposition, "rack-A.comp");
  REP_REQUIRE(composition != nullptr);
  {
    const auto& payload = std::get<RackCompositionPayload>(composition->payload);
    REP_CHECK_EQ(payload.rack.str(), std::string("rack-A"));
    REP_CHECK_EQ(payload.composition_revision.value(), std::uint64_t{11});
    const std::vector<ObligationId> occupants = {
        ObligationId{std::string("net1")}, ObligationId{std::string("res1")},
        ObligationId{std::string("st1")},  ObligationId{std::string("svc1")},
        ObligationId{std::string("w1")}};
    REP_CHECK(payload.occupants == occupants);
  }
  {
    const EvidenceRecord* enumeration =
        find_record(document, EvidenceKind::Enumeration, "rack-A.enum");
    REP_REQUIRE(enumeration != nullptr);
    const auto& payload = std::get<EnumerationPayload>(enumeration->payload);
    REP_CHECK(payload.complete);
    REP_CHECK_EQ(payload.enumerated.size(), std::size_t{5});
  }
  {
    const EvidenceRecord* catalog =
        find_record(document, EvidenceKind::ObligationCatalog, "rack-A.cat");
    REP_REQUIRE(catalog != nullptr);
    const auto& payload = std::get<ObligationCatalogPayload>(catalog->payload);
    REP_CHECK_EQ(payload.obligations.size(), std::size_t{5});
    const Obligation* storage = payload.find(ObligationId{std::string("st1")});
    REP_REQUIRE(storage != nullptr);
    REP_CHECK(storage->protected_obligation);
    REP_CHECK(storage->kind == ObligationKind::StorageReplica);
    REP_CHECK_EQ(storage->demand.to_text(), std::string("storage_bytes:4096"));
    const std::vector<ObligationId> depends = {ObligationId{std::string("w1")}};
    REP_CHECK(storage->depends_on == depends);
    const Obligation* workload = payload.find(ObligationId{std::string("w1")});
    REP_REQUIRE(workload != nullptr);
    REP_CHECK_EQ(workload->demand.to_text(),
                 std::string("cpu_millicores:1000;memory_bytes:2048"));
    REP_CHECK(workload->depends_on.empty());
  }
  {
    const EvidenceRecord* capacity = find_record(document, EvidenceKind::Capacity, "capacity");
    REP_REQUIRE(capacity != nullptr);
    const auto& payload = std::get<CapacityPayload>(capacity->payload);
    REP_CHECK_EQ(payload.destinations.size(), std::size_t{5});
    const DestinationCapacity* slot =
        payload.find(DestinationRef{DestinationKind::RackSlot, DestinationId{std::string("rack-C")}});
    REP_REQUIRE(slot != nullptr);
    REP_CHECK_EQ(slot->available.to_text(), std::string("rack_units:8"));
    REP_CHECK_EQ(slot->generation.value(), std::uint64_t{8});
    REP_CHECK_EQ(slot->epoch.value(), std::uint64_t{3});
  }
  {
    const EvidenceRecord* policy =
        find_record(document, EvidenceKind::PlacementPolicy, "policy");
    REP_REQUIRE(policy != nullptr);
    const auto& payload = std::get<PlacementPolicyPayload>(policy->payload);
    REP_CHECK_EQ(payload.rules.size(), std::size_t{4});
    bool saw_deny = false;
    bool saw_allow = false;
    bool saw_spread = false;
    bool saw_protected = false;
    for (const PolicyRule& rule : payload.rules) {
      if (rule.kind == PolicyRuleKind::DenyAction) {
        saw_deny = true;
        REP_CHECK(rule.obligation_kind == ObligationKind::NetworkPath);
        REP_CHECK(rule.action == ActionKind::Detach);
        REP_CHECK(rule.destination_kind == DestinationKind::FabricEndpoint);
        REP_CHECK(rule.domain.empty());
      } else if (rule.kind == PolicyRuleKind::AllowAction) {
        saw_allow = true;
        REP_CHECK(rule.obligation_kind == ObligationKind::Workload);
        REP_CHECK(rule.action == ActionKind::LiveMigrate);
      } else if (rule.kind == PolicyRuleKind::RequireDomainSpread) {
        saw_spread = true;
        REP_CHECK_EQ(rule.domain.str(), std::string("fd-3"));
      } else if (rule.kind == PolicyRuleKind::AllowProtectedMove) {
        saw_protected = true;
        REP_CHECK(rule.domain.empty());
      }
    }
    REP_CHECK(saw_deny);
    REP_CHECK(saw_allow);
    REP_CHECK(saw_spread);
    REP_CHECK(saw_protected);
  }
  {
    const EvidenceRecord* maintenance = find_record(document, EvidenceKind::Maintenance, "maint");
    REP_REQUIRE(maintenance != nullptr);
    const auto& payload = std::get<MaintenancePayload>(maintenance->payload);
    REP_CHECK_EQ(payload.windows.size(), std::size_t{4});
    REP_CHECK_EQ(payload.incidents.size(), std::size_t{3});
    bool saw_any_domain = false;
    bool saw_domain_only = false;
    bool saw_destination_only = false;
    bool saw_both = false;
    for (const MaintenanceWindow& window : payload.windows) {
      const bool has_domain = !window.domain.empty();
      const bool has_destination = !window.destination.id.empty();
      if (!has_domain && !has_destination) {
        saw_any_domain = true;
        REP_CHECK_EQ(window.starts_at.value(), std::int64_t{-100000});
        REP_CHECK_EQ(window.ends_at.value(), std::int64_t{100000});
        REP_CHECK(window.permits_evacuation);
      } else if (has_domain && !has_destination) {
        saw_domain_only = true;
        REP_CHECK_EQ(window.domain.str(), std::string("fd-2"));
        REP_CHECK(window.action == ActionKind::Rebind);
      } else if (!has_domain && has_destination) {
        saw_destination_only = true;
        REP_CHECK_EQ(window.destination.to_text(), std::string("storage_target:vol-1"));
        REP_CHECK(!window.permits_evacuation);
      } else {
        saw_both = true;
        REP_CHECK_EQ(window.domain.str(), std::string("fd-3"));
        REP_CHECK_EQ(window.destination.to_text(), std::string("fabric_endpoint:fab-1"));
        REP_CHECK(window.action == ActionKind::Detach);
      }
    }
    REP_CHECK(saw_any_domain);
    REP_CHECK(saw_domain_only);
    REP_CHECK(saw_destination_only);
    REP_CHECK(saw_both);
    bool saw_non_blocking = false;
    bool saw_critical = false;
    for (const Incident& incident : payload.incidents) {
      REP_CHECK(!incident.domain.empty());
      if (!incident.blocks_evacuation) {
        saw_non_blocking = true;
        REP_CHECK(incident.severity == IncidentSeverity::Advisory);
        REP_CHECK_EQ(incident.domain.str(), std::string("fd-9"));
      }
      if (incident.severity == IncidentSeverity::Critical) {
        saw_critical = true;
        REP_CHECK(incident.blocks_evacuation);
      }
    }
    REP_CHECK(saw_non_blocking);
    REP_CHECK(saw_critical);
  }
  {
    const EvidenceRecord* domains = find_record(document, EvidenceKind::FailureDomain, "domains");
    REP_REQUIRE(domains != nullptr);
    const auto& topology = std::get<FailureDomainTopology>(domains->payload);
    REP_CHECK_EQ(topology.members.size(), std::size_t{3});
    const auto rack_domain = topology.domain_of_rack(RackId{std::string("rack-A")});
    REP_REQUIRE(rack_domain.has_value());
    REP_CHECK_EQ(rack_domain->str(), std::string("fd-1"));
    const auto destination_domain = topology.domain_of_destination(
        DestinationRef{DestinationKind::RackSlot, DestinationId{std::string("rack-B")}});
    REP_REQUIRE(destination_domain.has_value());
    REP_CHECK_EQ(destination_domain->str(), std::string("fd-2"));
    const auto obligation_domain =
        topology.domain_of_obligation(ObligationId{std::string("w1")});
    REP_REQUIRE(obligation_domain.has_value());
    REP_CHECK_EQ(obligation_domain->str(), std::string("fd-3"));
  }
  {
    const EvidenceRecord* asi =
        find_record(document, EvidenceKind::AsiWorkloadState, "asi");
    REP_REQUIRE(asi != nullptr);
    const auto& payload = std::get<AsiWorkloadPayload>(asi->payload);
    REP_CHECK_EQ(payload.records.size(), std::size_t{4});
    const WorkloadStateRecord* workload = payload.find(ObligationId{std::string("w1")});
    REP_REQUIRE(workload != nullptr);
    REP_CHECK(workload->lifecycle == WorkloadLifecycle::Running);
    REP_CHECK(workload->migration == MigrationCapability::LiveAllowed);
    REP_CHECK(workload->attachment == StorageAttachment::Stateless);
    const WorkloadStateRecord* endpoint = payload.find(ObligationId{std::string("svc1")});
    REP_REQUIRE(endpoint != nullptr);
    REP_CHECK(endpoint->lifecycle == WorkloadLifecycle::Stopped);
    REP_CHECK(endpoint->migration == MigrationCapability::NotMigratable);
    REP_CHECK(endpoint->attachment == StorageAttachment::LocalState);
    const WorkloadStateRecord* unknown = payload.find(ObligationId{std::string("net1")});
    REP_REQUIRE(unknown != nullptr);
    REP_CHECK(unknown->lifecycle == WorkloadLifecycle::Unknown);
    REP_CHECK(unknown->migration == MigrationCapability::Unknown);
    REP_CHECK(unknown->attachment == StorageAttachment::Unknown);
  }
  {
    const EvidenceRecord* dfi = find_record(document, EvidenceKind::DfiObligation, "dfi");
    REP_REQUIRE(dfi != nullptr);
    const auto& payload = std::get<DfiObligationPayload>(dfi->payload);
    REP_CHECK_EQ(payload.records.size(), std::size_t{3});
    const FabricObligationRecord* replica = payload.find(ObligationId{std::string("st1")});
    REP_REQUIRE(replica != nullptr);
    REP_CHECK(replica->kind == FabricObligationKind::ReplicaLease);
    REP_CHECK(replica->path_migration_supported);
    REP_CHECK_EQ(replica->permitted_endpoints.size(), std::size_t{2});
    REP_CHECK_EQ(replica->permitted_endpoints[0].to_text(),
                 std::string("storage_target:vol-1"));
    REP_CHECK_EQ(replica->permitted_endpoints[1].to_text(),
                 std::string("storage_target:vol-2"));
    const std::optional<Digest> expected_mapping =
        Digest::from_hex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    REP_REQUIRE(expected_mapping.has_value());
    REP_CHECK(replica->mapping_digest == expected_mapping.value());
    const FabricObligationRecord* zoning = payload.find(ObligationId{std::string("net1")});
    REP_REQUIRE(zoning != nullptr);
    REP_CHECK(zoning->kind == FabricObligationKind::ZoningEntry);
    REP_CHECK(!zoning->path_migration_supported);
    REP_CHECK(zoning->permitted_endpoints.empty());
    const FabricObligationRecord* attachment = payload.find(ObligationId{std::string("svc1")});
    REP_REQUIRE(attachment != nullptr);
    REP_CHECK(attachment->kind == FabricObligationKind::PathAttachment);
    REP_CHECK(attachment->mapping_digest.is_zero());
  }
  {
    const EvidenceRecord* offers =
        find_record(document, EvidenceKind::CandidateOffers, "offers");
    REP_REQUIRE(offers != nullptr);
    const auto& payload = std::get<CandidateOffersPayload>(offers->payload);
    REP_CHECK_EQ(payload.candidates.size(), std::size_t{2});
    const Candidate* expensive = payload.find(CandidateId{std::string("c-2")});
    REP_REQUIRE(expensive != nullptr);
    REP_CHECK_EQ(expensive->estimated_cost, std::uint32_t{4294967295u});
    REP_CHECK(expensive->requires_maintenance_window);
    REP_CHECK_EQ(expensive->destination.to_text(), std::string("rack_slot:rack-C"));
    REP_CHECK_EQ(expensive->authority_generation.value(), std::uint64_t{5});
    REP_CHECK_EQ(expensive->authority_epoch.value(), std::uint64_t{3});
    REP_CHECK_EQ(expensive->evidence_digest, expensive->content_digest());
    const Candidate* cheap = payload.find(CandidateId{std::string("c-1")});
    REP_REQUIRE(cheap != nullptr);
    REP_CHECK_EQ(cheap->estimated_cost, std::uint32_t{7});
    REP_CHECK(!cheap->requires_maintenance_window);
  }
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

REP_TEST(Scenario, diagnostics_name_the_line_the_column_and_the_key) {
  std::vector<DiagnosticCase> cases;
  const auto add = [&cases](std::string name, std::string text, std::size_t line,
                            std::string token, std::size_t column, std::string detail) {
    DiagnosticCase item;
    item.name = std::move(name);
    item.text = std::move(text);
    item.line = line;
    item.token = std::move(token);
    item.column = column;
    item.detail = std::move(detail);
    cases.push_back(std::move(item));
  };
  const auto replace_line = [](const std::vector<std::string>& lines, std::size_t number,
                               std::string text) {
    std::vector<std::string> copy = lines;
    copy[number - 1] = std::move(text);
    return join_lines(copy);
  };
  const auto insert_line = [](const std::vector<std::string>& lines, std::size_t before,
                              std::string text) {
    std::vector<std::string> copy = lines;
    copy.insert(copy.begin() + static_cast<std::ptrdiff_t>(before - 1), std::move(text));
    return join_lines(copy);
  };
  const std::vector<std::string> lines = full_scenario_lines();

  add("empty document", std::string(), 1, std::string(), 1, "missing version directive");
  add("no planner", "version 1\n", 1, std::string(), 1, "missing planner directive");
  add("no request", "version 1\nplanner planner-01\n", 1, std::string(), 1,
      "missing request directive");
  add("unknown directive", insert_line(lines, 1, "frobnicate"), 1, "frobnicate", 0,
      "unknown directive 'frobnicate'");
  add("trailing content", insert_line(lines, kLastLine + 1, "leftover data"), kLastLine + 1,
      "leftover", 0, "unknown directive 'leftover'");
  add("duplicate version", insert_line(lines, kLastLine + 1, "version 1"), kLastLine + 1,
      "version", 0, "duplicate version directive");
  add("unsupported version", replace_line(lines, 1, "version 2"), 1, std::string(), 9,
      "unsupported scenario format version '2'");
  add("duplicate planner", insert_line(lines, 3, "planner planner-02"), 3, "planner", 0,
      "duplicate planner directive");
  add("duplicate request", insert_line(lines, 4, std::string(lines[2])), 4, "request", 0,
      "duplicate request directive");
  add("unknown key in the request",
      replace_line(lines, 3, std::string(lines[2]) + " extra=1"), 3, "extra=1", 0,
      "unknown key 'extra'");
  add("duplicate key in the request",
      replace_line(lines, 3, std::string(lines[2]) + " authority=requester-02"), 3,
      "authority=requester-02", 0, "duplicate key 'authority'");
  add("missing required key in the request",
      replace_line(lines, 3,
                   "request key=k1 authority=requester-01 epoch=0 time=1000 lineage=- rack=rack-A "
                   "isolation=depower composition_revision=7"),
      3, std::string(), 1, "missing required key 'kinds'");
  add("unknown isolation kind",
      replace_line(lines, 3,
                   "request key=k1 authority=requester-01 epoch=0 time=1000 lineage=- rack=rack-A "
                   "isolation=meltdown composition_revision=7 kinds=workload"),
      3, "isolation=meltdown", 0, "key 'isolation' has unknown value 'meltdown'");
  add("invalid lineage identifier",
      replace_line(lines, 3,
                   "request key=k1 authority=requester-01 epoch=0 time=1000 lineage=lin! rack=rack-A "
                   "isolation=depower composition_revision=7 kinds=workload"),
      3, "lineage=lin!", 0, "identifier contains an unsupported character");
  add("kinds with an empty list item",
      replace_line(lines, 3,
                   "request key=k1 authority=requester-01 epoch=0 time=1000 lineage=- rack=rack-A "
                   "isolation=depower composition_revision=7 kinds=workload,,reservation"),
      3, "kinds=workload,,reservation", 0, "has an empty list item");
  add("kinds with an unknown value",
      replace_line(lines, 3,
                   "request key=k1 authority=requester-01 epoch=0 time=1000 lineage=- rack=rack-A "
                   "isolation=depower composition_revision=7 kinds=gadget"),
      3, "kinds=gadget", 0, "kinds has unknown value 'gadget'");
  add("duplicate source declaration", insert_line(lines, 5, std::string(lines[3])), 5,
      "stream=rack-A.comp", 0, "duplicate source declaration for this stream");
  add("source with an unknown evidence kind",
      replace_line(lines, 4,
                   "source evidence=bogus authority=rack-authority stream=rack-A.comp"),
      4, "evidence=bogus", 0, "key 'evidence' has unknown value 'bogus'");
  add("unknown evidence kind",
      replace_line(lines, kCompositionHeaderLine,
                   "evidence bogus stream=rack-A.comp generation=7 epoch=2"),
      kCompositionHeaderLine, "bogus", 0, "unknown evidence kind 'bogus'");
  add("evidence before the request",
      insert_line(lines, 3, std::string(lines[kCompositionHeaderLine - 1])), 3, "evidence", 0,
      "evidence must follow the request");
  add("evidence with no source",
      replace_line(lines, kCompositionHeaderLine,
                   "evidence rack_composition stream=ghost.comp generation=7 epoch=2"),
      kCompositionHeaderLine, "stream=ghost.comp", 0,
      "evidence block names a stream that no source directive declares");
  add("malformed generation",
      replace_line(lines, kCompositionHeaderLine,
                   "evidence rack_composition stream=rack-A.comp generation=abc epoch=2"),
      kCompositionHeaderLine, "generation=abc", 0,
      "key 'generation' must be an unsigned decimal");
  add("negative generation",
      replace_line(lines, kCompositionHeaderLine,
                   "evidence rack_composition stream=rack-A.comp generation=-1 epoch=2"),
      kCompositionHeaderLine, "generation=-1", 0,
      "key 'generation' must be an unsigned decimal");
  add("missing evidence key",
      replace_line(lines, kCompositionHeaderLine,
                   "evidence rack_composition stream=rack-A.comp epoch=2"),
      kCompositionHeaderLine, std::string(), 1, "missing required key 'generation'");
  add("unknown evidence key",
      replace_line(lines, kCompositionHeaderLine,
                   "evidence rack_composition stream=rack-A.comp generation=7 epoch=2 source=x"),
      kCompositionHeaderLine, "source=x", 0, "unknown key 'source'");
  add("malformed digest",
      replace_line(lines, kCompositionHeaderLine,
                   "evidence rack_composition stream=rack-A.comp generation=7 epoch=2 digest=zz"),
      kCompositionHeaderLine, "digest=zz", 0,
      "digest must be 64 hexadecimal characters");
  add("body line with too many tokens",
      replace_line(lines, kCompositionOccupantsLine, "occupants w1 extra=1"),
      kCompositionOccupantsLine, std::string(), 1, "body lines are '<key> <value>'");
  add("unknown composition key",
      replace_line(lines, kCompositionOccupantsLine, "tenants w1"),
      kCompositionOccupantsLine, "tenants", 0, "unknown key 'tenants'");
  add("duplicate composition key",
      replace_line(lines, kCompositionOccupantsLine, "rack rack-A"), kCompositionOccupantsLine,
      "rack", 0, "duplicate key 'rack'");
  add("invalid identifier in a body line",
      replace_line(lines, 12, "rack rack-A!"), 12, "rack-A!", 0,
      "identifier contains an unsupported character at offset 6");
  add("not a boolean",
      replace_line(lines, kEnumerationCompleteLine, "complete maybe"), kEnumerationCompleteLine,
      "maybe", 0, "key 'complete' must be true or false");
  add("record with an unknown key",
      replace_line(lines, kObligationLine,
                   "obligation extra=1 id=w1 kind=workload rack=rack-A "
                   "demand=cpu_millicores:1000 protected=false depends=-"),
      kObligationLine, "extra=1", 0, "unknown key 'extra'");
  add("record with a duplicate key",
      replace_line(lines, kObligationLine,
                   "obligation id=w1 id=w2 kind=workload rack=rack-A "
                   "demand=cpu_millicores:1000 protected=false depends=-"),
      kObligationLine, "id=w2", 0, "duplicate key 'id'");
  add("record missing a required key",
      replace_line(lines, kObligationLine,
                   "obligation id=w1 rack=rack-A demand=cpu_millicores:1000 protected=false "
                   "depends=-"),
      kObligationLine, std::string(), 1, "missing required key 'kind'");
  add("record with an unknown enum value",
      replace_line(lines, kObligationLine,
                   "obligation id=w1 kind=gadget rack=rack-A demand=cpu_millicores:1000 "
                   "protected=false depends=-"),
      kObligationLine, "kind=gadget", 0, "key 'kind' has unknown value 'gadget'");
  add("record with a malformed demand",
      replace_line(lines, kObligationLine,
                   "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores "
                   "protected=false depends=-"),
      kObligationLine, "demand=cpu_millicores", 0, "resource token must be class:amount");
  add("record with an unknown resource class",
      replace_line(lines, kObligationLine,
                   "obligation id=w1 kind=workload rack=rack-A demand=gpu_cores:1 "
                   "protected=false depends=-"),
      kObligationLine, "demand=gpu_cores:1", 0, "unknown resource class 'gpu_cores'");
  add("record with a malformed resource amount",
      replace_line(lines, kObligationLine,
                   "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:lots "
                   "protected=false depends=-"),
      kObligationLine, "demand=cpu_millicores:lots", 0,
      "resource amount must be an unsigned decimal");
  add("record with a duplicate resource class",
      replace_line(lines, kObligationLine,
                   "obligation id=w1 kind=workload rack=rack-A "
                   "demand=cpu_millicores:1;cpu_millicores:2 protected=false depends=-"),
      kObligationLine, "demand=cpu_millicores:1;cpu_millicores:2", 0,
      "resource vector repeats class cpu_millicores");
  add("record with an empty depends item",
      replace_line(lines, kObligationLine,
                   "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 "
                   "protected=false depends=w1,"),
      kObligationLine, "depends=w1,", 0, "has an empty list item");
  add("bad destination form",
      replace_line(lines, kCapacityDestinationLine,
                   "destination destination=rack-B available=cpu_millicores:8000"),
      kCapacityDestinationLine, "destination=rack-B", 0, "destination must be kind:id");
  add("unknown destination kind",
      replace_line(lines, kCapacityDestinationLine,
                   "destination destination=slot:rack-B available=cpu_millicores:8000"),
      kCapacityDestinationLine, "destination=slot:rack-B", 0,
      "unknown destination kind 'slot'");
  add("missing end", join_lines(std::vector<std::string>(lines.begin(), lines.end() - 1)),
      kLastLine, std::string(), 1, "evidence block was not closed with 'end'");
  add("end without an open block", insert_line(lines, 4, "end"), 4, "end", 0,
      "'end' without an open evidence block");

  REP_CHECK_EQ(cases.size(), std::size_t{44});
  for (const DiagnosticCase& item : cases) {
    check_diagnostic(item);
  }
}

// ---------------------------------------------------------------------------
// Byte-level strictness
// ---------------------------------------------------------------------------

REP_TEST(Scenario, byte_level_strictness) {
  const Result<ScenarioDocument> baseline = parse_scenario(kFullScenario);
  REP_REQUIRE_OK(baseline);

  // A UTF-8 byte order mark is a container artifact and is accepted.
  std::string with_bom = "\xef\xbb\xbf";
  with_bom += kFullScenario;
  const Result<ScenarioDocument> bom = parse_scenario(with_bom);
  REP_REQUIRE_OK(bom);
  REP_CHECK(bom.value() == baseline.value());

  // A lone CR - one that is not the second half of a CRLF pair - is refused.
  std::string lone_cr = kFullScenario;
  const std::size_t first_newline = lone_cr.find('\n');
  REP_REQUIRE(first_newline != std::string::npos);
  lone_cr[first_newline] = '\r';
  const Result<ScenarioDocument> cr = parse_scenario(lone_cr);
  REP_REQUIRE(!cr.ok());
  REP_CHECK(cr.error().code == ErrorCode::InvalidEncoding);
  Location location;
  REP_CHECK(extract_location(cr.error().message, location));
  REP_CHECK_EQ(location.line, std::size_t{1});
  REP_CHECK(contains(location.detail, "printable ASCII"));

  // A CRLF file is still accepted: the CR is the second half of a pair.
  std::string crlf = kFullScenario;
  std::string::size_type position = 0;
  while ((position = crlf.find('\n', position)) != std::string::npos) {
    crlf.insert(position, 1, '\r');
    position += 2;
  }
  const Result<ScenarioDocument> windows = parse_scenario(crlf);
  REP_REQUIRE_OK(windows);
  REP_CHECK(windows.value() == baseline.value());

  // A stray non-ASCII byte anywhere is refused, including inside an identifier.
  const std::string non_ascii = "\xc3\xa9";
  std::vector<std::string> lines = full_scenario_lines();
  lines[1] = "planner planner-" + non_ascii;
  const Result<ScenarioDocument> identifier = parse_scenario(join_lines(lines));
  REP_REQUIRE(!identifier.ok());
  REP_CHECK(identifier.error().code == ErrorCode::InvalidEncoding);
  REP_CHECK(extract_location(identifier.error().message, location));
  REP_CHECK_EQ(location.line, std::size_t{2});
  REP_CHECK(contains(location.detail, "printable ASCII"));

  lines = full_scenario_lines();
  lines.push_back("# trailing " + non_ascii);
  const Result<ScenarioDocument> comment = parse_scenario(join_lines(lines));
  REP_REQUIRE(!comment.ok());
  REP_CHECK(comment.error().code == ErrorCode::InvalidEncoding);
  REP_CHECK(extract_location(comment.error().message, location));
  REP_CHECK_EQ(location.line, kLastLine + 1);

  std::string trailing_byte = kFullScenario;
  trailing_byte.push_back(static_cast<char>(0x80));
  const Result<ScenarioDocument> stray = parse_scenario(trailing_byte);
  REP_REQUIRE(!stray.ok());
  REP_CHECK(stray.error().code == ErrorCode::InvalidEncoding);
  REP_CHECK(extract_location(stray.error().message, location));
  REP_CHECK_EQ(location.line, kLastLine + 1);
  REP_CHECK_EQ(location.column, std::size_t{1});
}

REP_TEST(Scenario, limits_are_hard_refusals) {
  const Result<ScenarioDocument> baseline = parse_scenario(kFullScenario);
  REP_REQUIRE_OK(baseline);

  // A byte limit one byte below the input refuses it before parsing.
  ScenarioLimits byte_limits;
  byte_limits.max_bytes = std::char_traits<char>::length(kFullScenario) - 1;
  const Result<ScenarioDocument> too_many_bytes = parse_scenario(kFullScenario, byte_limits);
  REP_REQUIRE(!too_many_bytes.ok());
  REP_CHECK(too_many_bytes.error().code == ErrorCode::LimitExceeded);
  REP_CHECK(contains(too_many_bytes.error().message, "byte limit"));

  // The byte limit is a boundary: exactly the input length is accepted.
  ScenarioLimits exact_limits;
  exact_limits.max_bytes = std::char_traits<char>::length(kFullScenario);
  const Result<ScenarioDocument> exact = parse_scenario(kFullScenario, exact_limits);
  REP_REQUIRE_OK(exact);
  REP_CHECK(exact.value() == baseline.value());

  ScenarioLimits line_limits;
  line_limits.max_lines = kLastLine - 1;
  const Result<ScenarioDocument> too_many_lines = parse_scenario(kFullScenario, line_limits);
  REP_REQUIRE(!too_many_lines.ok());
  REP_CHECK(too_many_lines.error().code == ErrorCode::LimitExceeded);
  REP_CHECK(contains(too_many_lines.error().message, "line limit"));

  ScenarioLimits length_limits;
  length_limits.max_line_length = 20;
  const Result<ScenarioDocument> too_long = parse_scenario(kFullScenario, length_limits);
  REP_REQUIRE(!too_long.ok());
  REP_CHECK(too_long.error().code == ErrorCode::InvalidEncoding);
  Location location;
  REP_CHECK(extract_location(too_long.error().message, location));
  REP_CHECK_EQ(location.line, std::size_t{3});
  REP_CHECK_EQ(location.column, std::size_t{1});
  REP_CHECK(contains(location.detail, "maximum line length"));

  // The default line length is enforced on a line no fixture contains.
  std::string long_line = kFullScenario;
  long_line += "#";
  long_line.append(ScenarioLimits{}.max_line_length + 8, 'x');
  long_line += "\n";
  const Result<ScenarioDocument> over_default = parse_scenario(long_line);
  REP_REQUIRE(!over_default.ok());
  REP_CHECK(over_default.error().code == ErrorCode::InvalidEncoding);
  REP_CHECK(extract_location(over_default.error().message, location));
  REP_CHECK_EQ(location.line, kLastLine + 1);

  ScenarioLimits record_limits;
  record_limits.max_evidence_records = 2;
  const Result<ScenarioDocument> too_many_records = parse_scenario(kFullScenario, record_limits);
  REP_REQUIRE(!too_many_records.ok());
  REP_CHECK(too_many_records.error().code == ErrorCode::LimitExceeded);
  REP_CHECK(contains(too_many_records.error().message, "record"));

  ScenarioLimits obligation_limits;
  obligation_limits.max_obligations = 0;
  const Result<ScenarioDocument> too_many_obligations =
      parse_scenario(kFullScenario, obligation_limits);
  REP_REQUIRE(!too_many_obligations.ok());
  REP_CHECK(too_many_obligations.error().code == ErrorCode::InvalidEncoding);
  REP_CHECK(contains(too_many_obligations.error().message, "more obligations than the limit"));

  ScenarioLimits candidate_limits;
  candidate_limits.max_candidates = 0;
  const Result<ScenarioDocument> too_many_candidates =
      parse_scenario(kFullScenario, candidate_limits);
  REP_REQUIRE(!too_many_candidates.ok());
  REP_CHECK(too_many_candidates.error().code == ErrorCode::InvalidEncoding);
  REP_CHECK(contains(too_many_candidates.error().message, "more candidates than the limit"));
}

// ---------------------------------------------------------------------------
// Claimed digests
// ---------------------------------------------------------------------------

REP_TEST(Scenario, a_claimed_digest_that_does_not_match_is_refused) {
  const Result<ScenarioDocument> baseline = parse_scenario(kFullScenario);
  REP_REQUIRE_OK(baseline);
  const std::string zeros(64, '0');
  const std::string ones(64, '1');

  std::vector<std::string> lines = full_scenario_lines();
  lines[kCompositionHeaderLine - 1] =
      std::string(lines[kCompositionHeaderLine - 1]) + " digest=" + zeros;
  const Result<ScenarioDocument> wrong_stream =
      parse_scenario(join_lines(lines));
  REP_REQUIRE(!wrong_stream.ok());
  REP_CHECK(wrong_stream.error().code == ErrorCode::DigestMismatch);
  REP_CHECK(contains(wrong_stream.error().message, "payload digest does not match"));

  // The positive control: the same key carrying the digest the payload really
  // has is accepted, and produces the same document as omitting the key.
  const EvidenceRecord* composition = find_record(baseline.value(), EvidenceKind::RackComposition,
                                                  "rack-A.comp");
  REP_REQUIRE(composition != nullptr);
  const Digest honest = evidence_payload_digest(composition->kind, composition->payload);
  lines = full_scenario_lines();
  lines[kCompositionHeaderLine - 1] =
      std::string(lines[kCompositionHeaderLine - 1]) + " digest=" + honest.to_hex();
  const Result<ScenarioDocument> matching = parse_scenario(join_lines(lines));
  REP_REQUIRE_OK(matching);
  REP_CHECK(matching.value() == baseline.value());

  lines = full_scenario_lines();
  lines[kCandidateLine - 1] =
      std::string(lines[kCandidateLine - 1]) + " claimed_digest=" + ones;
  const Result<ScenarioDocument> wrong_candidate = parse_scenario(join_lines(lines));
  REP_REQUIRE(!wrong_candidate.ok());
  REP_CHECK(wrong_candidate.error().code == ErrorCode::DigestMismatch);
  REP_CHECK(contains(wrong_candidate.error().message, "declaration digest"));

  const EvidenceRecord* offers =
      find_record(baseline.value(), EvidenceKind::CandidateOffers, "offers");
  REP_REQUIRE(offers != nullptr);
  const auto& payload = std::get<CandidateOffersPayload>(offers->payload);
  const Candidate* candidate = payload.find(CandidateId{std::string("c1")});
  REP_REQUIRE(candidate != nullptr);
  lines = full_scenario_lines();
  lines[kCandidateLine - 1] = std::string(lines[kCandidateLine - 1]) +
                              " claimed_digest=" + candidate->evidence_digest.to_hex();
  const Result<ScenarioDocument> honest_candidate = parse_scenario(join_lines(lines));
  REP_REQUIRE_OK(honest_candidate);
  REP_CHECK(honest_candidate.value() == baseline.value());
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

REP_TEST(Scenario, load_scenario_file_reports_missing_files_and_reads_real_ones) {
  const std::filesystem::path missing("rep-scenario-that-does-not-exist.txt");
  const Result<ScenarioDocument> absent = load_scenario_file(missing);
  REP_REQUIRE(!absent.ok());
  REP_CHECK(absent.error().code != ErrorCode::Ok);
  REP_CHECK(absent.error().code == ErrorCode::NotFound ||
            absent.error().code == ErrorCode::IoError);

  const std::filesystem::path directory = std::filesystem::temp_directory_path() /
                                          "rep-scenario-file-test";
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::filesystem::create_directories(directory, ignored);
  REP_REQUIRE(std::filesystem::exists(directory));

  const std::filesystem::path path = directory / "scenario.txt";
  {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REP_REQUIRE(stream.good());
    stream.write(kFullScenario, static_cast<std::streamsize>(
                                    std::char_traits<char>::length(kFullScenario)));
    REP_REQUIRE(stream.good());
  }

  const Result<ScenarioDocument> loaded = load_scenario_file(path);
  REP_REQUIRE_OK(loaded);
  const Result<ScenarioDocument> parsed = parse_scenario(kFullScenario);
  REP_REQUIRE_OK(parsed);
  REP_CHECK(loaded.value() == parsed.value());

  const std::filesystem::path bom_path = directory / "scenario-bom.txt";
  {
    std::ofstream stream(bom_path, std::ios::binary | std::ios::trunc);
    REP_REQUIRE(stream.good());
    stream.write("\xef\xbb\xbf", 3);
    stream.write(kFullScenario, static_cast<std::streamsize>(
                                    std::char_traits<char>::length(kFullScenario)));
    REP_REQUIRE(stream.good());
  }
  const Result<ScenarioDocument> loaded_bom = load_scenario_file(bom_path);
  REP_REQUIRE_OK(loaded_bom);
  REP_CHECK(loaded_bom.value() == parsed.value());

  std::filesystem::remove_all(directory, ignored);
}

REP_TEST_MAIN()
