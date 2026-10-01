// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/adjacent.hpp"

#include <algorithm>
#include <utility>

#include "detail/containers.hpp"

namespace rep {

Result<CapacityPayload> CapacityPayload::make(std::vector<DestinationCapacity> destinations) {
  auto canonical = detail::sorted_unique_by(
      std::move(destinations),
      [](const DestinationCapacity& item) { return item.destination; }, "destination capacity");
  if (!canonical.ok()) {
    return canonical.error();
  }
  CapacityPayload payload;
  payload.destinations = canonical.take();
  return payload;
}

const DestinationCapacity* CapacityPayload::find(const DestinationRef& destination) const noexcept {
  const auto position = std::lower_bound(
      destinations.begin(), destinations.end(), destination,
      [](const DestinationCapacity& item, const DestinationRef& key) {
        return item.destination < key;
      });
  if (position == destinations.end() || !(position->destination == destination)) {
    return nullptr;
  }
  return &*position;
}

Result<AsiWorkloadPayload> AsiWorkloadPayload::make(std::vector<WorkloadStateRecord> records) {
  auto canonical = detail::sorted_unique_by(
      std::move(records), [](const WorkloadStateRecord& item) { return item.obligation; },
      "workload state");
  if (!canonical.ok()) {
    return canonical.error();
  }
  AsiWorkloadPayload payload;
  payload.records = canonical.take();
  return payload;
}

const WorkloadStateRecord* AsiWorkloadPayload::find(const ObligationId& id) const noexcept {
  const auto position = std::lower_bound(
      records.begin(), records.end(), id,
      [](const WorkloadStateRecord& item, const ObligationId& key) { return item.obligation < key; });
  if (position == records.end() || !(position->obligation == id)) {
    return nullptr;
  }
  return &*position;
}

Result<DfiObligationPayload> DfiObligationPayload::make(std::vector<FabricObligationRecord> records) {
  auto canonical = detail::sorted_unique_by(
      std::move(records), [](const FabricObligationRecord& item) { return item.obligation; },
      "fabric obligation");
  if (!canonical.ok()) {
    return canonical.error();
  }
  // The permitted endpoint set is documented canonical and the planner
  // searches it, so it is normalised here rather than trusted.
  for (FabricObligationRecord& record : canonical.value()) {
    record.permitted_endpoints = detail::sorted_deduped(std::move(record.permitted_endpoints));
  }
  DfiObligationPayload payload;
  payload.records = canonical.take();
  return payload;
}

const FabricObligationRecord* DfiObligationPayload::find(const ObligationId& id) const noexcept {
  const auto position = std::lower_bound(
      records.begin(), records.end(), id,
      [](const FabricObligationRecord& item, const ObligationId& key) {
        return item.obligation < key;
      });
  if (position == records.end() || !(position->obligation == id)) {
    return nullptr;
  }
  return &*position;
}

} // namespace rep
