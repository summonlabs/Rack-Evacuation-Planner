// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_ADJACENT_HPP
#define REP_ADJACENT_HPP

#include <cstdint>
#include <vector>

#include "rep/canonical.hpp"
#include "rep/export.hpp"
#include "rep/status.hpp"
#include "rep/types.hpp"

// Evidence payloads published by adjacent authorities.  The planner consumes
// them and never writes them back: nothing here is planner state.

namespace rep {

// Destination capacity published by the facility capacity authority.  The
// planner treats it as the authoritative statement of what a destination can
// accept at this generation, and never derives it from occupancy.
struct REP_API DestinationCapacity {
  DestinationRef destination;
  ResourceVector available;
  Generation generation;
  Epoch epoch;

  void encode(CanonicalWriter& writer) const {
    destination.encode(writer);
    available.encode(writer);
    generation.encode(writer);
    epoch.encode(writer);
  }
  friend bool operator==(const DestinationCapacity& a, const DestinationCapacity& b) noexcept {
    return a.destination == b.destination && a.available == b.available &&
           a.generation == b.generation && a.epoch == b.epoch;
  }
  friend auto operator<=>(const DestinationCapacity& a, const DestinationCapacity& b) noexcept {
    if (a.destination != b.destination) {
      return a.destination <=> b.destination;
    }
    if (a.generation != b.generation) {
      return a.generation <=> b.generation;
    }
    return a.epoch <=> b.epoch;
  }
};

struct REP_API CapacityPayload {
  std::vector<DestinationCapacity> destinations;   // canonical: sorted by destination, unique

  [[nodiscard]] static Result<CapacityPayload> make(std::vector<DestinationCapacity> destinations);
  [[nodiscard]] const DestinationCapacity* find(const DestinationRef& destination) const noexcept;

  void encode(CanonicalWriter& writer) const { encode_sequence(writer, destinations); }
  friend bool operator==(const CapacityPayload& a, const CapacityPayload& b) noexcept {
    return a.destinations == b.destinations;
  }
};

// Workload state published by the adjacent scheduling infrastructure (ASI).
struct REP_API WorkloadStateRecord {
  ObligationId obligation;
  WorkloadLifecycle lifecycle{WorkloadLifecycle::Unknown};
  MigrationCapability migration{MigrationCapability::Unknown};
  StorageAttachment attachment{StorageAttachment::Unknown};

  void encode(CanonicalWriter& writer) const {
    writer.text(obligation.view());
    writer.u16(static_cast<std::uint16_t>(lifecycle));
    writer.u16(static_cast<std::uint16_t>(migration));
    writer.u16(static_cast<std::uint16_t>(attachment));
  }
  friend bool operator==(const WorkloadStateRecord& a, const WorkloadStateRecord& b) noexcept {
    return a.obligation == b.obligation && a.lifecycle == b.lifecycle &&
           a.migration == b.migration && a.attachment == b.attachment;
  }
  friend auto operator<=>(const WorkloadStateRecord& a, const WorkloadStateRecord& b) noexcept {
    return a.obligation <=> b.obligation;
  }
};

struct REP_API AsiWorkloadPayload {
  std::vector<WorkloadStateRecord> records;   // canonical: sorted by obligation, unique

  [[nodiscard]] static Result<AsiWorkloadPayload> make(std::vector<WorkloadStateRecord> records);
  [[nodiscard]] const WorkloadStateRecord* find(const ObligationId& id) const noexcept;

  void encode(CanonicalWriter& writer) const { encode_sequence(writer, records); }
  friend bool operator==(const AsiWorkloadPayload& a, const AsiWorkloadPayload& b) noexcept {
    return a.records == b.records;
  }
};

// Path/namespace obligation state published by the data fabric authority
// (DFI).  permitted_endpoints is the set of destinations this obligation may
// be rebound to, as the authority that owns the path states it; an empty list
// means the authority published no endpoint restriction, which the planner
// reports honestly rather than reading as a denial.  A non-empty list is a
// closed set: a destination outside it is refused.
struct REP_API FabricObligationRecord {
  ObligationId obligation;
  FabricObligationKind kind{FabricObligationKind::PathAttachment};
  bool path_migration_supported{false};
  std::vector<DestinationRef> permitted_endpoints;   // canonical: sorted, unique
  Digest mapping_digest;

  void encode(CanonicalWriter& writer) const {
    writer.text(obligation.view());
    writer.u16(static_cast<std::uint16_t>(kind));
    writer.boolean(path_migration_supported);
    encode_sequence(writer, permitted_endpoints);
    writer.digest(mapping_digest);
  }
  friend bool operator==(const FabricObligationRecord& a,
                         const FabricObligationRecord& b) noexcept {
    return a.obligation == b.obligation && a.kind == b.kind &&
           a.path_migration_supported == b.path_migration_supported &&
           a.permitted_endpoints == b.permitted_endpoints && a.mapping_digest == b.mapping_digest;
  }
  friend auto operator<=>(const FabricObligationRecord& a,
                          const FabricObligationRecord& b) noexcept {
    return a.obligation <=> b.obligation;
  }
};

struct REP_API DfiObligationPayload {
  std::vector<FabricObligationRecord> records;   // canonical: sorted by obligation, unique

  [[nodiscard]] static Result<DfiObligationPayload> make(std::vector<FabricObligationRecord> records);
  [[nodiscard]] const FabricObligationRecord* find(const ObligationId& id) const noexcept;

  void encode(CanonicalWriter& writer) const { encode_sequence(writer, records); }
  friend bool operator==(const DfiObligationPayload& a,
                         const DfiObligationPayload& b) noexcept {
    return a.records == b.records;
  }
};

} // namespace rep

#endif // REP_ADJACENT_HPP
