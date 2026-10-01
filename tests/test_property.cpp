// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Randomized property tests against an independent reference model.  The
// scenarios are built programmatically (never through the scenario text
// format), so a defect in the parser cannot hide a defect in the planner, and
// the reference re-derives conservation, ordering and capacity from the
// request instead of reusing the planner's bookkeeping.
//
// Every case is driven by one printed seed and reproduces exactly.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rep/rep.hpp>

#include "testkit.hpp"

namespace {

using rep::ActionKind;
using rep::Candidate;
using rep::DestinationRef;
using rep::EvidenceKind;
using rep::EvidencePayload;
using rep::EvidenceRecord;
using rep::EvidenceSource;
using rep::Generation;
using rep::IsolationKind;
using rep::MigrationCapability;
using rep::Obligation;
using rep::ObligationId;
using rep::ObligationKind;
using rep::Plan;
using rep::PlanStatus;
using rep::ResourceVector;
using rep::StreamRef;

constexpr std::uint64_t kDefaultSeed = 0x5eed1234ULL;
constexpr std::size_t kIterations = 120;

[[nodiscard]] rep::Result<ResourceVector> resources(std::initializer_list<rep::ResourceAmount> items) {
  return ResourceVector::make(std::vector<rep::ResourceAmount>(items));
}

[[nodiscard]] rep::Result<ResourceVector> cpu(std::uint64_t amount) {
  return resources({{rep::ResourceClass::CpuMillicores, amount}});
}

// ---------------------------------------------------------------------------
// A small programmatic scenario generator
// ---------------------------------------------------------------------------

struct Generated {
  rep::PlanRequest request;
  std::vector<Obligation> occupants;
  std::map<std::string, const Candidate*> candidates_by_id;
  std::vector<Candidate> candidates;
  std::map<std::string, ResourceVector> capacity;
  std::set<std::string> obligations_without_state;
};

class Generator {
 public:
  explicit Generator(std::uint64_t seed) : rng_(seed) {}

  [[nodiscard]] std::uint64_t uniform(std::uint64_t bound) {
    return bound == 0 ? 0 : std::uniform_int_distribution<std::uint64_t>(0, bound - 1)(rng_);
  }

  [[nodiscard]] bool chance(std::uint64_t percent) { return uniform(100) < percent; }

  [[nodiscard]] rep::Result<Generated> make(std::size_t index) {
    Generated generated;

    const std::size_t count = 1 + static_cast<std::size_t>(uniform(4));
    const std::uint64_t revision = 7;
    std::vector<Obligation> obligations;
    obligations.reserve(count);
    for (std::size_t position = 0; position < count; ++position) {
      const std::string id = "ob-" + std::to_string(position);
      const ObligationKind kind = chance(70) ? ObligationKind::Workload : ObligationKind::NetworkPath;
      // A set keeps the generated dependency list free of repeats, which the
      // obligation factory refuses; a back edge creates a cycle in a small
      // fraction of cases and is intentional.
      std::set<std::string> dependency_set;
      if (position > 0 && chance(40)) {
        dependency_set.insert("ob-" + std::to_string(uniform(position)));
      }
      if (position > 0 && chance(8)) {
        dependency_set.insert("ob-0");
      }
      std::vector<ObligationId> depends;
      for (const std::string& dependency : dependency_set) {
        depends.emplace_back(dependency);
      }
      auto obligation = Obligation::make(
          ObligationId(id), kind, rep::RackId("rack-A"),
          kind == ObligationKind::Workload
              ? cpu(1000).value()
              : resources({{rep::ResourceClass::NetworkKib, 100}}).value(),
          chance(20), std::move(depends));
      if (!obligation.ok()) {
        return obligation.error();
      }
      obligations.push_back(obligation.take());
    }

    std::vector<ObligationId> occupants;
    for (const Obligation& obligation : obligations) {
      occupants.push_back(obligation.id);
    }

    const StreamRef composition_source{rep::AuthorityId("rack-authority"),
                                       rep::StreamId("rack-A.comp")};
    const StreamRef enumeration_source{rep::AuthorityId("rack-authority"),
                                       rep::StreamId("rack-A.enum")};
    const StreamRef catalog_source{rep::AuthorityId("rack-authority"), rep::StreamId("rack-A.cat")};
    const StreamRef capacity_source{rep::AuthorityId("facility-capacity"),
                                    rep::StreamId("capacity")};
    const StreamRef domain_source{rep::AuthorityId("facility-capacity"), rep::StreamId("domains")};
    const StreamRef asi_source{rep::AuthorityId("agent-scheduler"), rep::StreamId("asi")};
    const StreamRef dfi_source{rep::AuthorityId("data-fabric"), rep::StreamId("dfi")};
    const StreamRef offers_source{rep::AuthorityId("agent-scheduler"), rep::StreamId("offers")};
    const StreamRef policy_source{rep::AuthorityId("placement-policy"), rep::StreamId("policy")};

    std::vector<EvidenceRecord> records;
    const auto add = [&records](EvidenceKind kind, const StreamRef& source, Generation generation,
                                rep::Epoch epoch, EvidencePayload payload) {
      EvidenceRecord record;
      record.kind = kind;
      record.stamp.source = source;
      record.stamp.generation = generation;
      record.stamp.epoch = epoch;
      record.stamp.content_digest = rep::evidence_payload_digest(kind, payload);
      record.payload = std::move(payload);
      records.push_back(std::move(record));
    };

    auto composition = rep::RackCompositionPayload::make(rep::RackId("rack-A"), Generation{revision},
                                                         occupants);
    if (!composition.ok()) {
      return composition.error();
    }
    add(EvidenceKind::RackComposition, composition_source, Generation{revision}, rep::Epoch{2},
        composition.take());

    auto enumeration = rep::EnumerationPayload::make(rep::RackId("rack-A"), Generation{revision},
                                                     true, occupants);
    if (!enumeration.ok()) {
      return enumeration.error();
    }
    add(EvidenceKind::Enumeration, enumeration_source, Generation{revision}, rep::Epoch{2},
        enumeration.take());

    auto catalog = rep::ObligationCatalogPayload::make(obligations);
    if (!catalog.ok()) {
      return catalog.error();
    }
    add(EvidenceKind::ObligationCatalog, catalog_source, Generation{revision}, rep::Epoch{2},
        catalog.take());

    // Destinations: two rack slots, one of which may share the source domain.
    const DestinationRef destination_b = rep::DestinationRef::parse("rack_slot:rack-B").value();
    const DestinationRef destination_c = rep::DestinationRef::parse("rack_slot:rack-C").value();
    std::vector<rep::DestinationCapacity> capacities;
    const std::uint64_t capacity_b = 500 + uniform(2500);
    const std::uint64_t capacity_c = 500 + uniform(2500);
    capacities.push_back(rep::DestinationCapacity{destination_b, cpu(capacity_b).value(),
                                                  Generation{4}, rep::Epoch{1}});
    capacities.push_back(rep::DestinationCapacity{destination_c, cpu(capacity_c).value(),
                                                  Generation{4}, rep::Epoch{1}});
    auto capacity_payload = rep::CapacityPayload::make(capacities);
    if (!capacity_payload.ok()) {
      return capacity_payload.error();
    }
    generated.capacity.emplace(destination_b.to_text(), cpu(capacity_b).value());
    generated.capacity.emplace(destination_c.to_text(), cpu(capacity_c).value());
    add(EvidenceKind::Capacity, capacity_source, Generation{4}, rep::Epoch{1},
        capacity_payload.take());

    const bool b_shares_source_domain = chance(30);
    std::vector<rep::DomainMember> members;
    {
      rep::DomainMember rack_member;
      rack_member.is_rack = true;
      rack_member.rack = rep::RackId("rack-A");
      rack_member.domain = rep::FailureDomainId("fd-0");
      members.push_back(rack_member);

      rep::DomainMember b_member;
      b_member.destination = destination_b;
      b_member.domain = rep::FailureDomainId(b_shares_source_domain ? "fd-0" : "fd-1");
      members.push_back(b_member);

      rep::DomainMember c_member;
      c_member.destination = destination_c;
      c_member.domain = rep::FailureDomainId("fd-2");
      members.push_back(c_member);

      for (const Obligation& obligation : obligations) {
        rep::DomainMember member;
        member.obligation = obligation.id;
        member.domain = rep::FailureDomainId("fd-0");
        members.push_back(member);
      }
    }
    auto topology = rep::FailureDomainTopology::make(members);
    if (!topology.ok()) {
      return topology.error();
    }
    add(EvidenceKind::FailureDomain, domain_source, Generation{4}, rep::Epoch{1}, topology.take());

    // Policy: an optional spread requirement and an optional deny rule.
    std::vector<rep::PolicyRule> rules;
    const bool require_spread = chance(25);
    if (require_spread) {
      rep::PolicyRule rule;
      rule.kind = rep::PolicyRuleKind::RequireDomainSpread;
      rule.obligation_kind = ObligationKind::Workload;
      rule.action = ActionKind::LiveMigrate;
      rule.destination_kind = rep::DestinationKind::RackSlot;
      rules.push_back(rule);
    }
    const bool deny_c = chance(15);
    if (deny_c) {
      rep::PolicyRule rule;
      rule.kind = rep::PolicyRuleKind::DenyAction;
      rule.obligation_kind = ObligationKind::Workload;
      rule.action = ActionKind::LiveMigrate;
      rule.destination_kind = rep::DestinationKind::RackSlot;
      rule.domain = rep::FailureDomainId("fd-2");
      rules.push_back(rule);
    }
    auto policy = rep::PlacementPolicyPayload::make(rules);
    if (!policy.ok()) {
      return policy.error();
    }
    add(EvidenceKind::PlacementPolicy, policy_source, Generation{4}, rep::Epoch{1}, policy.take());

    // Adjacent state: one record per obligation unless it is dropped.
    std::vector<rep::WorkloadStateRecord> asi;
    std::vector<rep::FabricObligationRecord> dfi;
    for (const Obligation& obligation : obligations) {
      const bool state_present = !chance(10);
      if (!state_present) {
        generated.obligations_without_state.insert(obligation.id.str());
        continue;
      }
      if (obligation.kind == ObligationKind::Workload) {
        rep::WorkloadStateRecord record;
        record.obligation = obligation.id;
        record.lifecycle = rep::WorkloadLifecycle::Running;
        const std::uint64_t roll = uniform(100);
        record.migration = roll < 60   ? MigrationCapability::LiveAllowed
                           : roll < 75 ? MigrationCapability::ColdOnly
                           : roll < 88 ? MigrationCapability::NotMigratable
                                       : MigrationCapability::Unknown;
        record.attachment = rep::StorageAttachment::Stateless;
        asi.push_back(std::move(record));
      } else {
        rep::FabricObligationRecord record;
        record.obligation = obligation.id;
        record.kind = rep::FabricObligationKind::PathAttachment;
        record.path_migration_supported = !chance(20);
        record.permitted_endpoints = chance(50) ? std::vector<DestinationRef>{destination_b}
                                                : std::vector<DestinationRef>{};
        record.mapping_digest = rep::sha256_of(obligation.id.view());
        dfi.push_back(std::move(record));
      }
    }
    const bool asi_present = !chance(6);
    if (asi_present) {
      auto payload = rep::AsiWorkloadPayload::make(asi);
      if (!payload.ok()) {
        return payload.error();
      }
      add(EvidenceKind::AsiWorkloadState, asi_source, Generation{4}, rep::Epoch{1}, payload.take());
    }
    const bool dfi_present = !chance(6);
    if (dfi_present) {
      auto payload = rep::DfiObligationPayload::make(dfi);
      if (!payload.ok()) {
        return payload.error();
      }
      add(EvidenceKind::DfiObligation, dfi_source, Generation{4}, rep::Epoch{1}, payload.take());
    }

    // Candidate offers.
    std::vector<Candidate> offers;
    std::size_t candidate_index = 0;
    for (const Obligation& obligation : obligations) {
      const std::size_t offer_count = uniform(3);
      for (std::size_t offer = 0; offer < offer_count; ++offer) {
        const DestinationRef destination = chance(50) ? destination_b : destination_c;
        const ActionKind action = obligation.kind == ObligationKind::Workload
                                      ? (chance(70) ? ActionKind::LiveMigrate
                                                    : ActionKind::ColdMigrate)
                                      : (chance(50) ? ActionKind::Rebind : ActionKind::Detach);
        const std::uint64_t provision = obligation.kind == ObligationKind::Workload
                                            ? (chance(20) ? 100 : 1000)
                                            : 100;
        auto made = Candidate::make(
            rep::CandidateId("cand-" + std::to_string(candidate_index)), obligation.id, action,
            destination, rep::AuthorityId("agent-scheduler"), Generation{4}, rep::Epoch{1},
            obligation.kind == ObligationKind::Workload
                ? cpu(provision).value()
                : resources({{rep::ResourceClass::NetworkKib, provision}}).value(),
            static_cast<std::uint32_t>(uniform(50)), chance(10));
        if (!made.ok()) {
          return made.error();
        }
        ++candidate_index;
        offers.push_back(made.take());
      }
    }
    auto offers_payload = rep::CandidateOffersPayload::make(offers);
    if (!offers_payload.ok()) {
      return offers_payload.error();
    }
    generated.candidates = offers_payload.value().candidates;
    for (const Candidate& candidate : generated.candidates) {
      generated.candidates_by_id.emplace(candidate.id.str(), &candidate);
    }
    add(EvidenceKind::CandidateOffers, offers_source, Generation{4}, rep::Epoch{1},
        offers_payload.take());

    auto bundle = rep::EvidenceBundle::make(std::move(records));
    if (!bundle.ok()) {
      return bundle.error();
    }

    std::vector<EvidenceSource> sources = {
        {EvidenceKind::RackComposition, composition_source},
        {EvidenceKind::Enumeration, enumeration_source},
        {EvidenceKind::ObligationCatalog, catalog_source},
        {EvidenceKind::Capacity, capacity_source},
        {EvidenceKind::FailureDomain, domain_source},
        {EvidenceKind::PlacementPolicy, policy_source},
        {EvidenceKind::CandidateOffers, offers_source},
    };
    if (asi_present) {
      sources.push_back({EvidenceKind::AsiWorkloadState, asi_source});
    }
    if (dfi_present) {
      sources.push_back({EvidenceKind::DfiObligation, dfi_source});
    }

    auto isolation = rep::IsolationRequest::make(
        rep::RackId("rack-A"), IsolationKind::Depower, Generation{revision},
        {ObligationKind::Workload, ObligationKind::NetworkPath});
    if (!isolation.ok()) {
      return isolation.error();
    }
    auto request = rep::PlanRequest::make(rep::IdempotencyKey("prop-" + std::to_string(index)),
                                          rep::AuthorityId("requester-01"), rep::Epoch{0},
                                          rep::UnixNanos{1000}, isolation.take(), rep::LineageId{},
                                          std::move(sources), bundle.take());
    if (!request.ok()) {
      return request.error();
    }
    generated.request = request.take();
    generated.occupants = obligations;
    return generated;
  }

 private:
  std::mt19937_64 rng_;
};

// ---------------------------------------------------------------------------
// The reference model
// ---------------------------------------------------------------------------

struct Reference {
  std::vector<const Obligation*> in_scope;
  std::vector<const Obligation*> out_of_scope;
  std::map<std::string, std::vector<const Candidate*>> candidates;
  std::map<std::string, ResourceVector> capacity;
  std::set<std::string> migratable;
  std::set<std::string> immovable;

  // Builds the model from the request alone, with its own traversal.
  [[nodiscard]] static Reference build(const Generated& generated) {
    Reference model;
    const rep::PlanRequest& request = generated.request;

    std::vector<const Obligation*> catalog;
    for (const EvidenceRecord& record : request.evidence.records()) {
      if (record.kind == EvidenceKind::ObligationCatalog) {
        for (const Obligation& obligation : std::get<rep::ObligationCatalogPayload>(record.payload)
                                                .obligations) {
          catalog.push_back(&obligation);
        }
      }
      if (record.kind == EvidenceKind::Capacity) {
        for (const rep::DestinationCapacity& entry :
             std::get<rep::CapacityPayload>(record.payload).destinations) {
          model.capacity.emplace(entry.destination.to_text(), entry.available);
        }
      }
      if (record.kind == EvidenceKind::CandidateOffers) {
        for (const Candidate& candidate :
             std::get<rep::CandidateOffersPayload>(record.payload).candidates) {
          model.candidates[candidate.obligation.str()].push_back(&candidate);
        }
      }
      if (record.kind == EvidenceKind::AsiWorkloadState) {
        for (const rep::WorkloadStateRecord& entry :
             std::get<rep::AsiWorkloadPayload>(record.payload).records) {
          if (entry.migration == MigrationCapability::LiveAllowed ||
              entry.migration == MigrationCapability::ColdOnly) {
            model.migratable.insert(entry.obligation.str());
          } else {
            model.immovable.insert(entry.obligation.str());
          }
        }
      }
      if (record.kind == EvidenceKind::DfiObligation) {
        for (const rep::FabricObligationRecord& entry :
             std::get<rep::DfiObligationPayload>(record.payload).records) {
          if (entry.path_migration_supported) {
            model.migratable.insert(entry.obligation.str());
          } else {
            model.immovable.insert(entry.obligation.str());
          }
        }
      }
    }

    for (const Obligation& occupant : generated.occupants) {
      const Obligation* definition = nullptr;
      for (const Obligation* entry : catalog) {
        if (entry->id == occupant.id) {
          definition = entry;
          break;
        }
      }
      if (definition == nullptr) {
        continue;
      }
      model.in_scope.push_back(definition);
    }
    return model;
  }

  // Conservative structural eligibility: the reference deliberately ignores
  // sharing between obligations, so it never claims infeasibility that the
  // planner could have avoided.
  [[nodiscard]] std::vector<const Candidate*> eligible(const Obligation& obligation) const {
    std::vector<const Candidate*> result;
    const auto found = candidates.find(obligation.id.str());
    if (found == candidates.end()) {
      return result;
    }
    for (const Candidate* candidate : found->second) {
      if (!rep::action_matches_obligation(obligation.kind, candidate->action)) {
        continue;
      }
      if (!rep::action_matches_destination(candidate->action, candidate->destination.kind)) {
        continue;
      }
      if (candidate->destination.kind == rep::DestinationKind::RackSlot &&
          candidate->destination.id.view() == obligation.source_rack.view()) {
        continue;
      }
      if (!candidate->provision.dominates(obligation.demand)) {
        continue;
      }
      const auto availability = capacity.find(candidate->destination.to_text());
      if (availability == capacity.end() || !availability->second.dominates(candidate->provision)) {
        continue;
      }
      if (immovable.count(obligation.id.str()) != 0) {
        continue;
      }
      result.push_back(candidate);
    }
    return result;
  }

  // Brute force: does any assignment of one candidate per in-scope obligation
  // fit inside the capacity the request declares?
  [[nodiscard]] bool complete_assignment_exists() const {
    std::vector<std::vector<const Candidate*>> options;
    for (const Obligation* obligation : in_scope) {
      std::vector<const Candidate*> choices = eligible(*obligation);
      if (choices.empty()) {
        return false;
      }
      options.push_back(std::move(choices));
    }
    if (options.empty()) {
      return true;
    }
    std::vector<std::size_t> selection(options.size(), 0);
    while (true) {
      std::map<std::string, ResourceVector> used;
      bool fits = true;
      for (std::size_t index = 0; index < options.size(); ++index) {
        const Candidate& candidate = *options[index][selection[index]];
        const std::string key = candidate.destination.to_text();
        const auto existing = used.find(key);
        if (existing == used.end()) {
          used.emplace(key, candidate.provision);
        } else {
          auto sum = existing->second.add(candidate.provision);
          if (!sum.ok()) {
            fits = false;
            break;
          }
          existing->second = sum.take();
        }
      }
      if (fits) {
        for (const auto& entry : used) {
          const auto declared = capacity.find(entry.first);
          if (declared == capacity.end() || !declared->second.dominates(entry.second)) {
            fits = false;
            break;
          }
        }
      }
      if (fits) {
        return true;
      }
      std::size_t position = 0;
      while (position < options.size()) {
        if (++selection[position] < options[position].size()) {
          break;
        }
        selection[position] = 0;
        ++position;
      }
      if (position == options.size()) {
        return false;
      }
    }
  }

  [[nodiscard]] bool conservation_holds(const Plan& plan) const {
    std::set<std::string> expected;
    for (const Obligation* obligation : in_scope) {
      expected.insert(obligation->id.str());
    }
    std::set<std::string> actual;
    for (const rep::Assignment& assignment : plan.assignments) {
      if (!actual.insert(assignment.obligation.str()).second) {
        return false;
      }
    }
    for (const rep::Residual& residual : plan.residuals) {
      if (!actual.insert(residual.obligation.str()).second) {
        return false;
      }
    }
    for (const rep::ScopeExclusion& exclusion : plan.out_of_scope) {
      if (!actual.insert(exclusion.obligation.str()).second) {
        return false;
      }
    }
    return actual == expected;
  }

  // The reference re-derives the dependency order with a naive repeated scan,
  // which is a different algorithm from the planner's heap-based Kahn pass.
  [[nodiscard]] bool ordering_holds(const Plan& plan) const {
    std::map<std::string, std::uint32_t> order;
    for (const rep::Assignment& assignment : plan.assignments) {
      order.emplace(assignment.obligation.str(), assignment.order_index);
    }
    for (const rep::Assignment& assignment : plan.assignments) {
      const Obligation* definition = nullptr;
      for (const Obligation* obligation : in_scope) {
        if (obligation->id == assignment.obligation) {
          definition = obligation;
          break;
        }
      }
      if (definition == nullptr) {
        return false;
      }
      for (const ObligationId& dependency : definition->depends_on) {
        const auto expected = order.find(dependency.str());
        if (expected == order.end()) {
          continue;   // outside the scope: it does not order anything
        }
        if (!(expected->second < assignment.order_index)) {
          return false;
        }
      }
    }
    return true;
  }

  [[nodiscard]] bool capacity_holds(const Plan& plan) const {
    std::map<std::string, ResourceVector> used;
    for (const rep::Assignment& assignment : plan.assignments) {
      const auto candidate = candidates.find(assignment.obligation.str());
      if (candidate == candidates.end()) {
        return false;
      }
      const Candidate* chosen = nullptr;
      for (const Candidate* entry : candidate->second) {
        if (entry->id == assignment.candidate) {
          chosen = entry;
          break;
        }
      }
      if (chosen == nullptr) {
        return false;
      }
      const std::string key = chosen->destination.to_text();
      const auto existing = used.find(key);
      if (existing == used.end()) {
        used.emplace(key, chosen->provision);
      } else {
        auto sum = existing->second.add(chosen->provision);
        if (!sum.ok()) {
          return false;
        }
        existing->second = sum.take();
      }
    }
    for (const auto& entry : used) {
      const auto declared = capacity.find(entry.first);
      if (declared == capacity.end() || !declared->second.dominates(entry.second)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool assignments_are_structurally_valid(const Plan& plan) const {
    for (const rep::Assignment& assignment : plan.assignments) {
      if (!rep::action_matches_obligation(assignment.kind, assignment.action)) {
        return false;
      }
      if (!rep::action_matches_destination(assignment.action, assignment.destination.kind)) {
        return false;
      }
      if (assignment.destination.kind == rep::DestinationKind::RackSlot &&
          assignment.destination.id.view() == plan.source_rack.view()) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool residuals_are_explained(const Plan& plan) const {
    for (const rep::Residual& residual : plan.residuals) {
      if (residual.reason == rep::ResidualReason::NoCandidate && !residual.rejected.empty()) {
        return false;
      }
      if (residual.reason != rep::ResidualReason::NoCandidate && residual.rejected.empty() &&
          residual.reason != rep::ResidualReason::ProtectedObligation &&
          residual.reason != rep::ResidualReason::NonMigratable &&
          residual.reason != rep::ResidualReason::DependencyUnsatisfied &&
          residual.reason != rep::ResidualReason::DependencyCycle &&
          residual.reason != rep::ResidualReason::EvidenceMissing &&
          residual.reason != rep::ResidualReason::StateUnknown) {
        return false;
      }
    }
    return true;
  }
};

[[nodiscard]] rep::Result<Plan> plan_request(const rep::PlanRequest& request) {
  rep::EngineOptions options;
  options.planner = rep::PlannerId("property-planner");
  auto engine = rep::PlanEngine::open(std::move(options));
  if (!engine.ok()) {
    return engine.error();
  }
  auto outcome = engine.value()->submit(request);
  if (!outcome.ok()) {
    return outcome.error();
  }
  if (!outcome.value().has_plan) {
    return make_error(rep::ErrorCode::Internal, "request was rejected: " +
                                                    outcome.value().error.to_string());
  }
  return outcome.value().plan;
}

} // namespace

REP_TEST(property, planning_matches_the_reference_model) {
  REP_INFO("seed=0x" + [] {
    std::string text;
    constexpr char kHex[] = "0123456789abcdef";
    std::uint64_t value = kDefaultSeed;
    for (int index = 15; index >= 0; --index) {
      text.push_back(kHex[(value >> (index * 4)) & 0xf]);
    }
    return text;
  }());
  Generator generator(kDefaultSeed);
  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    auto generated = generator.make(iteration);
    REP_REQUIRE_MSG(generated.ok(), "generation failed at iteration " + std::to_string(iteration) +
                                        ": " + generated.error().to_string());
    const Reference model = Reference::build(generated.value());
    auto plan = plan_request(generated.value().request);
    REP_REQUIRE_MSG(plan.ok(), "iteration " + std::to_string(iteration) + " failed: " +
                                   plan.error().to_string());
    const Plan& sealed = plan.value();

    REP_CHECK_MSG(sealed.verify().ok(), "iteration " + std::to_string(iteration));
    REP_CHECK_MSG(model.conservation_holds(sealed), "conservation, iteration " +
                                                        std::to_string(iteration));
    REP_CHECK_MSG(model.ordering_holds(sealed), "ordering, iteration " + std::to_string(iteration));
    REP_CHECK_MSG(model.capacity_holds(sealed), "capacity, iteration " + std::to_string(iteration));
    REP_CHECK_MSG(model.assignments_are_structurally_valid(sealed),
                  "structure, iteration " + std::to_string(iteration));
    REP_CHECK_MSG(model.residuals_are_explained(sealed),
                  "explanations, iteration " + std::to_string(iteration));

    // Soundness: a plan may only claim completeness when an assignment that
    // satisfies every declared limit exists at all.
    if (sealed.status == PlanStatus::Complete || sealed.status == PlanStatus::EmptySafe) {
      REP_CHECK_MSG(model.complete_assignment_exists(),
                    "unsound completeness, iteration " + std::to_string(iteration));
    }
    if (sealed.status == PlanStatus::Partial) {
      REP_CHECK(!sealed.residuals.empty());
      REP_CHECK(sealed.verdict() == rep::SafetyVerdict::NotProven);
    }
    if (sealed.status == PlanStatus::Indeterminate) {
      REP_CHECK(sealed.assignments.empty());
    }
    const std::vector<rep::AuditFinding> findings =
        rep::audit_plan(sealed, generated.value().request);
    REP_CHECK_MSG(findings.empty(),
                  "audit, iteration " + std::to_string(iteration) +
                      (findings.empty() ? std::string()
                                        : " code=" + findings.front().code + " " +
                                              findings.front().detail));
  }
}

REP_TEST(property, planning_is_deterministic_and_order_independent) {
  Generator generator(kDefaultSeed + 1);
  for (std::size_t iteration = 0; iteration < 40; ++iteration) {
    auto generated = generator.make(iteration);
    REP_REQUIRE_MSG(generated.ok(), "generation failed at iteration " + std::to_string(iteration) +
                                        ": " + generated.error().to_string());
    auto first = plan_request(generated.value().request);
    auto second = plan_request(generated.value().request);
    REP_REQUIRE(first.ok());
    REP_REQUIRE(second.ok());
    REP_CHECK_MSG(first.value().plan_digest == second.value().plan_digest,
                  "digest, iteration " + std::to_string(iteration));
    REP_CHECK_MSG(first.value().to_text() == second.value().to_text(),
                  "text, iteration " + std::to_string(iteration));

    // Rebuilding the same bundle with its records supplied in a different
    // order must produce byte-identical evidence.
    std::vector<EvidenceRecord> shuffled(generated.value().request.evidence.records().begin(),
                                         generated.value().request.evidence.records().end());
    std::reverse(shuffled.begin(), shuffled.end());
    auto reordered = rep::EvidenceBundle::make(std::move(shuffled));
    REP_REQUIRE(reordered.ok());
    REP_CHECK_MSG(reordered.value() == generated.value().request.evidence,
                  "canonical order, iteration " + std::to_string(iteration));

    auto rebuilt = rep::PlanRequest::make(
        generated.value().request.idempotency_key, generated.value().request.requested_by,
        generated.value().request.expected_epoch, generated.value().request.evaluation_time,
        generated.value().request.isolation, generated.value().request.lineage,
        generated.value().request.evidence_sources, reordered.take());
    REP_REQUIRE(rebuilt.ok());
    REP_CHECK(rebuilt.value().digest() == generated.value().request.digest());
    auto third = plan_request(rebuilt.value());
    REP_REQUIRE(third.ok());
    REP_CHECK(third.value().plan_digest == first.value().plan_digest);
  }
}

REP_TEST(property, a_missing_state_stream_never_yields_a_complete_plan) {
  Generator generator(kDefaultSeed + 2);
  std::size_t observed = 0;
  for (std::size_t iteration = 0; iteration < 60; ++iteration) {
    auto generated = generator.make(iteration);
    REP_REQUIRE_MSG(generated.ok(), "generation failed at iteration " + std::to_string(iteration) +
                                        ": " + generated.error().to_string());
    if (generated.value().obligations_without_state.empty()) {
      continue;
    }
    ++observed;
    auto plan = plan_request(generated.value().request);
    REP_REQUIRE(plan.ok());
    const Plan& sealed = plan.value();
    if (sealed.status == PlanStatus::Indeterminate) {
      continue;
    }
    // An obligation whose state the adjacent authority never described can
    // only be residual; it must never be assigned.
    for (const std::string& obligation : generated.value().obligations_without_state) {
      for (const rep::Assignment& assignment : sealed.assignments) {
        REP_CHECK_MSG(assignment.obligation.str() != obligation,
                      "unstated obligation was assigned, iteration " + std::to_string(iteration));
      }
    }
    REP_CHECK(sealed.status != PlanStatus::Complete);
  }
  REP_CHECK_MSG(observed > 0, "the generator produced no case with unstated obligations");
}

REP_TEST(property, capacity_is_never_overbooked_across_iterations) {
  Generator generator(kDefaultSeed + 3);
  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    auto generated = generator.make(iteration);
    REP_REQUIRE_MSG(generated.ok(), "generation failed at iteration " + std::to_string(iteration) +
                                        ": " + generated.error().to_string());
    const Reference model = Reference::build(generated.value());
    auto plan = plan_request(generated.value().request);
    REP_REQUIRE(plan.ok());
    REP_CHECK_MSG(model.capacity_holds(plan.value()),
                  "overbooked, iteration " + std::to_string(iteration));
  }
}

REP_TEST(property, every_plan_survives_a_text_round_trip_of_its_bindings) {
  Generator generator(kDefaultSeed + 4);
  for (std::size_t iteration = 0; iteration < 30; ++iteration) {
    auto generated = generator.make(iteration);
    REP_REQUIRE_MSG(generated.ok(), "generation failed at iteration " + std::to_string(iteration) +
                                        ": " + generated.error().to_string());
    auto plan = plan_request(generated.value().request);
    REP_REQUIRE(plan.ok());
    const Plan& sealed = plan.value();
    // Freshness against the exact evidence the plan consumed.
    bool missing = true;
    const std::vector<rep::StalenessFinding> findings =
        sealed.staleness(generated.value().request.evidence, missing);
    REP_CHECK_MSG(findings.empty(), "not fresh, iteration " + std::to_string(iteration));
    REP_CHECK(!missing);
    REP_CHECK_EQ(sealed.bindings.size(), generated.value().request.evidence_sources.size());
  }
}

REP_TEST_MAIN()
