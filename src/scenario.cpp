// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/scenario.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "detail/platform.hpp"
#include "detail/text.hpp"
#include "rep/adjacent.hpp"
#include "rep/candidate.hpp"
#include "rep/evidence.hpp"
#include "rep/obligation.hpp"
#include "rep/policy.hpp"

// Canonical scenario text format ("rep.scenario.v1"):
//
//   version 1
//   planner <planner-id>
//   request key=<id> authority=<id> epoch=<n> time=<n> lineage=<id|-> rack=<id>
//           isolation=<kind> composition_revision=<n> kinds=<kind>[,<kind>...]
//   source evidence=<kind> authority=<id> stream=<id>
//   evidence <kind> stream=<id> [authority=<id>] generation=<n> epoch=<n> [digest=<hex64>]
//     (authority is required only when no source directive declares the
//      stream; such a record stays in the bundle as unbound evidence)
//     <body lines, one record per line>
//   end
//
// Scalar body lines are "<key> <value>"; record body lines are
// "<record> <key>=<value> ...".  "-" means an absent optional value.  A line
// whose first non-blank character is '#' is a comment.  Nothing else is
// accepted: unknown directives, unknown keys, duplicate keys, malformed
// numbers, invalid identifiers, and non-ASCII bytes are refused with the exact
// line and column.

namespace rep {
namespace {

constexpr std::string_view kMissing = "-";
constexpr std::uint64_t kFormatVersion = 1;

struct Token {
  std::string_view text;
  std::size_t column{1};
};

struct Line {
  std::size_t number{1};
  std::string_view text;
};

[[nodiscard]] Error fail(const Line& line, std::size_t column, std::string detail) {
  return make_error(ErrorCode::InvalidEncoding,
                    "line " + std::to_string(line.number) + " column " + std::to_string(column) +
                        ": " + std::move(detail),
                    "scenario");
}

[[nodiscard]] std::string_view trim(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

[[nodiscard]] std::vector<Token> tokenize(std::string_view text) {
  std::vector<Token> tokens;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) {
      ++index;
    }
    if (index >= text.size()) {
      break;
    }
    const std::size_t begin = index;
    while (index < text.size() && text[index] != ' ' && text[index] != '\t') {
      ++index;
    }
    tokens.push_back(Token{text.substr(begin, index - begin), begin + 1});
  }
  return tokens;
}

// ---------------------------------------------------------------------------
// Field helpers
// ---------------------------------------------------------------------------

struct Field {
  std::string_view key;
  std::string_view value;
  std::size_t column{1};
};

using Fields = std::vector<Field>;

[[nodiscard]] const Field* find_field(const Fields& fields, std::string_view key) {
  for (const Field& field : fields) {
    if (field.key == key) {
      return &field;
    }
  }
  return nullptr;
}

[[nodiscard]] Result<Fields> read_fields(const Line& line, const std::vector<Token>& tokens,
                                         std::size_t first, std::string_view record) {
  Fields fields;
  for (std::size_t index = first; index < tokens.size(); ++index) {
    const std::string_view token = tokens[index].text;
    const std::size_t separator = token.find('=');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= token.size()) {
      return fail(line, tokens[index].column,
                  "expected key=value in '" + std::string(record) + "', got '" +
                      detail::escape_bytes(token) + "'");
    }
    const std::string_view key = token.substr(0, separator);
    if (find_field(fields, key) != nullptr) {
      return fail(line, tokens[index].column, "duplicate key '" + std::string(key) + "'");
    }
    fields.push_back(Field{key, token.substr(separator + 1), tokens[index].column});
  }
  return fields;
}

// A field the caller has already required.  It carries the failure when the
// key is absent; otherwise it yields the field pointer, so a handler checks
// once and then reads the value through "**required" or "required->".
class RequiredField {
 public:
  RequiredField(Error error) : error_(std::move(error)) {}
  RequiredField(const Field* field) : field_(field) {}

  [[nodiscard]] bool ok() const noexcept { return field_ != nullptr; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] const Field* operator*() const noexcept { return field_; }
  [[nodiscard]] const Field* operator->() const noexcept { return field_; }

 private:
  const Field* field_{nullptr};
  Error error_;
};

[[nodiscard]] RequiredField require_field(const Line& line, const Fields& fields,
                                          std::string_view key) {
  const Field* field = find_field(fields, key);
  if (field == nullptr) {
    return RequiredField(fail(line, 1, "missing required key '" + std::string(key) + "'"));
  }
  return RequiredField(field);
}

[[nodiscard]] Result<void> reject_unknown(const Line& line, const Fields& fields,
                                          const std::vector<std::string_view>& allowed) {
  for (const Field& field : fields) {
    if (std::find(allowed.begin(), allowed.end(), field.key) == allowed.end()) {
      return fail(line, field.column, "unknown key '" + std::string(field.key) + "'");
    }
  }
  return {};
}

[[nodiscard]] Result<std::uint64_t> as_unsigned(const Line& line, const Field& field) {
  std::uint64_t value = 0;
  if (!detail::parse_u64(field.value, value)) {
    return fail(line, field.column, "key '" + std::string(field.key) +
                                        "' must be an unsigned decimal, got '" +
                                        detail::escape_bytes(field.value) + "'");
  }
  return value;
}

[[nodiscard]] Result<std::int64_t> as_signed(const Line& line, const Field& field) {
  std::int64_t value = 0;
  if (!detail::parse_i64(field.value, value)) {
    return fail(line, field.column, "key '" + std::string(field.key) +
                                        "' must be a signed decimal, got '" +
                                        detail::escape_bytes(field.value) + "'");
  }
  return value;
}

[[nodiscard]] Result<bool> as_boolean(const Line& line, const Field& field) {
  bool value = false;
  if (!detail::parse_bool(field.value, value)) {
    return fail(line, field.column, "key '" + std::string(field.key) +
                                        "' must be true or false, got '" +
                                        detail::escape_bytes(field.value) + "'");
  }
  return value;
}

template <class Enum, class FromString>
[[nodiscard]] Result<Enum> as_enum(const Line& line, const Field& field, FromString from_string) {
  const auto value = from_string(field.value);
  if (!value.has_value()) {
    return fail(line, field.column, "key '" + std::string(field.key) + "' has unknown value '" +
                                        detail::escape_bytes(field.value) + "'");
  }
  return value.value();
}

template <class Id>
[[nodiscard]] Result<Id> as_id(const Line& line, const Field& field) {
  auto parsed = Id::parse(field.value);
  if (!parsed.ok()) {
    return fail(line, field.column,
                "key '" + std::string(field.key) + "' " + parsed.error().message);
  }
  return parsed.take();
}

[[nodiscard]] Result<std::vector<std::string_view>> as_list(const Line& line, const Field& field) {
  std::vector<std::string_view> items;
  if (field.value == kMissing) {
    return items;
  }
  for (const std::string_view item : detail::split(field.value, ',')) {
    if (item.empty()) {
      return fail(line, field.column, "key '" + std::string(field.key) +
                                          "' has an empty list item");
    }
    items.push_back(item);
  }
  return items;
}

template <class Id>
[[nodiscard]] Result<std::vector<Id>> as_id_list(const Line& line, const Field& field) {
  const auto items = as_list(line, field);
  if (!items.ok()) {
    return items.error();
  }
  std::vector<Id> ids;
  ids.reserve(items.value().size());
  for (const std::string_view item : items.value()) {
    auto parsed = Id::parse(item);
    if (!parsed.ok()) {
      return fail(line, field.column,
                  "key '" + std::string(field.key) + "' " + parsed.error().message);
    }
    ids.push_back(parsed.take());
  }
  return ids;
}

[[nodiscard]] Result<ResourceVector> as_resources(const Line& line, const Field& field) {
  const std::string normalised = field.value == kMissing ? std::string() : std::string(field.value);
  auto parsed = ResourceVector::parse(normalised);
  if (!parsed.ok()) {
    return fail(line, field.column,
                "key '" + std::string(field.key) + "' " + parsed.error().message);
  }
  return parsed.take();
}

[[nodiscard]] Result<DestinationRef> as_destination(const Line& line, const Field& field) {
  auto parsed = DestinationRef::parse(field.value);
  if (!parsed.ok()) {
    return fail(line, field.column,
                "key '" + std::string(field.key) + "' " + parsed.error().message);
  }
  return parsed.take();
}

[[nodiscard]] Result<FailureDomainId> as_domain(const Line& line, const Field& field) {
  if (field.value == kMissing) {
    return FailureDomainId{};
  }
  return as_id<FailureDomainId>(line, field);
}

[[nodiscard]] Result<LineageId> as_lineage(const Line& line, const Field& field) {
  if (field.value == kMissing) {
    return LineageId{};
  }
  return as_id<LineageId>(line, field);
}

[[nodiscard]] std::string join_ids(const std::vector<ObligationId>& ids) {
  if (ids.empty()) {
    return std::string(kMissing);
  }
  std::string result;
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (index != 0) {
      result.push_back(',');
    }
    result += ids[index].str();
  }
  return result;
}

[[nodiscard]] std::string join_destinations(const std::vector<DestinationRef>& destinations) {
  if (destinations.empty()) {
    return std::string(kMissing);
  }
  std::string result;
  for (std::size_t index = 0; index < destinations.size(); ++index) {
    if (index != 0) {
      result.push_back(',');
    }
    result += destinations[index].to_text();
  }
  return result;
}

[[nodiscard]] std::string join_kinds(const std::vector<ObligationKind>& kinds) {
  if (kinds.empty()) {
    return std::string(kMissing);
  }
  std::string result;
  for (std::size_t index = 0; index < kinds.size(); ++index) {
    if (index != 0) {
      result.push_back(',');
    }
    result += to_string(kinds[index]);
  }
  return result;
}

[[nodiscard]] std::string optional_text(const std::string& value) {
  return value.empty() ? std::string(kMissing) : value;
}

[[nodiscard]] std::string resources_text(const ResourceVector& resources) {
  const std::string text = resources.to_text();
  return text.empty() ? std::string(kMissing) : text;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

struct SourceDecl {
  EvidenceKind kind{EvidenceKind::RackComposition};
  AuthorityId authority;
  StreamId stream;
};

// The payload being assembled for the evidence block currently open.
struct Block {
  bool open{false};
  EvidenceKind kind{EvidenceKind::RackComposition};
  AuthorityId authority;
  StreamId stream;
  bool bound{false};
  Generation generation;
  Epoch epoch;
  std::optional<Digest> claimed_digest;

  bool has_rack{false};
  RackId rack;
  bool has_revision{false};
  Generation revision;
  bool has_complete{false};
  bool complete{false};
  std::vector<ObligationId> occupants;
  bool occupants_set{false};

  std::vector<Obligation> catalog;
  std::vector<DestinationCapacity> capacity;
  std::vector<PolicyRule> rules;
  std::vector<MaintenanceWindow> windows;
  std::vector<Incident> incidents;
  std::vector<DomainMember> members;
  std::vector<WorkloadStateRecord> asi;
  std::vector<FabricObligationRecord> dfi;
  std::vector<Candidate> candidates;
};

struct Parser {
  const ScenarioLimits* limits{nullptr};
  bool version_seen{false};
  bool planner_seen{false};
  bool request_seen{false};
  PlannerId planner;
  IdempotencyKey key;
  AuthorityId authority;
  Epoch expected_epoch;
  UnixNanos evaluation_time;
  IsolationRequest isolation;
  LineageId lineage;
  std::vector<SourceDecl> sources;
  std::vector<EvidenceRecord> records;
  Block block;
};

[[nodiscard]] const SourceDecl* find_source(const Parser& parser, EvidenceKind kind,
                                                 const StreamId& stream) {
  for (const SourceDecl& source : parser.sources) {
    if (source.kind == kind && source.stream == stream) {
      return &source;
    }
  }
  return nullptr;
}

[[nodiscard]] Result<void> handle_composition(Parser& parser, const Line& line,
                                              const std::vector<Token>& tokens) {
  if (tokens.size() != 2) {
    return fail(line, 1, "rack_composition body lines are '<key> <value>'");
  }
  const std::string_view key = tokens[0].text;
  const Field field{key, tokens[1].text, tokens[1].column};
  if (key == "rack") {
    if (parser.block.has_rack) {
      return fail(line, tokens[0].column, "duplicate key 'rack'");
    }
    auto rack = as_id<RackId>(line, field);
    if (!rack.ok()) {
      return rack.error();
    }
    parser.block.rack = rack.take();
    parser.block.has_rack = true;
    return {};
  }
  if (key == "revision") {
    if (parser.block.has_revision) {
      return fail(line, tokens[0].column, "duplicate key 'revision'");
    }
    const auto revision = as_unsigned(line, field);
    if (!revision.ok()) {
      return revision.error();
    }
    parser.block.revision = Generation{revision.value()};
    parser.block.has_revision = true;
    return {};
  }
  if (key == "occupants") {
    if (parser.block.occupants_set) {
      return fail(line, tokens[0].column, "duplicate key 'occupants'");
    }
    auto occupants = as_id_list<ObligationId>(line, field);
    if (!occupants.ok()) {
      return occupants.error();
    }
    parser.block.occupants = occupants.take();
    parser.block.occupants_set = true;
    return {};
  }
  return fail(line, tokens[0].column, "unknown key '" + std::string(key) + "'");
}

[[nodiscard]] Result<void> handle_enumeration(Parser& parser, const Line& line,
                                              const std::vector<Token>& tokens) {
  if (tokens.size() != 2) {
    return fail(line, 1, "enumeration body lines are '<key> <value>'");
  }
  const std::string_view key = tokens[0].text;
  const Field field{key, tokens[1].text, tokens[1].column};
  if (key == "rack") {
    if (parser.block.has_rack) {
      return fail(line, tokens[0].column, "duplicate key 'rack'");
    }
    auto rack = as_id<RackId>(line, field);
    if (!rack.ok()) {
      return rack.error();
    }
    parser.block.rack = rack.take();
    parser.block.has_rack = true;
    return {};
  }
  if (key == "revision") {
    if (parser.block.has_revision) {
      return fail(line, tokens[0].column, "duplicate key 'revision'");
    }
    const auto revision = as_unsigned(line, field);
    if (!revision.ok()) {
      return revision.error();
    }
    parser.block.revision = Generation{revision.value()};
    parser.block.has_revision = true;
    return {};
  }
  if (key == "complete") {
    if (parser.block.has_complete) {
      return fail(line, tokens[0].column, "duplicate key 'complete'");
    }
    const auto complete = as_boolean(line, field);
    if (!complete.ok()) {
      return complete.error();
    }
    parser.block.complete = complete.value();
    parser.block.has_complete = true;
    return {};
  }
  if (key == "enumerated") {
    if (parser.block.occupants_set) {
      return fail(line, tokens[0].column, "duplicate key 'enumerated'");
    }
    auto enumerated = as_id_list<ObligationId>(line, field);
    if (!enumerated.ok()) {
      return enumerated.error();
    }
    parser.block.occupants = enumerated.take();
    parser.block.occupants_set = true;
    return {};
  }
  return fail(line, tokens[0].column, "unknown key '" + std::string(key) + "'");
}

[[nodiscard]] Result<void> handle_obligation(Parser& parser, const Line& line, const Fields& fields) {
  const auto unknown =
      reject_unknown(line, fields, {"id", "kind", "rack", "demand", "protected", "depends"});
  if (!unknown.ok()) {
    return unknown.error();
  }
  const auto id_field = require_field(line, fields, "id");
  if (!id_field.ok()) {
    return id_field.error();
  }
  const auto kind_field = require_field(line, fields, "kind");
  if (!kind_field.ok()) {
    return kind_field.error();
  }
  const auto rack_field = require_field(line, fields, "rack");
  if (!rack_field.ok()) {
    return rack_field.error();
  }
  const auto demand_field = require_field(line, fields, "demand");
  if (!demand_field.ok()) {
    return demand_field.error();
  }
  const auto protected_field = require_field(line, fields, "protected");
  if (!protected_field.ok()) {
    return protected_field.error();
  }
  const auto depends_field = require_field(line, fields, "depends");
  if (!depends_field.ok()) {
    return depends_field.error();
  }
  auto id = as_id<ObligationId>(line, **id_field);
  if (!id.ok()) {
    return id.error();
  }
  const auto kind =
      as_enum<ObligationKind>(line, **kind_field, &ObligationKind_from_string);
  if (!kind.ok()) {
    return kind.error();
  }
  auto rack = as_id<RackId>(line, **rack_field);
  if (!rack.ok()) {
    return rack.error();
  }
  auto demand = as_resources(line, **demand_field);
  if (!demand.ok()) {
    return demand.error();
  }
  const auto is_protected = as_boolean(line, **protected_field);
  if (!is_protected.ok()) {
    return is_protected.error();
  }
  auto depends = as_id_list<ObligationId>(line, **depends_field);
  if (!depends.ok()) {
    return depends.error();
  }
  if (parser.block.catalog.size() >= parser.limits->max_obligations) {
    return fail(line, 1, "scenario declares more obligations than the limit allows");
  }
  auto obligation = Obligation::make(id.take(), kind.value(), rack.take(), demand.take(),
                                     is_protected.value(), depends.take());
  if (!obligation.ok()) {
    return fail(line, 1, obligation.error().message);
  }
  parser.block.catalog.push_back(obligation.take());
  return {};
}

[[nodiscard]] Result<void> handle_destination(Parser& parser, const Line& line,
                                              const Fields& fields) {
  const auto unknown = reject_unknown(line, fields, {"destination", "available"});
  if (!unknown.ok()) {
    return unknown.error();
  }
  const auto destination_field = require_field(line, fields, "destination");
  if (!destination_field.ok()) {
    return destination_field.error();
  }
  const auto available_field = require_field(line, fields, "available");
  if (!available_field.ok()) {
    return available_field.error();
  }
  auto destination = as_destination(line, **destination_field);
  if (!destination.ok()) {
    return destination.error();
  }
  auto available = as_resources(line, **available_field);
  if (!available.ok()) {
    return available.error();
  }
  DestinationCapacity entry;
  entry.destination = destination.take();
  entry.available = available.take();
  entry.generation = parser.block.generation;
  entry.epoch = parser.block.epoch;
  parser.block.capacity.push_back(std::move(entry));
  return {};
}

[[nodiscard]] Result<void> handle_rule(Parser& parser, const Line& line, const Fields& fields) {
  const auto unknown =
      reject_unknown(line, fields, {"kind", "obligation", "action", "destination", "domain"});
  if (!unknown.ok()) {
    return unknown.error();
  }
  const auto kind_field = require_field(line, fields, "kind");
  if (!kind_field.ok()) {
    return kind_field.error();
  }
  const auto obligation_field = require_field(line, fields, "obligation");
  if (!obligation_field.ok()) {
    return obligation_field.error();
  }
  const auto action_field = require_field(line, fields, "action");
  if (!action_field.ok()) {
    return action_field.error();
  }
  const auto destination_field = require_field(line, fields, "destination");
  if (!destination_field.ok()) {
    return destination_field.error();
  }
  const auto domain_field = require_field(line, fields, "domain");
  if (!domain_field.ok()) {
    return domain_field.error();
  }
  const auto kind = as_enum<PolicyRuleKind>(line, **kind_field, &PolicyRuleKind_from_string);
  if (!kind.ok()) {
    return kind.error();
  }
  const auto obligation =
      as_enum<ObligationKind>(line, **obligation_field, &ObligationKind_from_string);
  if (!obligation.ok()) {
    return obligation.error();
  }
  const auto action = as_enum<ActionKind>(line, **action_field, &ActionKind_from_string);
  if (!action.ok()) {
    return action.error();
  }
  const auto destination =
      as_enum<DestinationKind>(line, **destination_field, &DestinationKind_from_string);
  if (!destination.ok()) {
    return destination.error();
  }
  auto domain = as_domain(line, **domain_field);
  if (!domain.ok()) {
    return domain.error();
  }
  PolicyRule rule;
  rule.kind = kind.value();
  rule.obligation_kind = obligation.value();
  rule.action = action.value();
  rule.destination_kind = destination.value();
  rule.domain = domain.take();
  parser.block.rules.push_back(std::move(rule));
  return {};
}

[[nodiscard]] Result<void> handle_window(Parser& parser, const Line& line, const Fields& fields) {
  const auto unknown =
      reject_unknown(line, fields, {"domain", "destination", "action", "start", "end", "permits"});
  if (!unknown.ok()) {
    return unknown.error();
  }
  const auto domain_field = require_field(line, fields, "domain");
  if (!domain_field.ok()) {
    return domain_field.error();
  }
  const auto destination_field = require_field(line, fields, "destination");
  if (!destination_field.ok()) {
    return destination_field.error();
  }
  const auto action_field = require_field(line, fields, "action");
  if (!action_field.ok()) {
    return action_field.error();
  }
  const auto start_field = require_field(line, fields, "start");
  if (!start_field.ok()) {
    return start_field.error();
  }
  const auto end_field = require_field(line, fields, "end");
  if (!end_field.ok()) {
    return end_field.error();
  }
  const auto permits_field = require_field(line, fields, "permits");
  if (!permits_field.ok()) {
    return permits_field.error();
  }
  auto domain = as_domain(line, **domain_field);
  if (!domain.ok()) {
    return domain.error();
  }
  MaintenanceWindow window;
  window.domain = domain.take();
  if ((**destination_field).value != kMissing) {
    auto destination = as_destination(line, **destination_field);
    if (!destination.ok()) {
      return destination.error();
    }
    window.destination = destination.take();
  } else {
    window.destination.kind = DestinationKind::RackSlot;
  }
  const auto action = as_enum<ActionKind>(line, **action_field, &ActionKind_from_string);
  if (!action.ok()) {
    return action.error();
  }
  window.action = action.value();
  const auto start = as_signed(line, **start_field);
  if (!start.ok()) {
    return start.error();
  }
  const auto end = as_signed(line, **end_field);
  if (!end.ok()) {
    return end.error();
  }
  window.starts_at = UnixNanos{start.value()};
  window.ends_at = UnixNanos{end.value()};
  const auto permits = as_boolean(line, **permits_field);
  if (!permits.ok()) {
    return permits.error();
  }
  window.permits_evacuation = permits.value();
  parser.block.windows.push_back(std::move(window));
  return {};
}

[[nodiscard]] Result<void> handle_incident(Parser& parser, const Line& line, const Fields& fields) {
  const auto unknown = reject_unknown(line, fields, {"domain", "severity", "blocks"});
  if (!unknown.ok()) {
    return unknown.error();
  }
  const auto domain_field = require_field(line, fields, "domain");
  if (!domain_field.ok()) {
    return domain_field.error();
  }
  const auto severity_field = require_field(line, fields, "severity");
  if (!severity_field.ok()) {
    return severity_field.error();
  }
  const auto blocks_field = require_field(line, fields, "blocks");
  if (!blocks_field.ok()) {
    return blocks_field.error();
  }
  auto domain = as_id<FailureDomainId>(line, **domain_field);
  if (!domain.ok()) {
    return domain.error();
  }
  const auto severity =
      as_enum<IncidentSeverity>(line, **severity_field, &IncidentSeverity_from_string);
  if (!severity.ok()) {
    return severity.error();
  }
  const auto blocks = as_boolean(line, **blocks_field);
  if (!blocks.ok()) {
    return blocks.error();
  }
  Incident incident;
  incident.domain = domain.take();
  incident.severity = severity.value();
  incident.blocks_evacuation = blocks.value();
  parser.block.incidents.push_back(std::move(incident));
  return {};
}

[[nodiscard]] Result<void> handle_member(Parser& parser, const Line& line, const Fields& fields) {
  const auto unknown =
      reject_unknown(line, fields, {"kind", "domain", "rack", "destination", "obligation"});
  if (!unknown.ok()) {
    return unknown.error();
  }
  const auto kind_field = require_field(line, fields, "kind");
  if (!kind_field.ok()) {
    return kind_field.error();
  }
  const auto domain_field = require_field(line, fields, "domain");
  if (!domain_field.ok()) {
    return domain_field.error();
  }
  auto domain = as_id<FailureDomainId>(line, **domain_field);
  if (!domain.ok()) {
    return domain.error();
  }
  DomainMember member;
  member.domain = domain.take();
  if ((**kind_field).value == "rack") {
    const auto rack_field = require_field(line, fields, "rack");
    if (!rack_field.ok()) {
      return rack_field.error();
    }
    auto rack = as_id<RackId>(line, **rack_field);
    if (!rack.ok()) {
      return rack.error();
    }
    member.is_rack = true;
    member.rack = rack.take();
  } else if ((**kind_field).value == "destination") {
    const auto destination_field = require_field(line, fields, "destination");
    if (!destination_field.ok()) {
      return destination_field.error();
    }
    auto destination = as_destination(line, **destination_field);
    if (!destination.ok()) {
      return destination.error();
    }
    member.destination = destination.take();
  } else if ((**kind_field).value == "obligation") {
    const auto obligation_field = require_field(line, fields, "obligation");
    if (!obligation_field.ok()) {
      return obligation_field.error();
    }
    auto obligation = as_id<ObligationId>(line, **obligation_field);
    if (!obligation.ok()) {
      return obligation.error();
    }
    member.obligation = obligation.take();
  } else {
    return fail(line, (**kind_field).column,
                "kind must be rack, destination, or obligation");
  }
  parser.block.members.push_back(std::move(member));
  return {};
}

[[nodiscard]] Result<void> handle_workload_state(Parser& parser, const Line& line,
                                                 const Fields& fields) {
  const auto unknown =
      reject_unknown(line, fields, {"obligation", "lifecycle", "migration", "attachment"});
  if (!unknown.ok()) {
    return unknown.error();
  }
  const auto obligation_field = require_field(line, fields, "obligation");
  if (!obligation_field.ok()) {
    return obligation_field.error();
  }
  const auto lifecycle_field = require_field(line, fields, "lifecycle");
  if (!lifecycle_field.ok()) {
    return lifecycle_field.error();
  }
  const auto migration_field = require_field(line, fields, "migration");
  if (!migration_field.ok()) {
    return migration_field.error();
  }
  const auto attachment_field = require_field(line, fields, "attachment");
  if (!attachment_field.ok()) {
    return attachment_field.error();
  }
  auto obligation = as_id<ObligationId>(line, **obligation_field);
  if (!obligation.ok()) {
    return obligation.error();
  }
  WorkloadStateRecord record;
  record.obligation = obligation.take();
  const auto lifecycle =
      as_enum<WorkloadLifecycle>(line, **lifecycle_field, &WorkloadLifecycle_from_string);
  if (!lifecycle.ok()) {
    return lifecycle.error();
  }
  record.lifecycle = lifecycle.value();
  const auto migration =
      as_enum<MigrationCapability>(line, **migration_field, &MigrationCapability_from_string);
  if (!migration.ok()) {
    return migration.error();
  }
  record.migration = migration.value();
  const auto attachment =
      as_enum<StorageAttachment>(line, **attachment_field, &StorageAttachment_from_string);
  if (!attachment.ok()) {
    return attachment.error();
  }
  record.attachment = attachment.value();
  parser.block.asi.push_back(std::move(record));
  return {};
}

[[nodiscard]] Result<void> handle_fabric_obligation(Parser& parser, const Line& line,
                                                    const Fields& fields) {
  const auto unknown = reject_unknown(
      line, fields, {"obligation", "kind", "path_migration", "endpoints", "mapping_digest"});
  if (!unknown.ok()) {
    return unknown.error();
  }
  const auto obligation_field = require_field(line, fields, "obligation");
  if (!obligation_field.ok()) {
    return obligation_field.error();
  }
  const auto kind_field = require_field(line, fields, "kind");
  if (!kind_field.ok()) {
    return kind_field.error();
  }
  const auto path_field = require_field(line, fields, "path_migration");
  if (!path_field.ok()) {
    return path_field.error();
  }
  const auto endpoints_field = require_field(line, fields, "endpoints");
  if (!endpoints_field.ok()) {
    return endpoints_field.error();
  }
  const auto mapping_field = require_field(line, fields, "mapping_digest");
  if (!mapping_field.ok()) {
    return mapping_field.error();
  }
  auto obligation = as_id<ObligationId>(line, **obligation_field);
  if (!obligation.ok()) {
    return obligation.error();
  }
  FabricObligationRecord record;
  record.obligation = obligation.take();
  const auto kind =
      as_enum<FabricObligationKind>(line, **kind_field, &FabricObligationKind_from_string);
  if (!kind.ok()) {
    return kind.error();
  }
  record.kind = kind.value();
  const auto path = as_boolean(line, **path_field);
  if (!path.ok()) {
    return path.error();
  }
  record.path_migration_supported = path.value();
  const auto endpoints = as_list(line, **endpoints_field);
  if (!endpoints.ok()) {
    return endpoints.error();
  }
  for (const std::string_view item : endpoints.value()) {
    const Field field{"endpoints", item, (**endpoints_field).column};
    auto destination = as_destination(line, field);
    if (!destination.ok()) {
      return destination.error();
    }
    record.permitted_endpoints.push_back(destination.take());
  }
  const auto mapping = Digest::from_hex((**mapping_field).value);
  if (!mapping.has_value()) {
    return fail(line, (**mapping_field).column,
                "mapping_digest must be 64 hexadecimal characters");
  }
  record.mapping_digest = mapping.value();
  parser.block.dfi.push_back(std::move(record));
  return {};
}

[[nodiscard]] Result<void> handle_candidate(Parser& parser, const Line& line,
                                            const Fields& fields) {
  const auto unknown = reject_unknown(line, fields,
                                      {"id", "obligation", "action", "destination", "authority",
                                       "generation", "epoch", "provision", "cost", "window",
                                       "claimed_digest"});
  if (!unknown.ok()) {
    return unknown.error();
  }
  const auto id_field = require_field(line, fields, "id");
  if (!id_field.ok()) {
    return id_field.error();
  }
  const auto obligation_field = require_field(line, fields, "obligation");
  if (!obligation_field.ok()) {
    return obligation_field.error();
  }
  const auto action_field = require_field(line, fields, "action");
  if (!action_field.ok()) {
    return action_field.error();
  }
  const auto destination_field = require_field(line, fields, "destination");
  if (!destination_field.ok()) {
    return destination_field.error();
  }
  const auto authority_field = require_field(line, fields, "authority");
  if (!authority_field.ok()) {
    return authority_field.error();
  }
  const auto generation_field = require_field(line, fields, "generation");
  if (!generation_field.ok()) {
    return generation_field.error();
  }
  const auto epoch_field = require_field(line, fields, "epoch");
  if (!epoch_field.ok()) {
    return epoch_field.error();
  }
  const auto provision_field = require_field(line, fields, "provision");
  if (!provision_field.ok()) {
    return provision_field.error();
  }
  const auto cost_field = require_field(line, fields, "cost");
  if (!cost_field.ok()) {
    return cost_field.error();
  }
  const auto window_field = require_field(line, fields, "window");
  if (!window_field.ok()) {
    return window_field.error();
  }

  auto id = as_id<CandidateId>(line, **id_field);
  if (!id.ok()) {
    return id.error();
  }
  auto obligation = as_id<ObligationId>(line, **obligation_field);
  if (!obligation.ok()) {
    return obligation.error();
  }
  const auto action = as_enum<ActionKind>(line, **action_field, &ActionKind_from_string);
  if (!action.ok()) {
    return action.error();
  }
  auto destination = as_destination(line, **destination_field);
  if (!destination.ok()) {
    return destination.error();
  }
  auto authority = as_id<AuthorityId>(line, **authority_field);
  if (!authority.ok()) {
    return authority.error();
  }
  const auto generation = as_unsigned(line, **generation_field);
  if (!generation.ok()) {
    return generation.error();
  }
  const auto epoch = as_unsigned(line, **epoch_field);
  if (!epoch.ok()) {
    return epoch.error();
  }
  auto provision = as_resources(line, **provision_field);
  if (!provision.ok()) {
    return provision.error();
  }
  const auto cost = as_unsigned(line, **cost_field);
  if (!cost.ok()) {
    return cost.error();
  }
  if (cost.value() > 0xffffffffull) {
    return fail(line, (**cost_field).column, "cost must fit in 32 bits");
  }
  const auto window = as_boolean(line, **window_field);
  if (!window.ok()) {
    return window.error();
  }

  if (parser.block.candidates.size() >= parser.limits->max_candidates) {
    return fail(line, 1, "scenario declares more candidates than the limit allows");
  }
  auto candidate = Candidate::make(id.take(), obligation.take(), action.value(), destination.take(),
                                   authority.take(), Generation{generation.value()},
                                   Epoch{epoch.value()}, provision.take(),
                                   static_cast<std::uint32_t>(cost.value()), window.value());
  if (!candidate.ok()) {
    return fail(line, 1, candidate.error().message);
  }
  Candidate built = candidate.take();
  if (const Field* claimed = find_field(fields, "claimed_digest"); claimed != nullptr) {
    // An explicitly claimed digest is what gets published.  If it does not
    // match the declaration, the evidence bundle refuses the record, which is
    // exactly the behaviour this knob exists to exercise.
    const auto digest = Digest::from_hex(claimed->value);
    if (!digest.has_value()) {
      return fail(line, claimed->column, "claimed_digest must be 64 hexadecimal characters");
    }
    built.evidence_digest = digest.value();
  }
  parser.block.candidates.push_back(std::move(built));
  return {};
}

[[nodiscard]] Result<void> handle_body(Parser& parser, const Line& line,
                                       const std::vector<Token>& tokens) {
  const std::string_view record = tokens[0].text;
  const EvidenceKind kind = parser.block.kind;
  if (kind == EvidenceKind::RackComposition) {
    return handle_composition(parser, line, tokens);
  }
  if (kind == EvidenceKind::Enumeration) {
    return handle_enumeration(parser, line, tokens);
  }

  const auto expected_record = [kind]() -> std::string_view {
    switch (kind) {
      case EvidenceKind::ObligationCatalog:
        return "obligation";
      case EvidenceKind::Capacity:
        return "destination";
      case EvidenceKind::PlacementPolicy:
        return "rule";
      case EvidenceKind::FailureDomain:
        return "member";
      case EvidenceKind::AsiWorkloadState:
      case EvidenceKind::DfiObligation:
        return "record";
      case EvidenceKind::CandidateOffers:
        return "candidate";
      default:
        return "window";
    }
  }();
  if (kind == EvidenceKind::Maintenance && record != "window" && record != "incident") {
    return fail(line, tokens[0].column,
                "expected 'window' or 'incident' in a maintenance block");
  }
  if (kind != EvidenceKind::Maintenance && record != expected_record) {
    return fail(line, tokens[0].column,
                "expected '" + std::string(expected_record) + "' in this block, got '" +
                    detail::escape_bytes(record) + "'");
  }

  const auto fields = read_fields(line, tokens, 1, record);
  if (!fields.ok()) {
    return fields.error();
  }
  switch (kind) {
    case EvidenceKind::ObligationCatalog:
      return handle_obligation(parser, line, fields.value());
    case EvidenceKind::Capacity:
      return handle_destination(parser, line, fields.value());
    case EvidenceKind::PlacementPolicy:
      return handle_rule(parser, line, fields.value());
    case EvidenceKind::Maintenance:
      if (record == "window") {
        return handle_window(parser, line, fields.value());
      }
      return handle_incident(parser, line, fields.value());
    case EvidenceKind::FailureDomain:
      return handle_member(parser, line, fields.value());
    case EvidenceKind::AsiWorkloadState:
      return handle_workload_state(parser, line, fields.value());
    case EvidenceKind::DfiObligation:
      return handle_fabric_obligation(parser, line, fields.value());
    case EvidenceKind::CandidateOffers:
      return handle_candidate(parser, line, fields.value());
    default:
      return fail(line, 1, "unsupported evidence kind");
  }
}

[[nodiscard]] Result<void> close_block(Parser& parser, const Line& line) {
  if (!parser.block.open) {
    return fail(line, 1, "'end' without an open evidence block");
  }
  EvidencePayload payload;
  switch (parser.block.kind) {
    case EvidenceKind::RackComposition: {
      if (!parser.block.has_rack || !parser.block.has_revision) {
        return fail(line, 1, "rack_composition block needs rack and revision");
      }
      auto made = RackCompositionPayload::make(parser.block.rack, parser.block.revision,
                                               parser.block.occupants);
      if (!made.ok()) {
        return fail(line, 1, made.error().message);
      }
      payload = made.take();
      break;
    }
    case EvidenceKind::Enumeration: {
      if (!parser.block.has_rack || !parser.block.has_revision || !parser.block.has_complete) {
        return fail(line, 1, "enumeration block needs rack, revision, and complete");
      }
      auto made = EnumerationPayload::make(parser.block.rack, parser.block.revision,
                                           parser.block.complete, parser.block.occupants);
      if (!made.ok()) {
        return fail(line, 1, made.error().message);
      }
      payload = made.take();
      break;
    }
    case EvidenceKind::ObligationCatalog: {
      auto made = ObligationCatalogPayload::make(parser.block.catalog);
      if (!made.ok()) {
        return fail(line, 1, made.error().message);
      }
      payload = made.take();
      break;
    }
    case EvidenceKind::Capacity: {
      auto made = CapacityPayload::make(parser.block.capacity);
      if (!made.ok()) {
        return fail(line, 1, made.error().message);
      }
      payload = made.take();
      break;
    }
    case EvidenceKind::PlacementPolicy: {
      auto made = PlacementPolicyPayload::make(parser.block.rules);
      if (!made.ok()) {
        return fail(line, 1, made.error().message);
      }
      payload = made.take();
      break;
    }
    case EvidenceKind::Maintenance: {
      auto made = MaintenancePayload::make(parser.block.windows, parser.block.incidents);
      if (!made.ok()) {
        return fail(line, 1, made.error().message);
      }
      payload = made.take();
      break;
    }
    case EvidenceKind::FailureDomain: {
      auto made = FailureDomainTopology::make(parser.block.members);
      if (!made.ok()) {
        return fail(line, 1, made.error().message);
      }
      payload = made.take();
      break;
    }
    case EvidenceKind::AsiWorkloadState: {
      auto made = AsiWorkloadPayload::make(parser.block.asi);
      if (!made.ok()) {
        return fail(line, 1, made.error().message);
      }
      payload = made.take();
      break;
    }
    case EvidenceKind::DfiObligation: {
      auto made = DfiObligationPayload::make(parser.block.dfi);
      if (!made.ok()) {
        return fail(line, 1, made.error().message);
      }
      payload = made.take();
      break;
    }
    case EvidenceKind::CandidateOffers: {
      auto made = CandidateOffersPayload::make(parser.block.candidates);
      if (!made.ok()) {
        return fail(line, 1, made.error().message);
      }
      payload = made.take();
      break;
    }
  }

  EvidenceStamp stamp;
  stamp.source = StreamRef{parser.block.authority, parser.block.stream};
  stamp.generation = parser.block.generation;
  stamp.epoch = parser.block.epoch;
  stamp.content_digest = parser.block.claimed_digest.has_value()
                             ? *parser.block.claimed_digest
                             : evidence_payload_digest(parser.block.kind, payload);
  parser.records.push_back(EvidenceRecord{parser.block.kind, stamp, std::move(payload)});
  parser.block = Block{};
  return {};
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void append_line(std::string& out, std::string_view text) {
  out.append(text);
  out.push_back('\n');
}

[[nodiscard]] std::string render_block_body(const EvidenceRecord& record) {
  std::string out;
  std::visit(
      [&out](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, RackCompositionPayload>) {
          append_line(out, "rack " + payload.rack.str());
          append_line(out, "revision " + std::to_string(payload.composition_revision.value()));
          append_line(out, "occupants " + join_ids(payload.occupants));
        } else if constexpr (std::is_same_v<Payload, EnumerationPayload>) {
          append_line(out, "rack " + payload.rack.str());
          append_line(out, "revision " + std::to_string(payload.composition_revision.value()));
          append_line(out, std::string("complete ") + (payload.complete ? "true" : "false"));
          append_line(out, "enumerated " + join_ids(payload.enumerated));
        } else if constexpr (std::is_same_v<Payload, ObligationCatalogPayload>) {
          for (const Obligation& obligation : payload.obligations) {
            append_line(out, "obligation id=" + obligation.id.str() + " kind=" +
                                 std::string(to_string(obligation.kind)) +
                                 " rack=" + obligation.source_rack.str() +
                                 " demand=" + resources_text(obligation.demand) +
                                 " protected=" + (obligation.protected_obligation ? "true" : "false") +
                                 " depends=" + join_ids(obligation.depends_on));
          }
        } else if constexpr (std::is_same_v<Payload, CapacityPayload>) {
          for (const DestinationCapacity& entry : payload.destinations) {
            append_line(out, "destination destination=" + entry.destination.to_text() +
                                 " available=" + resources_text(entry.available));
          }
        } else if constexpr (std::is_same_v<Payload, PlacementPolicyPayload>) {
          for (const PolicyRule& rule : payload.rules) {
            append_line(out, "rule kind=" + std::string(to_string(rule.kind)) +
                                 " obligation=" + std::string(to_string(rule.obligation_kind)) +
                                 " action=" + std::string(to_string(rule.action)) +
                                 " destination=" + std::string(to_string(rule.destination_kind)) +
                                 " domain=" + optional_text(rule.domain.str()));
          }
        } else if constexpr (std::is_same_v<Payload, MaintenancePayload>) {
          for (const MaintenanceWindow& window : payload.windows) {
            const std::string destination =
                window.destination.id.empty() ? std::string(kMissing) : window.destination.to_text();
            append_line(out, "window domain=" + optional_text(window.domain.str()) +
                                 " destination=" + destination +
                                 " action=" + std::string(to_string(window.action)) +
                                 " start=" + std::to_string(window.starts_at.value()) +
                                 " end=" + std::to_string(window.ends_at.value()) +
                                 " permits=" + (window.permits_evacuation ? "true" : "false"));
          }
          for (const Incident& incident : payload.incidents) {
            append_line(out, "incident domain=" + incident.domain.str() +
                                 " severity=" + std::string(to_string(incident.severity)) +
                                 " blocks=" + (incident.blocks_evacuation ? "true" : "false"));
          }
        } else if constexpr (std::is_same_v<Payload, FailureDomainTopology>) {
          for (const DomainMember& member : payload.members) {
            if (member.is_rack) {
              append_line(out, "member kind=rack rack=" + member.rack.str() +
                                   " domain=" + member.domain.str());
            } else if (!member.destination.id.empty()) {
              append_line(out, "member kind=destination destination=" +
                                   member.destination.to_text() +
                                   " domain=" + member.domain.str());
            } else {
              append_line(out, "member kind=obligation obligation=" + member.obligation.str() +
                                   " domain=" + member.domain.str());
            }
          }
        } else if constexpr (std::is_same_v<Payload, AsiWorkloadPayload>) {
          for (const WorkloadStateRecord& record : payload.records) {
            append_line(out, "record obligation=" + record.obligation.str() +
                                 " lifecycle=" + std::string(to_string(record.lifecycle)) +
                                 " migration=" + std::string(to_string(record.migration)) +
                                 " attachment=" + std::string(to_string(record.attachment)));
          }
        } else if constexpr (std::is_same_v<Payload, DfiObligationPayload>) {
          for (const FabricObligationRecord& record : payload.records) {
            append_line(out, "record obligation=" + record.obligation.str() +
                                 " kind=" + std::string(to_string(record.kind)) +
                                 " path_migration=" +
                                 (record.path_migration_supported ? "true" : "false") +
                                 " endpoints=" + join_destinations(record.permitted_endpoints) +
                                 " mapping_digest=" + record.mapping_digest.to_hex());
          }
        } else if constexpr (std::is_same_v<Payload, CandidateOffersPayload>) {
          for (const Candidate& candidate : payload.candidates) {
            append_line(out, "candidate id=" + candidate.id.str() + " obligation=" +
                                 candidate.obligation.str() +
                                 " action=" + std::string(to_string(candidate.action)) +
                                 " destination=" + candidate.destination.to_text() +
                                 " authority=" + candidate.authority.str() +
                                 " generation=" +
                                 std::to_string(candidate.authority_generation.value()) +
                                 " epoch=" + std::to_string(candidate.authority_epoch.value()) +
                                 " provision=" + resources_text(candidate.provision) +
                                 " cost=" + std::to_string(candidate.estimated_cost) +
                                 " window=" +
                                 (candidate.requires_maintenance_window ? "true" : "false") +
                                 " claimed_digest=" + candidate.evidence_digest.to_hex());
          }
        }
      },
      record.payload);
  return out;
}

} // namespace

Result<ScenarioDocument> parse_scenario(std::string_view text, const ScenarioLimits& limits) {
  if (text.size() > limits.max_bytes) {
    return make_error(ErrorCode::LimitExceeded, "scenario exceeds the byte limit", "scenario");
  }
  // A single leading UTF-8 byte order mark is a container artifact; every
  // other non-ASCII byte is refused, because nothing in this format needs one.
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
      static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf) {
    text.remove_prefix(3);
  }

  Parser parser;
  parser.limits = &limits;

  std::size_t start = 0;
  std::size_t number = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    std::string_view raw =
        end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start);
    if (!raw.empty() && raw.back() == '\r') {
      raw.remove_suffix(1);
    }
    ++number;
    const Line line{number, raw};

    if (raw.size() > limits.max_line_length) {
      return fail(line, 1, "line exceeds the maximum line length");
    }
    if (number > limits.max_lines) {
      return make_error(ErrorCode::LimitExceeded, "scenario exceeds the line limit", "scenario");
    }
    for (const char character : raw) {
      const auto byte = static_cast<unsigned char>(character);
      if (byte > 0x7e || (byte < 0x20 && byte != '\t')) {
        return fail(line, 1, "line contains a byte outside printable ASCII");
      }
    }

    const std::string_view trimmed = trim(raw);
    if (trimmed.empty() || trimmed.front() == '#') {
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
      continue;
    }

    const std::vector<Token> tokens = tokenize(trimmed);
    const std::string_view keyword = tokens[0].text;
    const Line content_line{number, trimmed};

    if (parser.block.open) {
      if (keyword == "end") {
        const auto closed = close_block(parser, content_line);
        if (!closed.ok()) {
          return closed.error();
        }
      } else {
        const auto body = handle_body(parser, content_line, tokens);
        if (!body.ok()) {
          return body.error();
        }
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
      continue;
    }

    if (keyword == "version") {
      if (tokens.size() != 2) {
        return fail(content_line, 1, "version takes exactly one value");
      }
      if (parser.version_seen) {
        return fail(content_line, tokens[0].column, "duplicate version directive");
      }
      std::uint64_t version = 0;
      if (!detail::parse_u64(tokens[1].text, version) || version != kFormatVersion) {
        return fail(content_line, tokens[1].column,
                    "unsupported scenario format version '" +
                        detail::escape_bytes(tokens[1].text) + "'");
      }
      parser.version_seen = true;
    } else if (keyword == "planner") {
      if (tokens.size() != 2) {
        return fail(content_line, 1, "planner takes exactly one value");
      }
      if (!parser.version_seen) {
        return fail(content_line, tokens[0].column, "version must be the first directive");
      }
      if (parser.planner_seen) {
        return fail(content_line, tokens[0].column, "duplicate planner directive");
      }
      auto planner = as_id<PlannerId>(content_line, Field{"planner", tokens[1].text, tokens[1].column});
      if (!planner.ok()) {
        return planner.error();
      }
      parser.planner = planner.take();
      parser.planner_seen = true;
    } else if (keyword == "request") {
      if (!parser.planner_seen) {
        return fail(content_line, tokens[0].column, "planner must be declared before the request");
      }
      if (parser.request_seen) {
        return fail(content_line, tokens[0].column, "duplicate request directive");
      }
      const auto fields = read_fields(content_line, tokens, 1, "request");
      if (!fields.ok()) {
        return fields.error();
      }
      const auto unknown = reject_unknown(
          content_line, fields.value(),
          {"key", "authority", "epoch", "time", "lineage", "rack", "isolation",
           "composition_revision", "kinds"});
      if (!unknown.ok()) {
        return unknown.error();
      }
      const auto key_field = require_field(content_line, fields.value(), "key");
      if (!key_field.ok()) {
        return key_field.error();
      }
      const auto authority_field = require_field(content_line, fields.value(), "authority");
      if (!authority_field.ok()) {
        return authority_field.error();
      }
      const auto time_field = require_field(content_line, fields.value(), "time");
      if (!time_field.ok()) {
        return time_field.error();
      }
      const auto rack_field = require_field(content_line, fields.value(), "rack");
      if (!rack_field.ok()) {
        return rack_field.error();
      }
      const auto isolation_field = require_field(content_line, fields.value(), "isolation");
      if (!isolation_field.ok()) {
        return isolation_field.error();
      }
      const auto revision_field = require_field(content_line, fields.value(), "composition_revision");
      if (!revision_field.ok()) {
        return revision_field.error();
      }
      const auto kinds_field = require_field(content_line, fields.value(), "kinds");
      if (!kinds_field.ok()) {
        return kinds_field.error();
      }
      auto key = as_id<IdempotencyKey>(content_line, **key_field);
      if (!key.ok()) {
        return key.error();
      }
      auto authority = as_id<AuthorityId>(content_line, **authority_field);
      if (!authority.ok()) {
        return authority.error();
      }
      const auto time = as_signed(content_line, **time_field);
      if (!time.ok()) {
        return time.error();
      }
      auto rack = as_id<RackId>(content_line, **rack_field);
      if (!rack.ok()) {
        return rack.error();
      }
      const auto isolation =
          as_enum<IsolationKind>(content_line, **isolation_field, &IsolationKind_from_string);
      if (!isolation.ok()) {
        return isolation.error();
      }
      const auto revision = as_unsigned(content_line, **revision_field);
      if (!revision.ok()) {
        return revision.error();
      }
      std::vector<ObligationKind> kinds;
      const auto kind_items = as_list(content_line, **kinds_field);
      if (!kind_items.ok()) {
        return kind_items.error();
      }
      for (const std::string_view item : kind_items.value()) {
        const auto kind = ObligationKind_from_string(item);
        if (!kind.has_value()) {
          return fail(content_line, (**kinds_field).column,
                      "kinds has unknown value '" + detail::escape_bytes(item) + "'");
        }
        kinds.push_back(kind.value());
      }
      auto isolation_request = IsolationRequest::make(rack.take(), isolation.value(),
                                                      Generation{revision.value()}, kinds);
      if (!isolation_request.ok()) {
        return fail(content_line, 1, isolation_request.error().message);
      }
      Epoch expected_epoch;
      if (const Field* field = find_field(fields.value(), "epoch"); field != nullptr) {
        const auto epoch = as_unsigned(content_line, *field);
        if (!epoch.ok()) {
          return epoch.error();
        }
        expected_epoch = Epoch{epoch.value()};
      }
      if (const Field* field = find_field(fields.value(), "lineage"); field != nullptr) {
        auto lineage = as_lineage(content_line, *field);
        if (!lineage.ok()) {
          return lineage.error();
        }
        parser.lineage = lineage.take();
      }
      parser.key = key.take();
      parser.authority = authority.take();
      parser.evaluation_time = UnixNanos{time.value()};
      parser.isolation = isolation_request.take();
      parser.expected_epoch = expected_epoch;
      parser.request_seen = true;
    } else if (keyword == "source") {
      if (!parser.version_seen) {
        return fail(content_line, tokens[0].column, "version must be the first directive");
      }
      const auto fields = read_fields(content_line, tokens, 1, "source");
      if (!fields.ok()) {
        return fields.error();
      }
      const auto unknown =
          reject_unknown(content_line, fields.value(), {"evidence", "authority", "stream"});
      if (!unknown.ok()) {
        return unknown.error();
      }
      const auto kind_field = require_field(content_line, fields.value(), "evidence");
      if (!kind_field.ok()) {
        return kind_field.error();
      }
      const auto authority_field = require_field(content_line, fields.value(), "authority");
      if (!authority_field.ok()) {
        return authority_field.error();
      }
      const auto stream_field = require_field(content_line, fields.value(), "stream");
      if (!stream_field.ok()) {
        return stream_field.error();
      }
      const auto kind =
          as_enum<EvidenceKind>(content_line, **kind_field, &EvidenceKind_from_string);
      if (!kind.ok()) {
        return kind.error();
      }
      auto authority = as_id<AuthorityId>(content_line, **authority_field);
      if (!authority.ok()) {
        return authority.error();
      }
      auto stream = as_id<StreamId>(content_line, **stream_field);
      if (!stream.ok()) {
        return stream.error();
      }
      for (const SourceDecl& existing : parser.sources) {
        if (existing.kind == kind.value() && existing.stream == stream.value()) {
          return fail(content_line, (**stream_field).column,
                      "duplicate source declaration for this stream");
        }
      }
      parser.sources.push_back(SourceDecl{kind.value(), authority.take(), stream.take()});
    } else if (keyword == "evidence") {
      if (!parser.request_seen) {
        return fail(content_line, tokens[0].column, "evidence must follow the request");
      }
      if (tokens.size() < 4) {
        return fail(content_line, 1,
                    "evidence needs a kind and stream=, generation=, epoch= keys");
      }
      const auto kind = EvidenceKind_from_string(tokens[1].text);
      if (!kind.has_value()) {
        return fail(content_line, tokens[1].column,
                    "unknown evidence kind '" + detail::escape_bytes(tokens[1].text) + "'");
      }
      const auto fields = read_fields(content_line, tokens, 2, "evidence");
      if (!fields.ok()) {
        return fields.error();
      }
      const auto unknown = reject_unknown(content_line, fields.value(),
                                          {"stream", "authority", "generation", "epoch", "digest"});
      if (!unknown.ok()) {
        return unknown.error();
      }
      const auto stream_field = require_field(content_line, fields.value(), "stream");
      if (!stream_field.ok()) {
        return stream_field.error();
      }
      const auto generation_field = require_field(content_line, fields.value(), "generation");
      if (!generation_field.ok()) {
        return generation_field.error();
      }
      const auto epoch_field = require_field(content_line, fields.value(), "epoch");
      if (!epoch_field.ok()) {
        return epoch_field.error();
      }
      auto stream = as_id<StreamId>(content_line, **stream_field);
      if (!stream.ok()) {
        return stream.error();
      }
      const auto generation = as_unsigned(content_line, **generation_field);
      if (!generation.ok()) {
        return generation.error();
      }
      const auto epoch = as_unsigned(content_line, **epoch_field);
      if (!epoch.ok()) {
        return epoch.error();
      }
      // A block whose stream a source directive declares is bound evidence.  A
      // block whose stream is not declared stays in the bundle as unbound
      // evidence, and then it must name the authority that published it.
      const SourceDecl* declaration = find_source(parser, kind.value(), stream.value());
      AuthorityId authority;
      if (declaration != nullptr) {
        authority = declaration->authority;
        if (const Field* declared = find_field(fields.value(), "authority");
            declared != nullptr && declared->value != authority.view()) {
          return fail(content_line, declared->column,
                      "evidence authority disagrees with the source declaration");
        }
      } else {
        const Field* declared = find_field(fields.value(), "authority");
        if (declared == nullptr) {
          return fail(content_line, (**stream_field).column,
                      "evidence block names a stream that no source directive declares, so it "
                      "must name its authority");
        }
        auto parsed_authority = as_id<AuthorityId>(content_line, *declared);
        if (!parsed_authority.ok()) {
          return parsed_authority.error();
        }
        authority = parsed_authority.take();
      }
      parser.block = Block{};
      parser.block.open = true;
      parser.block.kind = kind.value();
      parser.block.authority = std::move(authority);
      parser.block.bound = declaration != nullptr;
      parser.block.stream = stream.take();
      parser.block.generation = Generation{generation.value()};
      parser.block.epoch = Epoch{epoch.value()};
      if (const Field* field = find_field(fields.value(), "digest"); field != nullptr) {
        const auto digest = Digest::from_hex(field->value);
        if (!digest.has_value()) {
          return fail(content_line, field->column,
                      "digest must be 64 hexadecimal characters");
        }
        parser.block.claimed_digest = digest.value();
      }
    } else if (keyword == "end") {
      return fail(content_line, tokens[0].column, "'end' without an open evidence block");
    } else {
      return fail(content_line, tokens[0].column,
                  "unknown directive '" + detail::escape_bytes(keyword) + "'");
    }

    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }

  if (!parser.version_seen) {
    return make_error(ErrorCode::InvalidEncoding, "line 1 column 1: missing version directive",
                      "scenario");
  }
  if (!parser.planner_seen) {
    return make_error(ErrorCode::InvalidEncoding, "line 1 column 1: missing planner directive",
                      "scenario");
  }
  if (!parser.request_seen) {
    return make_error(ErrorCode::InvalidEncoding, "line 1 column 1: missing request directive",
                      "scenario");
  }
  if (parser.block.open) {
    return fail(Line{number, std::string_view()}, 1, "evidence block was not closed with 'end'");
  }

  std::vector<EvidenceSource> sources;
  sources.reserve(parser.sources.size());
  for (const SourceDecl& source : parser.sources) {
    sources.push_back(EvidenceSource{source.kind, StreamRef{source.authority, source.stream}});
  }

  auto bundle = EvidenceBundle::make(std::move(parser.records), limits.max_evidence_records);
  if (!bundle.ok()) {
    return bundle.error();
  }
  auto request =
      PlanRequest::make(parser.key, parser.authority, parser.expected_epoch, parser.evaluation_time,
                        parser.isolation, parser.lineage, std::move(sources), bundle.take());
  if (!request.ok()) {
    return request.error();
  }
  ScenarioDocument document;
  document.planner = parser.planner;
  document.request = request.take();
  return document;
}

Result<ScenarioDocument> load_scenario_file(const std::filesystem::path& path,
                                            const ScenarioLimits& limits) {
  const auto bytes = detail::read_file(path, limits.max_bytes);
  if (!bytes.ok()) {
    return bytes.error();
  }
  const std::string_view text(reinterpret_cast<const char*>(bytes.value().data()),
                              bytes.value().size());
  return parse_scenario(text, limits);
}

std::string render_scenario(const ScenarioDocument& document) {
  std::string out;
  append_line(out, "version " + std::to_string(kFormatVersion));
  append_line(out, "planner " + document.planner.str());

  const PlanRequest& request = document.request;
  append_line(out, "request key=" + request.idempotency_key.str() +
                       " authority=" + request.requested_by.str() +
                       " epoch=" + std::to_string(request.expected_epoch.value()) +
                       " time=" + std::to_string(request.evaluation_time.value()) +
                       " lineage=" + optional_text(request.lineage.str()) +
                       " rack=" + request.isolation.rack.str() +
                       " isolation=" + std::string(to_string(request.isolation.kind)) +
                       " composition_revision=" +
                       std::to_string(request.isolation.composition_revision.value()) +
                       " kinds=" + join_kinds(request.isolation.evacuate_kinds));

  for (const EvidenceSource& source : request.evidence_sources) {
    append_line(out, "source evidence=" + std::string(to_string(source.kind)) +
                         " authority=" + source.source.authority.str() +
                         " stream=" + source.source.stream.str());
  }

  for (const EvidenceRecord& record : request.evidence.records()) {
    bool bound = false;
    for (const EvidenceSource& source : request.evidence_sources) {
      if (source.kind == record.kind && source.source == record.stamp.source) {
        bound = true;
        break;
      }
    }
    const std::string authority = bound ? std::string() : " authority=" + record.stamp.source.authority.str();
    append_line(out, "evidence " + std::string(to_string(record.kind)) +
                         " stream=" + record.stamp.source.stream.str() + authority +
                         " generation=" + std::to_string(record.stamp.generation.value()) +
                         " epoch=" + std::to_string(record.stamp.epoch.value()));
    out += render_block_body(record);
    append_line(out, "end");
  }
  return out;
}

} // namespace rep
