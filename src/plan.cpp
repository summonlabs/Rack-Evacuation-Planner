// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "rep/adjacent.hpp"
#include "rep/candidate.hpp"
#include "rep/evidence.hpp"
#include "rep/obligation.hpp"
#include "rep/policy.hpp"
#include "rep/types.hpp"

namespace rep {
namespace {

[[nodiscard]] std::string join_kinds(const std::vector<ObligationKind>& kinds) {
  std::string result;
  bool first = true;
  for (const ObligationKind kind : kinds) {
    if (!first) {
      result.push_back(',');
    }
    first = false;
    result += to_string(kind);
  }
  return result;
}

[[nodiscard]] std::string_view bool_text(bool value) noexcept {
  return value ? std::string_view("true") : std::string_view("false");
}

} // namespace

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

void Plan::encode_content(CanonicalWriter& writer) const {
  writer.text(id.view());
  writer.text(planner.view());
  revision.encode(writer);
  sequence.encode(writer);
  epoch.encode(writer);
  writer.text(lineage.view());
  writer.text(idempotency_key.view());
  writer.digest(request_digest);
  writer.text(source_rack.view());
  composition_revision.encode(writer);
  writer.u16(static_cast<std::uint16_t>(isolation));
  writer.u64(static_cast<std::uint64_t>(evacuate_kinds.size()));
  for (const ObligationKind kind : evacuate_kinds) {
    writer.u16(static_cast<std::uint16_t>(kind));
  }
  encode_sequence(writer, bindings);
  writer.u16(static_cast<std::uint16_t>(status));
  writer.u64(static_cast<std::uint64_t>(indeterminacy_reasons.size()));
  for (const IndeterminacyReason reason : indeterminacy_reasons) {
    writer.u16(static_cast<std::uint16_t>(reason));
  }
  encode_sequence(writer, assignments);
  encode_sequence(writer, residuals);
  encode_sequence(writer, out_of_scope);
}

void Plan::encode(CanonicalWriter& writer) const {
  encode_content(writer);
  writer.digest(plan_digest);
}

Digest Plan::content_digest() const {
  CanonicalWriter writer(domains::kPlan);
  encode_content(writer);
  return writer.finish();
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

Result<void> Plan::verify() const {
  if (!(content_digest() == plan_digest)) {
    return make_error(ErrorCode::DigestMismatch,
                      "plan digest does not match the plan content", "plan_digest");
  }
  if (id.empty() || planner.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "plan identity is incomplete", "id");
  }
  if (source_rack.empty()) {
    return make_error(ErrorCode::InvalidIdentifier, "plan source rack is empty", "source_rack");
  }
  if (evacuate_kinds.empty()) {
    return make_error(ErrorCode::InvalidArgument, "plan names no obligation kinds to evacuate",
                      "evacuate_kinds");
  }
  for (std::size_t i = 1; i < evacuate_kinds.size(); ++i) {
    if (!(evacuate_kinds[i - 1] < evacuate_kinds[i])) {
      return make_error(ErrorCode::InvalidArgument,
                        "plan evacuate kinds are not strictly increasing", "evacuate_kinds");
    }
  }
  for (std::size_t i = 1; i < bindings.size(); ++i) {
    if (!(bindings[i - 1] < bindings[i])) {
      return make_error(ErrorCode::InvalidArgument,
                        "plan bindings are not strictly increasing by stream", "bindings");
    }
  }
  for (std::size_t i = 1; i < indeterminacy_reasons.size(); ++i) {
    if (!(static_cast<std::uint16_t>(indeterminacy_reasons[i - 1]) <
          static_cast<std::uint16_t>(indeterminacy_reasons[i]))) {
      return make_error(ErrorCode::InvalidArgument,
                        "plan indeterminacy reasons are not strictly increasing",
                        "indeterminacy_reasons");
    }
  }

  // The evacuation order is a dense permutation starting at zero.
  for (std::size_t i = 0; i < assignments.size(); ++i) {
    if (assignments[i].order_index != i) {
      return make_error(ErrorCode::InvalidArgument,
                        "plan evacuation order is not a dense sequence", "assignments");
    }
  }

  // Every obligation appears exactly once across the three outcome lists, and
  // each list is itself strictly ordered.
  std::set<ObligationId> seen;
  for (std::size_t i = 0; i < assignments.size(); ++i) {
    if (!seen.insert(assignments[i].obligation).second) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "obligation " + assignments[i].obligation.str() +
                            " is assigned more than once",
                        "assignments");
    }
  }
  for (std::size_t i = 0; i < residuals.size(); ++i) {
    if (i > 0 && !(residuals[i - 1].obligation < residuals[i].obligation)) {
      return make_error(ErrorCode::InvalidArgument,
                        "plan residuals are not strictly ordered", "residuals");
    }
    if (!seen.insert(residuals[i].obligation).second) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "obligation " + residuals[i].obligation.str() +
                            " is both resolved and residual",
                        "residuals");
    }
  }
  for (std::size_t i = 0; i < out_of_scope.size(); ++i) {
    if (i > 0 && !(out_of_scope[i - 1].obligation < out_of_scope[i].obligation)) {
      return make_error(ErrorCode::InvalidArgument,
                        "plan scope exclusions are not strictly ordered", "out_of_scope");
    }
    if (!seen.insert(out_of_scope[i].obligation).second) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "obligation " + out_of_scope[i].obligation.str() +
                            " appears in two outcome lists",
                        "out_of_scope");
    }
  }

  switch (status) {
    case PlanStatus::Indeterminate:
      if (!assignments.empty() || !residuals.empty() || !out_of_scope.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "an indeterminate plan must not assert any obligation outcome",
                          "status");
      }
      if (indeterminacy_reasons.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "an indeterminate plan must name at least one reason", "status");
      }
      break;
    case PlanStatus::EmptySafe:
      if (!assignments.empty() || !residuals.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "a proven-empty plan must not assign or retain obligations", "status");
      }
      if (!indeterminacy_reasons.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "a proven-empty plan must not carry indeterminacy reasons", "status");
      }
      break;
    case PlanStatus::Complete:
      if (assignments.empty() || !residuals.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "a complete plan must assign every in-scope obligation", "status");
      }
      if (!indeterminacy_reasons.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "a complete plan must not carry indeterminacy reasons", "status");
      }
      break;
    case PlanStatus::Partial:
      if (residuals.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "a partial plan must name at least one residual obligation", "status");
      }
      if (!indeterminacy_reasons.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "a partial plan must not carry indeterminacy reasons", "status");
      }
      break;
  }

  // Wave structure: a wave greater than zero requires an earlier assignment in
  // the immediately preceding wave, because that is where its dependency was
  // resolved.
  for (const Assignment& assignment : assignments) {
    if (assignment.wave == 0) {
      continue;
    }
    bool found = false;
    for (const Assignment& earlier : assignments) {
      if (earlier.order_index < assignment.order_index && earlier.wave == assignment.wave - 1) {
        found = true;
        break;
      }
    }
    if (!found) {
      return make_error(ErrorCode::InvalidArgument,
                        "assignment " + assignment.obligation.str() +
                            " is in a wave with no predecessor wave",
                        "assignments");
    }
  }
  return {};
}

Result<Plan> Plan::seal(Plan plan) {
  plan.plan_digest = plan.content_digest();
  const auto verified = plan.verify();
  if (!verified.ok()) {
    return verified.error();
  }
  return plan;
}

// ---------------------------------------------------------------------------
// Staleness and verdict
// ---------------------------------------------------------------------------

std::vector<StalenessFinding> Plan::staleness(const EvidenceBundle& current) const {
  bool unused = false;
  return staleness(current, unused);
}

std::vector<StalenessFinding> Plan::staleness(const EvidenceBundle& current,
                                              bool& any_stream_missing) const {
  any_stream_missing = false;
  std::vector<StalenessFinding> findings;
  for (const PlanBinding& binding : bindings) {
    const EvidenceRecord* record = current.find(binding.kind, binding.stream);
    if (record == nullptr) {
      any_stream_missing = true;
      findings.push_back(StalenessFinding{binding.stream, StalenessKind::StreamMissing,
                                          binding.generation, Generation{}, binding.epoch,
                                          Epoch{}});
      continue;
    }
    if (!(record->stamp.generation == binding.generation)) {
      findings.push_back(StalenessFinding{binding.stream, StalenessKind::GenerationMoved,
                                          binding.generation, record->stamp.generation,
                                          binding.epoch, record->stamp.epoch});
    }
    if (!(record->stamp.epoch == binding.epoch)) {
      findings.push_back(StalenessFinding{binding.stream, StalenessKind::EpochChanged,
                                          binding.generation, record->stamp.generation,
                                          binding.epoch, record->stamp.epoch});
    }
    if (!(record->stamp.content_digest == binding.digest)) {
      findings.push_back(StalenessFinding{binding.stream, StalenessKind::DigestChanged,
                                          binding.generation, record->stamp.generation,
                                          binding.epoch, record->stamp.epoch});
    }
  }
  std::sort(findings.begin(), findings.end());
  return findings;
}

SafetyVerdict Plan::verdict(std::span<const StalenessFinding> findings) const {
  if (!findings.empty()) {
    return SafetyVerdict::Stale;
  }
  switch (status) {
    case PlanStatus::Complete:
    case PlanStatus::EmptySafe:
      return SafetyVerdict::Safe;
    case PlanStatus::Partial:
      return SafetyVerdict::NotProven;
    case PlanStatus::Indeterminate:
      return SafetyVerdict::Indeterminate;
  }
  return SafetyVerdict::Indeterminate;
}

SafetyVerdict Plan::verdict() const { return verdict(std::span<const StalenessFinding>()); }

std::uint32_t Plan::wave_count() const {
  std::uint32_t highest = 0;
  for (const Assignment& assignment : assignments) {
    if (assignment.wave > highest) {
      highest = assignment.wave;
    }
  }
  return assignments.empty() ? 0 : highest + 1;
}

std::size_t Plan::obligation_count() const {
  return assignments.size() + residuals.size() + out_of_scope.size();
}

// ---------------------------------------------------------------------------
// Canonical text
// ---------------------------------------------------------------------------

std::string Plan::to_text() const {
  std::string out;
  const auto line = [&out](std::string_view text) {
    out.append(text);
    out.push_back('\n');
  };

  line("plan id=" + id.str() + " planner=" + planner.str() +
       " revision=" + std::to_string(revision.value()) +
       " sequence=" + std::to_string(sequence.value()) +
       " epoch=" + std::to_string(epoch.value()) + " lineage=" + lineage.str() +
       " idempotency_key=" + idempotency_key.str() + " request_digest=" + request_digest.to_hex());
  line("source rack=" + source_rack.str() +
       " composition_revision=" + std::to_string(composition_revision.value()) +
       " isolation=" + std::string(to_string(isolation)) +
       " evacuate_kinds=" + join_kinds(evacuate_kinds) +
       " status=" + std::string(to_string(status)) +
       " status_verdict=" + std::string(to_string(verdict())) +
       " obligations=" + std::to_string(obligation_count()) +
       " waves=" + std::to_string(wave_count()) + " plan_digest=" + plan_digest.to_hex());

  for (const PlanBinding& binding : bindings) {
    line("binding kind=" + std::string(to_string(binding.kind)) +
         " authority=" + binding.stream.authority.str() + " stream=" + binding.stream.stream.str() +
         " generation=" + std::to_string(binding.generation.value()) +
         " epoch=" + std::to_string(binding.epoch.value()) + " digest=" + binding.digest.to_hex());
  }
  for (const IndeterminacyReason reason : indeterminacy_reasons) {
    line("indeterminacy reason=" + std::string(to_string(reason)));
  }
  for (const ScopeExclusion& exclusion : out_of_scope) {
    line("out_of_scope obligation=" + exclusion.obligation.str() +
         " kind=" + std::string(to_string(exclusion.kind)) +
         " reason=" + std::string(to_string(exclusion.reason)));
  }
  for (const Residual& residual : residuals) {
    line("residual obligation=" + residual.obligation.str() +
         " kind=" + std::string(to_string(residual.kind)) +
         " reason=" + std::string(to_string(residual.reason)));
  }
  for (const Residual& residual : residuals) {
    for (const RejectedCandidate& rejected : residual.rejected) {
      line("rejected obligation=" + residual.obligation.str() +
           " candidate=" + rejected.candidate.str() +
           " reason=" + std::string(to_string(rejected.reason)));
    }
  }
  for (const Assignment& assignment : assignments) {
    line("assignment order=" + std::to_string(assignment.order_index) +
         " obligation=" + assignment.obligation.str() +
         " kind=" + std::string(to_string(assignment.kind)) +
         " candidate=" + assignment.candidate.str() +
         " action=" + std::string(to_string(assignment.action)) +
         " destination=" + assignment.destination.to_text() +
         " authority=" + assignment.authority.str() +
         " wave=" + std::to_string(assignment.wave));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Independent audit
// ---------------------------------------------------------------------------

namespace {

struct AuditBound {
  std::vector<const RackCompositionPayload*> compositions;
  std::vector<const EnumerationPayload*> enumerations;
  std::map<ObligationId, const Obligation*> catalog;
  std::map<CandidateId, const Candidate*> candidates;
  std::map<DestinationRef, ResourceVector> capacity;
  std::vector<PlanBinding> bindings;
  bool candidates_present{false};
  bool capacity_present{false};
  bool catalog_present{false};
};

void audit_collect(const PlanRequest& request, AuditBound& bound) {
  for (const EvidenceSource& source : request.evidence_sources) {
    const EvidenceRecord* record = request.evidence.find(source.kind, source.source);
    if (record == nullptr) {
      continue;
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
          bound.catalog.emplace(obligation.id, &obligation);
        }
        break;
      }
      case EvidenceKind::Capacity: {
        bound.capacity_present = true;
        const auto& payload = std::get<CapacityPayload>(record->payload);
        for (const DestinationCapacity& entry : payload.destinations) {
          bound.capacity.emplace(entry.destination, entry.available);
        }
        break;
      }
      case EvidenceKind::CandidateOffers: {
        bound.candidates_present = true;
        const auto& payload = std::get<CandidateOffersPayload>(record->payload);
        for (const Candidate& candidate : payload.candidates) {
          bound.candidates.emplace(candidate.id, &candidate);
        }
        break;
      }
      default:
        break;
    }
  }
  std::sort(bound.bindings.begin(), bound.bindings.end());
}

void add_finding(std::vector<AuditFinding>& findings, std::string code, std::string detail) {
  findings.push_back(AuditFinding{std::move(code), std::move(detail)});
}

} // namespace

std::vector<AuditFinding> audit_plan(const Plan& plan, const PlanRequest& request) {
  std::vector<AuditFinding> findings;

  const auto integrity = plan.verify();
  if (!integrity.ok()) {
    add_finding(findings, "plan_integrity", integrity.error().to_string());
    return findings;
  }

  if (!(plan.request_digest == request.digest())) {
    add_finding(findings, "request_digest_mismatch",
                "plan was not sealed against this request");
  }
  if (!(plan.source_rack == request.isolation.rack) ||
      !(plan.composition_revision == request.isolation.composition_revision) ||
      plan.isolation != request.isolation.kind ||
      !(plan.evacuate_kinds == request.isolation.evacuate_kinds)) {
    add_finding(findings, "request_binding_mismatch",
                "plan binding does not match the request it claims to answer");
  }

  AuditBound bound;
  audit_collect(request, bound);

  if (!(bound.bindings == plan.bindings)) {
    add_finding(findings, "binding_set_mismatch",
                "plan bindings are not exactly the bound streams that were present");
  }

  // Rebuild the obligation set from the request, independently of the planner.
  const RackId& rack = request.isolation.rack;
  const RackCompositionPayload* composition = nullptr;
  for (const RackCompositionPayload* candidate : bound.compositions) {
    if (candidate->rack == rack && (candidate->composition_revision == request.isolation.composition_revision)) {
      composition = candidate;
      break;
    }
  }
  const EnumerationPayload* enumeration = nullptr;
  for (const EnumerationPayload* candidate : bound.enumerations) {
    if (candidate->rack == rack &&
        (candidate->composition_revision == request.isolation.composition_revision) &&
        candidate->complete) {
      enumeration = candidate;
      break;
    }
  }

  if (composition == nullptr || enumeration == nullptr ||
      !(enumeration->enumerated == composition->occupants)) {
    if (plan.status != PlanStatus::Indeterminate) {
      add_finding(findings, "expected_indeterminate",
                  "the obligation set is not established by the bound evidence, but the plan "
                  "does not report indeterminate");
    }
    return findings;
  }

  const std::set<ObligationKind> requested(request.isolation.evacuate_kinds.begin(),
                                           request.isolation.evacuate_kinds.end());
  std::set<ObligationId> expected_in_scope;
  std::set<ObligationId> expected_out_of_scope;
  std::map<ObligationId, const Obligation*> definitions;
  for (const ObligationId& occupant : composition->occupants) {
    const auto found = bound.catalog.find(occupant);
    if (found == bound.catalog.end()) {
      if (plan.status != PlanStatus::Indeterminate) {
        add_finding(findings, "expected_indeterminate",
                    "occupant " + occupant.str() + " has no definition");
      }
      return findings;
    }
    definitions.emplace(occupant, found->second);
    if (requested.count(found->second->kind) != 0) {
      expected_in_scope.insert(occupant);
    } else {
      expected_out_of_scope.insert(occupant);
    }
  }

  std::set<ObligationId> actual;
  for (const Assignment& assignment : plan.assignments) {
    actual.insert(assignment.obligation);
  }
  for (const Residual& residual : plan.residuals) {
    actual.insert(residual.obligation);
  }
  std::set<ObligationId> actual_out;
  for (const ScopeExclusion& exclusion : plan.out_of_scope) {
    actual_out.insert(exclusion.obligation);
  }

  for (const ObligationId& obligation : expected_in_scope) {
    if (actual.count(obligation) == 0) {
      add_finding(findings, "conservation_missing",
                  "in-scope obligation " + obligation.str() + " has no outcome in the plan");
    }
  }
  for (const ObligationId& obligation : actual) {
    if (expected_in_scope.count(obligation) == 0) {
      add_finding(findings, "conservation_extra",
                  "plan reports " + obligation.str() + " as in scope, but it is not");
    }
  }
  for (const ObligationId& obligation : expected_out_of_scope) {
    if (actual_out.count(obligation) == 0) {
      add_finding(findings, "scope_missing",
                  "out-of-scope obligation " + obligation.str() + " is not recorded");
    }
  }
  for (const ObligationId& obligation : actual_out) {
    if (expected_out_of_scope.count(obligation) == 0) {
      add_finding(findings, "scope_extra",
                  "plan claims " + obligation.str() + " is out of scope, but it is not");
    }
  }

  if (plan.status == PlanStatus::EmptySafe && !expected_in_scope.empty()) {
    add_finding(findings, "unsound_empty_safe",
                "plan claims nothing must leave, but in-scope obligations exist");
  }
  if ((plan.status == PlanStatus::Complete) && plan.assignments.size() != expected_in_scope.size()) {
    add_finding(findings, "unsound_complete",
                "plan claims completeness without assigning every in-scope obligation");
  }

  // Ordering: dependencies must be assigned strictly earlier.
  std::map<ObligationId, std::uint32_t> order_of;
  std::map<ObligationId, std::uint32_t> wave_of;
  for (const Assignment& assignment : plan.assignments) {
    order_of.emplace(assignment.obligation, assignment.order_index);
    wave_of.emplace(assignment.obligation, assignment.wave);
  }
  for (const Assignment& assignment : plan.assignments) {
    const auto definition = definitions.find(assignment.obligation);
    if (definition == definitions.end()) {
      continue;
    }
    for (const ObligationId& dependency : definition->second->depends_on) {
      if (expected_in_scope.count(dependency) == 0) {
        continue;
      }
      const auto dependency_order = order_of.find(dependency);
      if (dependency_order == order_of.end()) {
        add_finding(findings, "ordering_dependency_unassigned",
                    assignment.obligation.str() + " is assigned, but its dependency " +
                        dependency.str() + " is not");
        continue;
      }
      if (!(dependency_order->second < assignment.order_index)) {
        add_finding(findings, "ordering_dependency_after",
                    assignment.obligation.str() + " is ordered before its dependency " +
                        dependency.str());
      }
      if (wave_of[assignment.obligation] <= wave_of[dependency]) {
        add_finding(findings, "ordering_wave",
                    assignment.obligation.str() + " is not in a later wave than its dependency " +
                        dependency.str());
      }
    }
  }

  // Capacity: the assigned set must fit in the reported capacity of every
  // destination, using checked arithmetic.
  std::map<DestinationRef, ResourceVector> used;
  for (const Assignment& assignment : plan.assignments) {
    const auto candidate = bound.candidates.find(assignment.candidate);
    if (candidate == bound.candidates.end()) {
      add_finding(findings, "assignment_candidate_unknown",
                  "assignment names candidate " + assignment.candidate.str() +
                      ", which no bound stream offers");
      continue;
    }
    const Candidate& offer = *candidate->second;
    if (!(offer.obligation == assignment.obligation) || offer.action != assignment.action ||
        !(offer.destination == assignment.destination) ||
        !(offer.authority == assignment.authority)) {
      add_finding(findings, "assignment_candidate_mismatch",
                  "assignment does not match the candidate it names");
    }
    const auto definition = definitions.find(assignment.obligation);
    if (definition != definitions.end()) {
      if (!action_matches_obligation(definition->second->kind, offer.action)) {
        add_finding(findings, "assignment_action_incompatible",
                    assignment.obligation.str() + " cannot be evacuated by " +
                        std::string(to_string(offer.action)));
      }
      if (!offer.provision.dominates(definition->second->demand)) {
        add_finding(findings, "assignment_provision_short",
                    assignment.obligation.str() + " is moved with less than it holds");
      }
      if (offer.destination.kind == DestinationKind::RackSlot &&
          offer.destination.id.view() == definition->second->source_rack.view()) {
        add_finding(findings, "assignment_self_destination",
                    assignment.obligation.str() + " is assigned to its own rack");
      }
    }
    const auto existing = used.find(offer.destination);
    if (existing == used.end()) {
      used.emplace(offer.destination, offer.provision);
    } else {
      auto sum = existing->second.add(offer.provision);
      if (!sum.ok()) {
        add_finding(findings, "assignment_capacity_overflow",
                    "provisions for " + offer.destination.to_text() + " overflow");
      } else {
        existing->second = sum.take();
      }
    }
  }
  for (const auto& entry : used) {
    const auto declared = bound.capacity.find(entry.first);
    if (declared == bound.capacity.end()) {
      add_finding(findings, "assignment_destination_unknown",
                  "no bound capacity stream describes " + entry.first.to_text());
      continue;
    }
    if (!declared->second.dominates(entry.second)) {
      add_finding(findings, "assignment_capacity_exceeded",
                  "assigned provisions exceed the reported capacity of " +
                      entry.first.to_text());
    }
  }

  std::sort(findings.begin(), findings.end(),
            [](const AuditFinding& a, const AuditFinding& b) {
              if (a.code != b.code) {
                return a.code < b.code;
              }
              return a.detail < b.detail;
            });
  return findings;
}

} // namespace rep
