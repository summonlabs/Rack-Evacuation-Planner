// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "detail/planner.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "detail/containers.hpp"
#include "rep/adjacent.hpp"
#include "rep/candidate.hpp"
#include "rep/evidence.hpp"
#include "rep/obligation.hpp"
#include "rep/policy.hpp"
#include "rep/types.hpp"

namespace rep::detail {
namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::uint16_t kind_code(ObligationKind kind) noexcept {
  return static_cast<std::uint16_t>(kind);
}

// Canonical ordering key for obligations: kind first, so a plan reads as
// workloads, then data, then fabric, then services, then reservations.
[[nodiscard]] std::pair<std::uint16_t, ObligationId> obligation_key(
    const Obligation& obligation) noexcept {
  return {kind_code(obligation.kind), obligation.id};
}

[[nodiscard]] bool same_id_set(const std::vector<ObligationId>& a,
                               const std::vector<ObligationId>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// Which adjacent state stream an obligation kind depends on.
enum class StateSource : std::uint8_t { None, Asi, Dfi };

[[nodiscard]] StateSource state_source_for(ObligationKind kind) noexcept {
  switch (kind) {
    case ObligationKind::Workload:
    case ObligationKind::Appliance:
      return StateSource::Asi;
    case ObligationKind::StorageReplica:
    case ObligationKind::NetworkPath:
    case ObligationKind::ServiceEndpoint:
      return StateSource::Dfi;
    case ObligationKind::Reservation:
      return StateSource::None;
  }
  return StateSource::None;
}

// ---------------------------------------------------------------------------
// Bound evidence
// ---------------------------------------------------------------------------

struct OfferSource {
  StreamRef source;
  Generation generation;
  Epoch epoch;
};

struct BoundEvidence {
  // Obligation-set layer: only streams the request bound, and only records
  // that are actually present.
  std::vector<const RackCompositionPayload*> compositions;
  std::vector<const EnumerationPayload*> enumerations;
  std::vector<const Obligation*> catalog;
  bool catalog_present{false};

  // Adjacent-authority layer, merged across every bound stream.
  bool capacity_present{false};
  bool policy_present{false};
  bool maintenance_present{false};
  bool domains_present{false};
  bool asi_present{false};
  bool dfi_present{false};
  bool offers_present{false};

  CapacityPayload capacity;
  PlacementPolicyPayload policy;
  MaintenancePayload maintenance;
  FailureDomainTopology domains;
  AsiWorkloadPayload asi;
  DfiObligationPayload dfi;
  CandidateOffersPayload offers;

  std::vector<OfferSource> offer_sources;
  std::vector<PlanBinding> bindings;
};

// Reads every stream the request bound.  A record is used only when the
// request named its stream; a record that happens to be present but unbound is
// never evidence.  Contradictory duplicates across bound streams are refused
// here, because the adjacent layer has no principled way to choose a side.
[[nodiscard]] Result<void> collect_bound_evidence(const PlanRequest& request,
                                                  BoundEvidence& bound) {
  std::vector<DestinationCapacity> capacity_items;
  std::vector<PolicyRule> policy_items;
  std::vector<MaintenanceWindow> window_items;
  std::vector<Incident> incident_items;
  std::vector<DomainMember> domain_items;
  std::vector<WorkloadStateRecord> asi_items;
  std::vector<FabricObligationRecord> dfi_items;
  std::vector<Candidate> candidate_items;

  for (const EvidenceSource& source : request.evidence_sources) {
    const EvidenceRecord* record = request.evidence.find(source.kind, source.source);
    if (record == nullptr) {
      continue;   // bound but absent; reported per obligation, never guessed
    }
    bound.bindings.push_back(PlanBinding{source.kind, source.source, record->stamp.generation,
                                         record->stamp.epoch, record->stamp.content_digest});
    switch (source.kind) {
      case EvidenceKind::RackComposition:
        bound.compositions.push_back(&std::get<RackCompositionPayload>(record->payload));
        break;
      case EvidenceKind::Enumeration:
        bound.enumerations.push_back(&std::get<EnumerationPayload>(record->payload));
        break;
      case EvidenceKind::ObligationCatalog: {
        bound.catalog_present = true;
        const auto& payload = std::get<ObligationCatalogPayload>(record->payload);
        for (const Obligation& obligation : payload.obligations) {
          bound.catalog.push_back(&obligation);
        }
        break;
      }
      case EvidenceKind::Capacity: {
        bound.capacity_present = true;
        const auto& payload = std::get<CapacityPayload>(record->payload);
        capacity_items.insert(capacity_items.end(), payload.destinations.begin(),
                              payload.destinations.end());
        break;
      }
      case EvidenceKind::PlacementPolicy: {
        bound.policy_present = true;
        const auto& payload = std::get<PlacementPolicyPayload>(record->payload);
        policy_items.insert(policy_items.end(), payload.rules.begin(), payload.rules.end());
        break;
      }
      case EvidenceKind::Maintenance: {
        bound.maintenance_present = true;
        const auto& payload = std::get<MaintenancePayload>(record->payload);
        window_items.insert(window_items.end(), payload.windows.begin(), payload.windows.end());
        incident_items.insert(incident_items.end(), payload.incidents.begin(),
                              payload.incidents.end());
        break;
      }
      case EvidenceKind::FailureDomain: {
        bound.domains_present = true;
        const auto& payload = std::get<FailureDomainTopology>(record->payload);
        domain_items.insert(domain_items.end(), payload.members.begin(), payload.members.end());
        break;
      }
      case EvidenceKind::AsiWorkloadState: {
        bound.asi_present = true;
        const auto& payload = std::get<AsiWorkloadPayload>(record->payload);
        asi_items.insert(asi_items.end(), payload.records.begin(), payload.records.end());
        break;
      }
      case EvidenceKind::DfiObligation: {
        bound.dfi_present = true;
        const auto& payload = std::get<DfiObligationPayload>(record->payload);
        dfi_items.insert(dfi_items.end(), payload.records.begin(), payload.records.end());
        break;
      }
      case EvidenceKind::CandidateOffers: {
        bound.offers_present = true;
        const auto& payload = std::get<CandidateOffersPayload>(record->payload);
        candidate_items.insert(candidate_items.end(), payload.candidates.begin(),
                               payload.candidates.end());
        bound.offer_sources.push_back(
            OfferSource{source.source, record->stamp.generation, record->stamp.epoch});
        break;
      }
    }
  }

  auto capacity = CapacityPayload::make(std::move(capacity_items));
  if (!capacity.ok()) {
    return capacity.error();
  }
  bound.capacity = capacity.take();

  auto policy = PlacementPolicyPayload::make(std::move(policy_items));
  if (!policy.ok()) {
    return policy.error();
  }
  bound.policy = policy.take();

  auto maintenance = MaintenancePayload::make(std::move(window_items), std::move(incident_items));
  if (!maintenance.ok()) {
    return maintenance.error();
  }
  bound.maintenance = maintenance.take();

  auto domains = FailureDomainTopology::make(std::move(domain_items));
  if (!domains.ok()) {
    return domains.error();
  }
  bound.domains = domains.take();

  auto asi = AsiWorkloadPayload::make(std::move(asi_items));
  if (!asi.ok()) {
    return asi.error();
  }
  bound.asi = asi.take();

  auto dfi = DfiObligationPayload::make(std::move(dfi_items));
  if (!dfi.ok()) {
    return dfi.error();
  }
  bound.dfi = dfi.take();

  auto offers = CandidateOffersPayload::make(std::move(candidate_items));
  if (!offers.ok()) {
    return offers.error();
  }
  bound.offers = offers.take();

  std::sort(bound.bindings.begin(), bound.bindings.end());
  std::sort(bound.offer_sources.begin(), bound.offer_sources.end(),
            [](const OfferSource& a, const OfferSource& b) { return a.source < b.source; });
  return {};
}

// ---------------------------------------------------------------------------
// Obligation set resolution
// ---------------------------------------------------------------------------

struct ObligationSet {
  RackCompositionPayload composition;
  std::vector<Obligation> in_scope;   // canonical: by obligation key
  std::vector<ScopeExclusion> out_of_scope;
  std::vector<IndeterminacyReason> reasons;
};

void add_reason(ObligationSet& set, IndeterminacyReason reason) {
  set.reasons.push_back(reason);
}

// Establishes exactly which obligations must leave, or refuses to claim that
// it knows.  Every early return here means "coverage is not established": the
// planner will publish an indeterminate plan that assigns nothing.
void resolve_obligation_set(const PlanRequest& request, const BoundEvidence& bound,
                            const PlanLimits& limits, ObligationSet& result) {
  const RackId& rack = request.isolation.rack;

  std::vector<const RackCompositionPayload*> compositions;
  for (const RackCompositionPayload* payload : bound.compositions) {
    if (payload->rack == rack) {
      compositions.push_back(payload);
    }
  }
  if (compositions.empty()) {
    add_reason(result, IndeterminacyReason::RackCompositionMissing);
    return;
  }
  const RackCompositionPayload& composition = *compositions.front();
  for (std::size_t i = 1; i < compositions.size(); ++i) {
    if (!(*compositions[i] == composition)) {
      add_reason(result, IndeterminacyReason::RackCompositionConflict);
      return;
    }
  }
  if (!(composition.composition_revision == request.isolation.composition_revision)) {
    add_reason(result, IndeterminacyReason::RackCompositionRevisionMismatch);
    return;
  }
  result.composition = composition;

  std::vector<const EnumerationPayload*> enumerations;
  for (const EnumerationPayload* payload : bound.enumerations) {
    if (payload->rack == rack) {
      enumerations.push_back(payload);
    }
  }
  if (enumerations.empty()) {
    add_reason(result, IndeterminacyReason::EnumerationMissing);
    return;
  }
  const EnumerationPayload& enumeration = *enumerations.front();
  for (std::size_t i = 1; i < enumerations.size(); ++i) {
    if (!(*enumerations[i] == enumeration)) {
      add_reason(result, IndeterminacyReason::EnumerationConflict);
      return;
    }
  }
  if (!enumeration.complete) {
    add_reason(result, IndeterminacyReason::EnumerationIncomplete);
    return;
  }
  if (!(enumeration.composition_revision == request.isolation.composition_revision)) {
    add_reason(result, IndeterminacyReason::EnumerationRevisionMismatch);
    return;
  }
  if (!same_id_set(enumeration.enumerated, composition.occupants)) {
    add_reason(result, IndeterminacyReason::EnumerationSetMismatch);
    return;
  }

  if (composition.occupants.empty()) {
    return;   // proven empty for this exact revision
  }

  if (!bound.catalog_present || bound.catalog.empty()) {
    add_reason(result, IndeterminacyReason::ObligationCatalogMissing);
    return;
  }
  std::vector<Obligation> definitions;
  definitions.reserve(bound.catalog.size());
  for (const Obligation* obligation : bound.catalog) {
    definitions.push_back(*obligation);
  }
  auto catalog = ObligationCatalogPayload::make(std::move(definitions));
  if (!catalog.ok()) {
    add_reason(result, IndeterminacyReason::ObligationDefinitionDuplicate);
    return;
  }

  const std::set<ObligationKind> requested(request.isolation.evacuate_kinds.begin(),
                                           request.isolation.evacuate_kinds.end());

  for (const ObligationId& occupant : composition.occupants) {
    const Obligation* obligation = catalog.value().find(occupant);
    if (obligation == nullptr) {
      add_reason(result, IndeterminacyReason::ObligationUndefined);
      return;
    }
    if (!(obligation->source_rack == rack)) {
      add_reason(result, IndeterminacyReason::ObligationSourceRackMismatch);
      return;
    }
    if (requested.count(obligation->kind) == 0) {
      result.out_of_scope.push_back(ScopeExclusion{obligation->id, obligation->kind,
                                                   ScopeExclusionReason::KindNotRequested});
      continue;
    }
    result.in_scope.push_back(*obligation);
  }

  if (result.in_scope.size() > limits.max_obligations) {
    add_reason(result, IndeterminacyReason::LimitExceeded);
    result.in_scope.clear();
    result.out_of_scope.clear();
    return;
  }

  std::sort(result.in_scope.begin(), result.in_scope.end(),
            [](const Obligation& a, const Obligation& b) {
              return obligation_key(a) < obligation_key(b);
            });
  std::sort(result.out_of_scope.begin(), result.out_of_scope.end());
}

// ---------------------------------------------------------------------------
// Dependency order
// ---------------------------------------------------------------------------

struct DependencyOrder {
  std::vector<std::size_t> order;          // acyclic nodes, canonical topological order
  std::vector<std::size_t> cycle_members;  // canonical index order
};

// Tarjan's algorithm (iterative, so an adversarial dependency chain cannot
// exhaust the stack), then a deterministic Kahn pass over what remains.  The
// ready set is ordered by the canonical obligation key, so the resulting order
// never depends on container iteration order.
[[nodiscard]] DependencyOrder order_by_dependency(
    const std::vector<Obligation>& obligations,
    const std::map<ObligationId, std::size_t>& position) {
  const std::size_t count = obligations.size();
  std::vector<std::vector<std::size_t>> dependents(count);
  std::vector<std::vector<std::size_t>> dependencies(count);
  for (std::size_t node = 0; node < count; ++node) {
    for (const ObligationId& dependency : obligations[node].depends_on) {
      const auto found = position.find(dependency);
      if (found == position.end()) {
        continue;   // outside the scope: it is not leaving, so it orders nothing
      }
      dependencies[node].push_back(found->second);
      dependents[found->second].push_back(node);
    }
    std::sort(dependencies[node].begin(), dependencies[node].end());
    dependencies[node].erase(std::unique(dependencies[node].begin(), dependencies[node].end()),
                             dependencies[node].end());
    std::sort(dependents[node].begin(), dependents[node].end());
    dependents[node].erase(std::unique(dependents[node].begin(), dependents[node].end()),
                           dependents[node].end());
  }

  constexpr std::size_t kUnvisited = (std::numeric_limits<std::size_t>::max)();
  std::vector<std::size_t> visit_index(count, kUnvisited);
  std::vector<std::size_t> low_link(count, 0);
  std::vector<std::size_t> child_cursor(count, 0);
  std::vector<bool> on_stack(count, false);
  std::vector<std::size_t> component_stack;
  std::vector<std::size_t> work;
  std::vector<bool> in_cycle(count, false);
  std::size_t counter = 0;

  for (std::size_t root = 0; root < count; ++root) {
    if (visit_index[root] != kUnvisited) {
      continue;
    }
    work.push_back(root);
    while (!work.empty()) {
      const std::size_t node = work.back();
      if (visit_index[node] == kUnvisited) {
        visit_index[node] = counter;
        low_link[node] = counter;
        ++counter;
        component_stack.push_back(node);
        on_stack[node] = true;
      }
      bool descended = false;
      while (child_cursor[node] < dependents[node].size()) {
        const std::size_t next = dependents[node][child_cursor[node]];
        ++child_cursor[node];
        if (visit_index[next] == kUnvisited) {
          work.push_back(next);
          descended = true;
          break;
        }
        if (on_stack[next] && visit_index[next] < low_link[node]) {
          low_link[node] = visit_index[next];
        }
      }
      if (descended) {
        continue;
      }
      if (low_link[node] == visit_index[node]) {
        std::vector<std::size_t> component;
        while (true) {
          const std::size_t member = component_stack.back();
          component_stack.pop_back();
          on_stack[member] = false;
          component.push_back(member);
          if (member == node) {
            break;
          }
        }
        // Every node of a multi-node component lies on a cycle.
        if (component.size() > 1) {
          for (const std::size_t member : component) {
            in_cycle[member] = true;
          }
        } else if (std::binary_search(dependencies[node].begin(), dependencies[node].end(),
                                      node)) {
          in_cycle[node] = true;
        }
      }
      work.pop_back();
      if (!work.empty()) {
        const std::size_t parent = work.back();
        if (low_link[node] < low_link[parent]) {
          low_link[parent] = low_link[node];
        }
      }
    }
  }

  DependencyOrder result;
  for (std::size_t node = 0; node < count; ++node) {
    if (in_cycle[node]) {
      result.cycle_members.push_back(node);
    }
  }

  std::vector<std::size_t> in_degree(count, 0);
  for (std::size_t node = 0; node < count; ++node) {
    if (in_cycle[node]) {
      continue;
    }
    for (const std::size_t dependency : dependencies[node]) {
      if (!in_cycle[dependency]) {
        ++in_degree[node];
      }
    }
  }

  std::set<std::pair<std::uint16_t, ObligationId>> ready;
  for (std::size_t node = 0; node < count; ++node) {
    if (!in_cycle[node] && in_degree[node] == 0) {
      ready.insert(obligation_key(obligations[node]));
    }
  }
  while (!ready.empty()) {
    const auto first = ready.begin();
    const ObligationId current = first->second;
    ready.erase(first);
    const std::size_t node = position.at(current);
    result.order.push_back(node);
    for (const std::size_t dependent : dependents[node]) {
      if (in_cycle[dependent] || in_degree[dependent] == 0) {
        continue;
      }
      --in_degree[dependent];
      if (in_degree[dependent] == 0) {
        ready.insert(obligation_key(obligations[dependent]));
      }
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Eligibility
// ---------------------------------------------------------------------------

struct PlanningIndex {
  std::map<DestinationRef, ResourceVector> remaining_capacity;
  std::map<ObligationId, std::vector<const Candidate*>> candidates_by_obligation;
  std::map<RackId, FailureDomainId> rack_domain;
  std::map<DestinationRef, FailureDomainId> destination_domain;
  std::map<ObligationId, FailureDomainId> obligation_domain;
};

void build_index(const BoundEvidence& bound, PlanningIndex& index) {
  for (const DestinationCapacity& entry : bound.capacity.destinations) {
    index.remaining_capacity.emplace(entry.destination, entry.available);
  }
  for (const Candidate& candidate : bound.offers.candidates) {
    index.candidates_by_obligation[candidate.obligation].push_back(&candidate);
  }
  for (const DomainMember& member : bound.domains.members) {
    if (member.is_rack) {
      index.rack_domain.emplace(member.rack, member.domain);
    } else if (!member.destination.id.empty()) {
      index.destination_domain.emplace(member.destination, member.domain);
    } else if (!member.obligation.empty()) {
      index.obligation_domain.emplace(member.obligation, member.domain);
    }
  }
}

[[nodiscard]] bool rule_scope_matches(const PolicyRule& rule, DestinationKind destination,
                                      const std::optional<FailureDomainId>& domain) {
  if (rule.destination_kind != destination) {
    return false;
  }
  if (!rule.domain.empty()) {
    return domain.has_value() && *domain == rule.domain;
  }
  return true;
}

struct Eligibility {
  const Candidate* selected{nullptr};
  std::vector<RejectedCandidate> rejected;
};

struct CandidateContext {
  std::optional<FailureDomainId> obligation_domain;
  std::optional<FailureDomainId> source_rack_domain;
  std::optional<FailureDomainId> destination_domain;
  const std::vector<DestinationRef>* permitted_endpoints{nullptr};
  bool duplicate{false};
  bool spread_required{false};
};

// Evaluates one candidate.  The check order is fixed and documented; the
// first failing check is the candidate's recorded reason.
[[nodiscard]] std::optional<RejectionReason> evaluate_candidate(
    const Obligation& obligation, const Candidate& candidate, const PlanRequest& request,
    const BoundEvidence& bound, const PlanningIndex& index, const CandidateContext& context) {
  if (context.duplicate) {
    return RejectionReason::DuplicateCandidate;
  }
  if (!action_matches_obligation(obligation.kind, candidate.action)) {
    return RejectionReason::ObligationKindNotAllowed;
  }
  if (!action_matches_destination(candidate.action, candidate.destination.kind)) {
    return RejectionReason::ActionDestinationMismatch;
  }
  if (candidate.destination.kind == DestinationKind::RackSlot &&
      candidate.destination.id.view() == obligation.source_rack.view()) {
    return RejectionReason::SelfDestination;
  }

  // The authority that asserts the action must be a stream the request bound,
  // at exactly the generation and epoch that stream published.
  bool authority_bound = false;
  bool binding_matches = false;
  for (const OfferSource& source : bound.offer_sources) {
    if (!(source.source.authority == candidate.authority)) {
      continue;
    }
    authority_bound = true;
    if (source.generation == candidate.authority_generation &&
        source.epoch == candidate.authority_epoch) {
      binding_matches = true;
      break;
    }
  }
  if (!authority_bound) {
    return RejectionReason::AuthorityMismatch;
  }
  if (!binding_matches) {
    return RejectionReason::StaleGeneration;
  }

  if (!candidate.provision.dominates(obligation.demand)) {
    return RejectionReason::CapacityClaimMismatch;
  }

  if (context.permitted_endpoints != nullptr && !context.permitted_endpoints->empty()) {
    const bool listed =
        std::binary_search(context.permitted_endpoints->begin(),
                           context.permitted_endpoints->end(), candidate.destination);
    if (!listed) {
      return RejectionReason::DestinationNotPermitted;
    }
  }

  for (const PolicyRule& rule : bound.policy.rules) {
    if (rule.kind != PolicyRuleKind::DenyAction || rule.obligation_kind != obligation.kind ||
        rule.action != candidate.action) {
      continue;
    }
    if (rule_scope_matches(rule, candidate.destination.kind, context.destination_domain)) {
      return RejectionReason::PolicyDenied;
    }
  }
  bool has_allow_rule = false;
  bool allowed = false;
  for (const PolicyRule& rule : bound.policy.rules) {
    if (rule.kind != PolicyRuleKind::AllowAction || rule.obligation_kind != obligation.kind) {
      continue;
    }
    has_allow_rule = true;
    if (rule.action == candidate.action &&
        rule_scope_matches(rule, candidate.destination.kind, context.destination_domain)) {
      allowed = true;
    }
  }
  if (has_allow_rule && !allowed) {
    return RejectionReason::PolicyDenied;
  }

  if (context.spread_required) {
    if (!context.destination_domain.has_value()) {
      return RejectionReason::FailureDomainUnknown;
    }
    const std::optional<FailureDomainId>& origin = context.obligation_domain.has_value()
                                                      ? context.obligation_domain
                                                      : context.source_rack_domain;
    if (!origin.has_value()) {
      return RejectionReason::FailureDomainUnknown;
    }
    if (*origin == *context.destination_domain) {
      return RejectionReason::SameFailureDomain;
    }
  }

  // An active blocking incident in the source or the destination failure
  // domain stops automated evacuation: the planner will not plan into or out
  // of a domain the maintenance authority has flagged.
  for (const Incident& incident : bound.maintenance.incidents) {
    if (!incident.blocks_evacuation) {
      continue;
    }
    if (context.destination_domain.has_value() && *context.destination_domain == incident.domain) {
      return RejectionReason::IncidentActive;
    }
    if (context.obligation_domain.has_value() && *context.obligation_domain == incident.domain) {
      return RejectionReason::IncidentActive;
    }
    if (context.source_rack_domain.has_value() &&
        *context.source_rack_domain == incident.domain) {
      return RejectionReason::IncidentActive;
    }
  }

  const UnixNanos instant = request.evaluation_time;
  bool window_open = false;
  for (const MaintenanceWindow& window : bound.maintenance.windows) {
    if (window.action != candidate.action) {
      continue;
    }
    if (!window.covers_destination(candidate.destination, context.destination_domain)) {
      continue;
    }
    if (!window.is_open_at(instant)) {
      continue;
    }
    if (window.permits_evacuation) {
      window_open = true;
    } else {
      return RejectionReason::MaintenanceWindowClosed;
    }
  }
  if (candidate.requires_maintenance_window && !window_open) {
    return RejectionReason::MaintenanceWindowRequired;
  }

  const auto available = index.remaining_capacity.find(candidate.destination);
  if (available == index.remaining_capacity.end()) {
    return RejectionReason::DestinationUnknown;
  }
  if (!available->second.dominates(candidate.provision)) {
    return RejectionReason::CapacityInsufficient;
  }
  return std::nullopt;
}

// Reduces a set of per-candidate rejections to one deterministic reason for
// the obligation itself, by fixed priority.
[[nodiscard]] ResidualReason dominant_residual(const std::vector<RejectedCandidate>& rejected) {
  bool capacity = false;
  bool incident = false;
  bool maintenance = false;
  bool domain = false;
  bool policy = false;
  for (const RejectedCandidate& entry : rejected) {
    switch (entry.reason) {
      case RejectionReason::CapacityInsufficient:
      case RejectionReason::DestinationUnknown:
        capacity = true;
        break;
      case RejectionReason::IncidentActive:
        incident = true;
        break;
      case RejectionReason::MaintenanceWindowRequired:
      case RejectionReason::MaintenanceWindowClosed:
        maintenance = true;
        break;
      case RejectionReason::SameFailureDomain:
      case RejectionReason::FailureDomainUnknown:
        domain = true;
        break;
      case RejectionReason::PolicyDenied:
      case RejectionReason::ObligationKindNotAllowed:
      case RejectionReason::ActionDestinationMismatch:
      case RejectionReason::DestinationNotPermitted:
      case RejectionReason::SelfDestination:
      case RejectionReason::ProtectedObligation:
        policy = true;
        break;
      default:
        break;
    }
  }
  if (capacity) {
    return ResidualReason::CapacityExhausted;
  }
  if (incident) {
    return ResidualReason::IncidentActive;
  }
  if (maintenance) {
    return ResidualReason::MaintenanceBlocked;
  }
  if (domain) {
    return ResidualReason::FailureDomainConflict;
  }
  if (policy) {
    return ResidualReason::PolicyDenied;
  }
  return ResidualReason::CandidatesRejected;
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

Result<Plan> plan_evacuation(const PlanRequest& request, const PlanSealContext& context,
                             const PlanLimits& limits) {
  Plan plan;
  plan.id = context.plan_id;
  plan.planner = context.planner;
  plan.revision = context.revision;
  plan.sequence = context.sequence;
  plan.epoch = context.epoch;
  plan.lineage = context.lineage;
  plan.idempotency_key = request.idempotency_key;
  plan.request_digest = request.digest();
  plan.source_rack = request.isolation.rack;
  plan.composition_revision = request.isolation.composition_revision;
  plan.isolation = request.isolation.kind;
  plan.evacuate_kinds = request.isolation.evacuate_kinds;

  BoundEvidence bound;
  const auto collected = collect_bound_evidence(request, bound);
  if (!collected.ok()) {
    return collected.error();
  }
  plan.bindings = bound.bindings;

  const auto seal_now = [](Plan candidate) -> Result<Plan> { return Plan::seal(std::move(candidate)); };

  if (bound.offers.candidates.size() > limits.max_candidates) {
    plan.status = PlanStatus::Indeterminate;
    plan.indeterminacy_reasons = {IndeterminacyReason::LimitExceeded};
    return seal_now(std::move(plan));
  }

  ObligationSet obligations;
  resolve_obligation_set(request, bound, limits, obligations);
  plan.indeterminacy_reasons = obligations.reasons;
  std::sort(plan.indeterminacy_reasons.begin(), plan.indeterminacy_reasons.end());
  plan.indeterminacy_reasons.erase(
      std::unique(plan.indeterminacy_reasons.begin(), plan.indeterminacy_reasons.end()),
      plan.indeterminacy_reasons.end());
  if (!plan.indeterminacy_reasons.empty()) {
    // Coverage is not established, so the plan asserts nothing beyond that.
    plan.status = PlanStatus::Indeterminate;
    return seal_now(std::move(plan));
  }

  plan.out_of_scope = obligations.out_of_scope;
  if (obligations.in_scope.empty()) {
    plan.status = PlanStatus::EmptySafe;
    return seal_now(std::move(plan));
  }

  PlanningIndex index;
  build_index(bound, index);

  std::map<ObligationId, std::size_t> position;
  for (std::size_t node = 0; node < obligations.in_scope.size(); ++node) {
    position.emplace(obligations.in_scope[node].id, node);
  }

  const DependencyOrder order = order_by_dependency(obligations.in_scope, position);

  std::vector<Assignment> assignments;
  std::vector<Residual> residuals;
  std::vector<bool> residual_flag(obligations.in_scope.size(), false);
  std::vector<std::uint32_t> wave_of(obligations.in_scope.size(), 0);

  for (const std::size_t node : order.cycle_members) {
    residual_flag[node] = true;
    residuals.push_back(Residual{obligations.in_scope[node].id, obligations.in_scope[node].kind,
                                 ResidualReason::DependencyCycle, {}});
  }

  const std::optional<FailureDomainId> source_rack_domain = [&index, &request]()
      -> std::optional<FailureDomainId> {
    const auto found = index.rack_domain.find(request.isolation.rack);
    if (found == index.rack_domain.end()) {
      return std::nullopt;
    }
    return found->second;
  }();

  std::uint32_t next_order_index = 0;

  for (const std::size_t node : order.order) {
    const Obligation& obligation = obligations.in_scope[node];

    bool dependency_failed = false;
    for (const ObligationId& dependency : obligation.depends_on) {
      const auto found = position.find(dependency);
      if (found != position.end() && residual_flag[found->second]) {
        dependency_failed = true;
        break;
      }
    }
    if (dependency_failed) {
      residual_flag[node] = true;
      residuals.push_back(Residual{obligation.id, obligation.kind,
                                   ResidualReason::DependencyUnsatisfied, {}});
      continue;
    }

    const std::optional<FailureDomainId> obligation_domain = [&index, &obligation]()
        -> std::optional<FailureDomainId> {
      const auto found = index.obligation_domain.find(obligation.id);
      if (found == index.obligation_domain.end()) {
        return std::nullopt;
      }
      return found->second;
    }();

    bool spread_required = false;
    for (const PolicyRule& rule : bound.policy.rules) {
      if (rule.kind == PolicyRuleKind::RequireDomainSpread &&
          rule.obligation_kind == obligation.kind) {
        spread_required = true;
        break;
      }
    }

    // Adjacent state for this obligation kind.  An absent stream, an absent
    // record, or an explicit "unknown" all mean the same thing here: the
    // planner cannot prove this obligation is movable.
    bool state_available = true;
    bool state_non_migratable = false;
    bool state_unknown = false;
    const std::vector<DestinationRef>* permitted_endpoints = nullptr;
    switch (state_source_for(obligation.kind)) {
      case StateSource::None:
        break;
      case StateSource::Asi: {
        const WorkloadStateRecord* record =
            bound.asi_present ? bound.asi.find(obligation.id) : nullptr;
        if (record == nullptr) {
          state_available = false;
        } else if (record->migration == MigrationCapability::Unknown) {
          state_available = false;
          state_unknown = true;
        } else if (record->migration == MigrationCapability::NotMigratable) {
          state_available = false;
          state_non_migratable = true;
        }
        break;
      }
      case StateSource::Dfi: {
        const FabricObligationRecord* record =
            bound.dfi_present ? bound.dfi.find(obligation.id) : nullptr;
        if (record == nullptr) {
          state_available = false;
        } else if (!record->path_migration_supported) {
          state_available = false;
          state_non_migratable = true;
        } else {
          permitted_endpoints = &record->permitted_endpoints;
        }
        break;
      }
    }
    if (!state_available) {
      residual_flag[node] = true;
      const ResidualReason reason = state_non_migratable ? ResidualReason::NonMigratable
                                   : state_unknown      ? ResidualReason::StateUnknown
                                                        : ResidualReason::EvidenceMissing;
      residuals.push_back(Residual{obligation.id, obligation.kind, reason, {}});
      continue;
    }

    // Protection is never inferred away: it takes an explicit policy rule.
    if (obligation.protected_obligation) {
      bool permitted = false;
      for (const PolicyRule& rule : bound.policy.rules) {
        if (rule.kind == PolicyRuleKind::AllowProtectedMove &&
            rule.obligation_kind == obligation.kind) {
          permitted = true;
          break;
        }
      }
      if (!permitted) {
        residual_flag[node] = true;
        residuals.push_back(Residual{obligation.id, obligation.kind,
                                     ResidualReason::ProtectedObligation, {}});
        continue;
      }
    }

    const auto found_candidates = index.candidates_by_obligation.find(obligation.id);
    if (found_candidates == index.candidates_by_obligation.end() ||
        found_candidates->second.empty()) {
      residual_flag[node] = true;
      residuals.push_back(Residual{obligation.id, obligation.kind, ResidualReason::NoCandidate, {}});
      continue;
    }

    // Candidates arrive in canonical id order.  Duplicate (action,
    // destination) claims are settled by that same order, so which one
    // survives never depends on how the input was assembled.
    std::set<std::pair<std::uint16_t, DestinationRef>> claimed;
    std::vector<std::pair<const Candidate*, bool>> considered;
    considered.reserve(found_candidates->second.size());
    for (const Candidate* candidate : found_candidates->second) {
      const auto key = std::make_pair(static_cast<std::uint16_t>(candidate->action),
                                      candidate->destination);
      const bool duplicate = !claimed.insert(key).second;
      considered.emplace_back(candidate, duplicate);
    }
    std::sort(considered.begin(), considered.end(),
              [](const std::pair<const Candidate*, bool>& a,
                 const std::pair<const Candidate*, bool>& b) {
                if (a.first->estimated_cost != b.first->estimated_cost) {
                  return a.first->estimated_cost < b.first->estimated_cost;
                }
                if (a.first->action != b.first->action) {
                  return static_cast<std::uint16_t>(a.first->action) <
                         static_cast<std::uint16_t>(b.first->action);
                }
                if (!(a.first->destination == b.first->destination)) {
                  return a.first->destination < b.first->destination;
                }
                return a.first->id < b.first->id;
              });

    Eligibility eligibility;
    for (const auto& entry : considered) {
      const Candidate& candidate = *entry.first;
      CandidateContext candidate_context;
      candidate_context.obligation_domain = obligation_domain;
      candidate_context.source_rack_domain = source_rack_domain;
      const auto destination_domain = index.destination_domain.find(candidate.destination);
      if (destination_domain != index.destination_domain.end()) {
        candidate_context.destination_domain = destination_domain->second;
      }
      candidate_context.permitted_endpoints = permitted_endpoints;
      candidate_context.duplicate = entry.second;
      candidate_context.spread_required = spread_required;

      const auto rejection =
          evaluate_candidate(obligation, candidate, request, bound, index, candidate_context);
      if (rejection.has_value()) {
        eligibility.rejected.push_back(RejectedCandidate{candidate.id, *rejection});
        continue;
      }
      eligibility.selected = &candidate;
      break;
    }

    if (eligibility.selected == nullptr) {
      residual_flag[node] = true;
      std::sort(eligibility.rejected.begin(), eligibility.rejected.end());
      residuals.push_back(Residual{obligation.id, obligation.kind,
                                   dominant_residual(eligibility.rejected),
                                   std::move(eligibility.rejected)});
      continue;
    }

    const Candidate& chosen = *eligibility.selected;
    const auto remaining = index.remaining_capacity.find(chosen.destination);
    if (remaining != index.remaining_capacity.end()) {
      auto reduced = remaining->second.subtract(chosen.provision);
      if (reduced.ok()) {
        remaining->second = reduced.take();
      }
    }

    std::uint32_t wave = 0;
    for (const ObligationId& dependency : obligation.depends_on) {
      const auto found = position.find(dependency);
      if (found == position.end()) {
        continue;   // outside the scope: it does not order anything
      }
      const std::uint32_t dependency_wave = wave_of[found->second];
      const std::uint32_t candidate_wave = dependency_wave + 1;
      if (candidate_wave > wave) {
        wave = candidate_wave;
      }
    }
    wave_of[node] = wave;

    assignments.push_back(Assignment{obligation.id, obligation.kind, chosen.id, chosen.action,
                                     chosen.destination, chosen.authority, wave,
                                     next_order_index});
    ++next_order_index;
  }

  std::sort(assignments.begin(), assignments.end());
  std::sort(residuals.begin(), residuals.end());
  plan.assignments = std::move(assignments);
  plan.residuals = std::move(residuals);
  plan.status = plan.residuals.empty() ? PlanStatus::Complete : PlanStatus::Partial;
  return seal_now(std::move(plan));
}

} // namespace rep::detail
