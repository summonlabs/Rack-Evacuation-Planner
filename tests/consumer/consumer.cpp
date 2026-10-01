// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Downstream consumer of the installed Rack Evacuation Planner package.
//
// The program builds one small, internally consistent evacuation scenario out
// of the public factories, submits it to a non-durable in-memory engine, and
// reports what came back.  It exits 0 only when the submission was committed,
// the plan covers everything that had to be covered, and the linked library
// agrees with the headers this program was built against.

#include <rep/rep.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int kFailure = 1;

// --------------------------------------------------------------------------
// Scenario facts
// --------------------------------------------------------------------------

constexpr std::string_view kRack = "rack-a1";
constexpr std::string_view kTargetSlot = "rack-b7-slot-3";
constexpr std::string_view kWorkload = "workload-1001";
constexpr std::string_view kOffer = "offer-live-migrate-1";
constexpr std::string_view kPlanner = "planner-a";
constexpr std::string_view kRequester = "facility-change-orchestrator";
constexpr std::string_view kRequestKey = "change-window-42";
constexpr std::string_view kSourceDomain = "domain-a";
constexpr std::string_view kTargetDomain = "domain-b";

constexpr std::uint64_t kCompositionRevision = 7;
constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t kEvaluationTime = 1000000000;

void report(std::string_view what, const rep::Error& error) {
  std::cerr << what << ": " << rep::to_string(error.code) << ": " << error.message << '\n';
}

// Unwraps a factory result, reporting the failure it carries.
template <class T>
[[nodiscard]] std::optional<T> checked(rep::Result<T> result, std::string_view what) {
  if (!result.ok()) {
    report(what, result.error());
    return std::nullopt;
  }
  return result.take();
}

template <class Id>
[[nodiscard]] bool parse_id(std::string_view text, std::string_view what, Id& out) {
  auto parsed = Id::parse(text);
  if (!parsed.ok()) {
    report(what, parsed.error());
    return false;
  }
  out = parsed.take();
  return true;
}

// One adjacent evidence stream: the authority that published it, its name, and
// the generation and epoch it was published at.  A candidate has to name the
// authority of the offers stream it arrived on, at exactly this generation and
// epoch, or the planner refuses to use it.
struct EvidenceStream {
  rep::StreamRef source;
  rep::Generation generation;
  rep::Epoch epoch;
};

[[nodiscard]] std::optional<EvidenceStream> make_stream(std::string_view authority,
                                                        std::string_view name,
                                                        std::uint64_t generation,
                                                        std::uint64_t epoch) {
  EvidenceStream stream;
  if (!parse_id(authority, "evidence authority", stream.source.authority) ||
      !parse_id(name, "evidence stream", stream.source.stream)) {
    return std::nullopt;
  }
  stream.generation = rep::Generation{generation};
  stream.epoch = rep::Epoch{epoch};
  return stream;
}

// Binds a payload into a record.  The stamp digest is computed exactly the way
// the library computes it when the bundle is validated.
template <class Payload>
[[nodiscard]] rep::EvidenceRecord bind(rep::EvidenceKind kind, const EvidenceStream& stream,
                                       Payload payload) {
  rep::EvidenceRecord record;
  record.kind = kind;
  record.stamp.source = stream.source;
  record.stamp.generation = stream.generation;
  record.stamp.epoch = stream.epoch;
  record.payload = std::move(payload);
  record.stamp.content_digest = rep::evidence_payload_digest(kind, record.payload);
  return record;
}

[[nodiscard]] rep::EvidenceSource bind_source(rep::EvidenceKind kind,
                                              const EvidenceStream& stream) {
  return rep::EvidenceSource{kind, stream.source};
}

struct Scenario {
  rep::PlannerId planner;
  rep::IdempotencyKey key;
  rep::AuthorityId requester;
  rep::IsolationRequest isolation;
  std::vector<rep::EvidenceSource> sources;
  rep::EvidenceBundle evidence;
};

[[nodiscard]] std::optional<Scenario> build_scenario() {
  Scenario scenario;
  if (!parse_id(kPlanner, "planner id", scenario.planner) ||
      !parse_id(kRequestKey, "idempotency key", scenario.key) ||
      !parse_id(kRequester, "requesting authority", scenario.requester)) {
    return std::nullopt;
  }

  // One stream per evidence kind, each published by the authority that owns
  // that truth.  The offers stream is the one a candidate must point back at.
  const auto composition_stream =
      make_stream("rack-registry", "rack-composition", kCompositionRevision, 3);
  const auto enumeration_stream =
      make_stream("rack-registry", "rack-enumeration", kCompositionRevision, 3);
  const auto catalog_stream = make_stream("workload-registry", "obligation-catalog", 21, 3);
  const auto capacity_stream = make_stream("facility-capacity", "destination-capacity", 44, 3);
  const auto policy_stream = make_stream("facility-policy", "placement-policy", 5, 3);
  const auto domain_stream = make_stream("failure-domain-registry", "domain-topology", 9, 3);
  const auto asi_stream = make_stream("agent-scheduler", "workload-state", 118, 3);
  const auto offers_stream = make_stream("migration-authority", "candidate-offers", 77, 3);
  if (!composition_stream || !enumeration_stream || !catalog_stream || !capacity_stream ||
      !policy_stream || !domain_stream || !asi_stream || !offers_stream) {
    return std::nullopt;
  }

  rep::RackId rack;
  rep::ObligationId workload;
  rep::DestinationId slot;
  rep::CandidateId offer;
  rep::FailureDomainId source_domain;
  rep::FailureDomainId target_domain;
  if (!parse_id(kRack, "rack id", rack) || !parse_id(kWorkload, "obligation id", workload) ||
      !parse_id(kTargetSlot, "destination id", slot) || !parse_id(kOffer, "candidate id", offer) ||
      !parse_id(kSourceDomain, "failure domain id", source_domain) ||
      !parse_id(kTargetDomain, "failure domain id", target_domain)) {
    return std::nullopt;
  }

  // The destination has to hold at least what the action provisions, and the
  // action has to provision at least what the obligation demands.
  const auto demand = checked(
      rep::ResourceVector::make({rep::ResourceAmount{rep::ResourceClass::CpuMillicores, 4000},
                                 rep::ResourceAmount{rep::ResourceClass::MemoryBytes, 8 * kGiB}}),
      "workload demand");
  if (!demand) {
    return std::nullopt;
  }
  const auto available = checked(
      rep::ResourceVector::make({rep::ResourceAmount{rep::ResourceClass::CpuMillicores, 16000},
                                 rep::ResourceAmount{rep::ResourceClass::MemoryBytes, 64 * kGiB}}),
      "destination capacity");
  if (!available) {
    return std::nullopt;
  }

  const rep::Generation revision{kCompositionRevision};
  const rep::DestinationRef destination{rep::DestinationKind::RackSlot, slot};

  const auto obligation = checked(
      rep::Obligation::make(workload, rep::ObligationKind::Workload, rack, *demand, false, {}),
      "workload obligation");
  if (!obligation) {
    return std::nullopt;
  }

  const auto composition = checked(
      rep::RackCompositionPayload::make(rack, revision, {workload}), "rack composition");
  const auto enumeration = checked(
      rep::EnumerationPayload::make(rack, revision, true, {workload}), "rack enumeration");
  const auto catalog =
      checked(rep::ObligationCatalogPayload::make({*obligation}), "obligation catalog");
  // An empty rule set is a policy decision, not a missing input.
  const auto policy = checked(rep::PlacementPolicyPayload::make({}), "placement policy");
  const auto capacity = checked(
      rep::CapacityPayload::make({rep::DestinationCapacity{
          destination, *available, capacity_stream->generation, capacity_stream->epoch}}),
      "destination capacity");
  // A workload is only migratable when the scheduling authority says so: the
  // planner refuses to move one it cannot prove is live-migratable.
  const auto workload_state = checked(
      rep::AsiWorkloadPayload::make({rep::WorkloadStateRecord{
          workload, rep::WorkloadLifecycle::Running, rep::MigrationCapability::LiveAllowed,
          rep::StorageAttachment::SharedVolume}}),
      "workload state");
  const auto candidate = checked(
      rep::Candidate::make(offer, workload, rep::ActionKind::LiveMigrate, destination,
                           offers_stream->source.authority, offers_stream->generation,
                           offers_stream->epoch, *demand, 10, false),
      "live migration candidate");
  if (!composition || !enumeration || !catalog || !policy || !capacity || !workload_state ||
      !candidate) {
    return std::nullopt;
  }

  const auto offers =
      checked(rep::CandidateOffersPayload::make({*candidate}), "candidate offers");

  rep::DomainMember source_member;
  source_member.domain = source_domain;
  source_member.rack = rack;
  source_member.is_rack = true;
  rep::DomainMember target_member;
  target_member.domain = target_domain;
  target_member.destination = destination;
  const auto domains = checked(rep::FailureDomainTopology::make({source_member, target_member}),
                               "failure domains");
  if (!offers || !domains) {
    return std::nullopt;
  }

  std::vector<rep::EvidenceRecord> records;
  records.push_back(bind(rep::EvidenceKind::RackComposition, *composition_stream, *composition));
  records.push_back(bind(rep::EvidenceKind::Enumeration, *enumeration_stream, *enumeration));
  records.push_back(bind(rep::EvidenceKind::ObligationCatalog, *catalog_stream, *catalog));
  records.push_back(bind(rep::EvidenceKind::Capacity, *capacity_stream, *capacity));
  records.push_back(bind(rep::EvidenceKind::PlacementPolicy, *policy_stream, *policy));
  records.push_back(bind(rep::EvidenceKind::FailureDomain, *domain_stream, *domains));
  records.push_back(bind(rep::EvidenceKind::AsiWorkloadState, *asi_stream, *workload_state));
  records.push_back(bind(rep::EvidenceKind::CandidateOffers, *offers_stream, *offers));

  const auto bundle = checked(rep::EvidenceBundle::make(std::move(records)), "evidence bundle");
  const auto isolation = checked(
      rep::IsolationRequest::make(rack, rep::IsolationKind::Depower, revision,
                                  {rep::ObligationKind::Workload}),
      "isolation request");
  if (!bundle || !isolation) {
    return std::nullopt;
  }

  // Every bound stream is named explicitly: a record that happens to be in the
  // bundle is never used unless the request asked for it.
  scenario.sources = {
      bind_source(rep::EvidenceKind::RackComposition, *composition_stream),
      bind_source(rep::EvidenceKind::Enumeration, *enumeration_stream),
      bind_source(rep::EvidenceKind::ObligationCatalog, *catalog_stream),
      bind_source(rep::EvidenceKind::Capacity, *capacity_stream),
      bind_source(rep::EvidenceKind::PlacementPolicy, *policy_stream),
      bind_source(rep::EvidenceKind::FailureDomain, *domain_stream),
      bind_source(rep::EvidenceKind::AsiWorkloadState, *asi_stream),
      bind_source(rep::EvidenceKind::CandidateOffers, *offers_stream)};
  scenario.evidence = std::move(*bundle);
  scenario.isolation = std::move(*isolation);
  return scenario;
}

} // namespace

int main() {
  const auto scenario = build_scenario();
  if (!scenario) {
    return kFailure;
  }

  // No store directory: the engine keeps its plans in this process and nothing
  // survives a restart.  The wall clock is left unset because plan evaluation
  // uses the request's evaluation time and never a clock of its own.
  rep::EngineOptions options;
  options.planner = scenario->planner;
  auto opened = rep::PlanEngine::open(options);
  if (!opened.ok()) {
    report("engine open", opened.error());
    return kFailure;
  }
  std::unique_ptr<rep::PlanEngine> engine = opened.take();

  const auto request = rep::PlanRequest::make(
      scenario->key, scenario->requester, engine->current_epoch(), rep::UnixNanos{kEvaluationTime},
      scenario->isolation, rep::LineageId{}, scenario->sources, scenario->evidence);
  if (!request.ok()) {
    report("plan request", request.error());
    return kFailure;
  }

  const auto submitted = engine->submit(request.value());
  if (!submitted.ok()) {
    report("submit", submitted.error());
    return kFailure;
  }
  const rep::PlanOutcome& outcome = submitted.value();
  if (outcome.committed()) {
    std::cout << outcome.plan.to_text();
  }
  std::cout << "outcome: " << rep::to_string(outcome.kind) << '\n';
  std::cout << "status: " << rep::to_string(outcome.plan.status) << '\n';
  std::cout << "verdict: " << rep::to_string(outcome.plan.verdict()) << '\n';
  if (!outcome.error.ok()) {
    std::cout << "error: " << outcome.error.to_string() << '\n';
  }

  const bool committed =
      outcome.kind == rep::OutcomeKind::Planned || outcome.kind == rep::OutcomeKind::Replayed;
  const bool covered = outcome.plan.status == rep::PlanStatus::Complete ||
                       outcome.plan.status == rep::PlanStatus::EmptySafe;
  const bool versions_agree = rep::version_is_consistent();
  if (!versions_agree) {
    std::cerr << "headers and library disagree about the version\n";
  }
  return committed && covered && versions_agree ? 0 : kFailure;
}
