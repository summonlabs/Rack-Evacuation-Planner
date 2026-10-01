// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/policy.hpp"

#include <algorithm>
#include <utility>

#include "detail/containers.hpp"

namespace rep {

bool MaintenanceWindow::covers_destination(
    const DestinationRef& candidate, const std::optional<FailureDomainId>& candidate_domain) const {
  if (!destination.id.empty() && !(destination == candidate)) {
    return false;
  }
  if (!domain.empty()) {
    // A domain-scoped window applies only when the candidate is proven to be
    // in that domain.  An unknown domain never satisfies a scope.
    if (!candidate_domain.has_value() || !(*candidate_domain == domain)) {
      return false;
    }
  }
  return true;
}

bool MaintenanceWindow::is_open_at(UnixNanos instant) const {
  return starts_at <= instant && instant < ends_at;
}

Result<FailureDomainTopology> FailureDomainTopology::make(std::vector<DomainMember> members) {
  auto sorted = detail::sorted_deduped(std::move(members));

  // A member must describe exactly one subject, and a subject must have one
  // domain.  Both are refusals, never a silent pick.
  for (const DomainMember& member : sorted) {
    if (member.domain.empty()) {
      return make_error(ErrorCode::InvalidArgument, "failure domain member has an empty domain",
                        "domain");
    }
    int subjects = 0;
    if (member.is_rack) {
      if (member.rack.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "failure domain rack member has an empty rack", "member");
      }
      ++subjects;
    } else {
      if (!member.destination.id.empty()) {
        ++subjects;
      }
      if (!member.obligation.empty()) {
        ++subjects;
      }
    }
    if (subjects != 1) {
      return make_error(ErrorCode::InvalidArgument,
                        "failure domain member must name exactly one rack, destination, or "
                        "obligation",
                        "member");
    }
  }

  for (std::size_t i = 1; i < sorted.size(); ++i) {
    const DomainMember& previous = sorted[i - 1];
    const DomainMember& current = sorted[i];
    if (previous.is_rack != current.is_rack) {
      continue;
    }
    const bool same_subject =
        previous.is_rack ? (previous.rack == current.rack)
                         : (previous.destination == current.destination &&
                            previous.obligation == current.obligation);
    if (same_subject && !(previous.domain == current.domain)) {
      return make_error(ErrorCode::PreconditionFailed,
                        "failure domain topology assigns one subject to two domains", "member");
    }
  }

  FailureDomainTopology topology;
  topology.members = std::move(sorted);
  return topology;
}

std::optional<FailureDomainId> FailureDomainTopology::domain_of_rack(const RackId& rack) const {
  for (const DomainMember& member : members) {
    if (member.is_rack && member.rack == rack) {
      return member.domain;
    }
  }
  return std::nullopt;
}

std::optional<FailureDomainId> FailureDomainTopology::domain_of_destination(
    const DestinationRef& destination) const {
  for (const DomainMember& member : members) {
    if (!member.is_rack && member.destination == destination && !member.destination.id.empty()) {
      return member.domain;
    }
  }
  return std::nullopt;
}

std::optional<FailureDomainId> FailureDomainTopology::domain_of_obligation(
    const ObligationId& obligation) const {
  for (const DomainMember& member : members) {
    if (!member.is_rack && member.obligation == obligation && !member.obligation.empty()) {
      return member.domain;
    }
  }
  return std::nullopt;
}

Result<PlacementPolicyPayload> PlacementPolicyPayload::make(std::vector<PolicyRule> rules) {
  PlacementPolicyPayload payload;
  payload.rules = detail::sorted_deduped(std::move(rules));
  return payload;
}

Result<MaintenancePayload> MaintenancePayload::make(std::vector<MaintenanceWindow> windows,
                                                    std::vector<Incident> incidents) {
  for (const MaintenanceWindow& window : windows) {
    if (!(window.starts_at < window.ends_at)) {
      return make_error(ErrorCode::InvalidArgument,
                        "maintenance window must end after it starts", "window");
    }
  }
  MaintenancePayload payload;
  payload.windows = detail::sorted_deduped(std::move(windows));
  payload.incidents = detail::sorted_deduped(std::move(incidents));
  return payload;
}

} // namespace rep
