// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_SCENARIO_HPP
#define REP_SCENARIO_HPP

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include "rep/export.hpp"
#include "rep/request.hpp"
#include "rep/status.hpp"
#include "rep/types.hpp"

namespace rep {

// Resource limits applied while parsing.  Every one of them is a hard refusal,
// never a truncation: input that exceeds a limit is rejected with its exact
// location.
struct REP_API ScenarioLimits {
  std::size_t max_bytes{8u * 1024u * 1024u};
  std::size_t max_lines{200000};
  std::size_t max_line_length{8192};
  std::size_t max_list_items{65536};
  std::size_t max_obligations{100000};
  std::size_t max_candidates{100000};
  std::size_t max_evidence_records{4096};
};

struct REP_API ScenarioDocument {
  PlannerId planner;
  PlanRequest request;

  friend bool operator==(const ScenarioDocument& a, const ScenarioDocument& b) noexcept {
    return a.planner == b.planner && a.request == b.request;
  }
};

// Parses the canonical scenario text format.  Strict by design: unknown
// directives, unknown keys, duplicate keys, malformed numbers, invalid
// identifiers, and non-ASCII bytes are refused with a line and column.
//
// Diagnostic message format is exactly "line <N> column <C>: <detail>".
[[nodiscard]] REP_API Result<ScenarioDocument> parse_scenario(
    std::string_view text, const ScenarioLimits& limits = ScenarioLimits{});

[[nodiscard]] REP_API Result<ScenarioDocument> load_scenario_file(
    const std::filesystem::path& path, const ScenarioLimits& limits = ScenarioLimits{});

// Renders the canonical form of a document.  parse(render(d)) == d, and
// render(parse(t)) is byte-identical for any t that parses.
[[nodiscard]] REP_API std::string render_scenario(const ScenarioDocument& document);

} // namespace rep

#endif // REP_SCENARIO_HPP
