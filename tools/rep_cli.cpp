// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// rep_cli: command line front end for the library.  "rep_cli help" prints the
// subcommand list and the exit codes.
//
// Reports go to stdout, diagnostics to stderr.  No clock is read: the only
// timestamp handed to the library is the scenario's own evaluation time, used
// as the engine's epoch-claim clock because the engine requires one.

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "rep/rep.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitNotSafe = 1;
constexpr int kExitInput = 2;
constexpr int kExitFailure = 3;

void print_usage(std::ostream& out) {
  out << "rep_cli - Rack Evacuation Planner command line front end\n"
         "usage:\n"
         "  rep_cli version\n"
         "  rep_cli render --scenario <file>\n"
         "  rep_cli plan --scenario <file> [--store <dir>] [--planner <id>] [--out <file>]\n"
         "  rep_cli verify --scenario <file> --plan <file>\n"
         "  rep_cli inspect --store <dir>\n"
         "  rep_cli help\n"
         "exit codes: 0 success; 1 not safe or verification failed; 2 bad input or\n"
         "  rejected request; 3 engine, store, or IO failure\n";
}

void diag(std::string_view message) { std::cerr << "rep_cli: " << message << "\n"; }

// Canonical unsigned decimal: at least one digit, no sign, no whitespace, no
// leading zero unless the value is exactly "0", and no wrap.
[[nodiscard]] std::optional<std::uint64_t> parse_decimal(std::string_view text) {
  if (text.empty() || (text.size() > 1 && text.front() == '0')) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char* const last = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(text.data(), last, value, 10);
  if (result.ec != std::errc{} || result.ptr != last) {
    return std::nullopt;
  }
  return value;
}

// Splits one line on single spaces.  A canonical line never has a leading,
// trailing, or repeated space, so any of those is a hard error.
[[nodiscard]] bool split_tokens(std::string_view line, std::vector<std::string_view>& tokens,
                                std::string& error) {
  if (line.empty()) {
    error = "empty line";
    return false;
  }
  if (line.front() == ' ' || line.back() == ' ') {
    error = "line has a leading or trailing space";
    return false;
  }
  std::size_t start = 0;
  while (true) {
    const std::size_t space = line.find(' ', start);
    if (space == std::string_view::npos) {
      tokens.push_back(line.substr(start));
      return true;
    }
    if (space == start) {
      error = "line repeats a space";
      return false;
    }
    tokens.push_back(line.substr(start, space - start));
    start = space + 1;
  }
}

// The key=value fields of one record line.  Every field must be consumed
// exactly once by the parser of its record: a repeated consumption, a missing
// field, or a field the parser never asks for is a hard error.  That is what
// makes the accepted field set exact without listing it up front, and it is
// the only thing this reader does not take from the library itself.
class Fields {
 public:
  explicit Fields(const std::vector<std::string_view>& tokens) {
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::size_t separator = tokens[i].find('=');
      if (separator == std::string_view::npos || separator == 0) {
        error_ = "token '" + std::string(tokens[i]) + "' is not key=value";
        return;
      }
      entries_.push_back(Entry{tokens[i].substr(0, separator), tokens[i].substr(separator + 1)});
    }
    valid_ = true;
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] bool reject(std::string message) {
    error_ = std::move(message);
    return false;
  }

  // Fields are consumed in the exact order the emitter writes them.
  [[nodiscard]] bool take(std::string_view key, std::string_view& value) {
    if (cursor_ >= entries_.size()) {
      return reject("missing field '" + std::string(key) + "'");
    }
    const Entry& entry = entries_[cursor_];
    if (entry.key != key) {
      return reject("expected field '" + std::string(key) + "', found '" +
                    std::string(entry.key) + "'");
    }
    ++cursor_;
    value = entry.value;
    return true;
  }

  [[nodiscard]] bool exhausted() {
    if (cursor_ != entries_.size()) {
      return reject("unexpected field '" + std::string(entries_[cursor_].key) + "'");
    }
    return true;
  }

  template <class Id>
  [[nodiscard]] bool id(std::string_view key, Id& out) {
    std::string_view value;
    if (!take(key, value)) {
      return false;
    }
    const auto parsed = Id::parse(value);
    if (!parsed.ok()) {
      return reject("field '" + std::string(key) + "': " + parsed.error().message);
    }
    out = parsed.value();
    return true;
  }

  template <class Enum, class Parse>
  [[nodiscard]] bool enumeration(std::string_view key, Enum& out, Parse parse) {
    std::string_view value;
    if (!take(key, value)) {
      return false;
    }
    const std::optional<Enum> parsed = parse(value);
    if (!parsed.has_value()) {
      return reject("field '" + std::string(key) + "': unknown token '" + std::string(value) +
                    "'");
    }
    out = *parsed;
    return true;
  }

  template <class Strong>
  [[nodiscard]] bool strong(std::string_view key, Strong& out) {
    using Value = typename Strong::value_type;
    static_assert(std::is_unsigned_v<Value>, "plan text counters are unsigned");
    std::string_view value;
    if (!take(key, value)) {
      return false;
    }
    const std::optional<std::uint64_t> parsed = parse_decimal(value);
    if (!parsed.has_value() ||
        *parsed > static_cast<std::uint64_t>((std::numeric_limits<Value>::max)())) {
      return reject("field '" + std::string(key) + "': '" + std::string(value) +
                    "' is not a counter in range");
    }
    out = Strong(static_cast<Value>(*parsed));
    return true;
  }

  [[nodiscard]] bool counter(std::string_view key, std::uint64_t& out) {
    std::string_view value;
    if (!take(key, value)) {
      return false;
    }
    const std::optional<std::uint64_t> parsed = parse_decimal(value);
    if (!parsed.has_value()) {
      return reject("field '" + std::string(key) + "': '" + std::string(value) +
                    "' is not an unsigned decimal");
    }
    out = *parsed;
    return true;
  }

  [[nodiscard]] bool counter32(std::string_view key, std::uint32_t& out) {
    std::uint64_t wide = 0;
    if (!counter(key, wide)) {
      return false;
    }
    if (wide > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
      return reject("field '" + std::string(key) + "': counter does not fit 32 bits");
    }
    out = static_cast<std::uint32_t>(wide);
    return true;
  }

  [[nodiscard]] bool digest(std::string_view key, rep::Digest& out) {
    std::string_view value;
    if (!take(key, value)) {
      return false;
    }
    const std::optional<rep::Digest> parsed = rep::Digest::from_hex(value);
    if (!parsed.has_value()) {
      return reject("field '" + std::string(key) +
                    "': expected exactly 64 hexadecimal characters");
    }
    out = *parsed;
    return true;
  }

 private:
  struct Entry {
    std::string_view key;
    std::string_view value;
  };

  std::vector<Entry> entries_;
  std::string error_;
  std::size_t cursor_{0};
  bool valid_{false};
};

// ---------------------------------------------------------------------------
// Canonical plan text reader
// ---------------------------------------------------------------------------
//
// Plan::to_text() is the only writer of this format and it is deterministic:
// every line is "record key=value key=value ..." ended by one line feed, and
// the records appear in exactly this order, each carrying exactly the fields
// its parser below consumes:
//
//   plan          id planner revision sequence epoch lineage idempotency_key
//                 request_digest
//   source        rack composition_revision isolation evacuate_kinds status
//                 status_verdict obligations waves plan_digest
//   binding       kind authority stream generation epoch digest
//   indeterminacy reason
//   out_of_scope  obligation kind reason
//   residual      obligation kind reason
//   rejected      obligation candidate reason
//   assignment    order obligation kind candidate action destination authority
//                 wave
//
// Strictness: the record order above is mandatory and plan and source appear
// exactly once; each record must carry exactly the fields listed for it, in
// the order shown; the derived counters and the status-only verdict on the
// source record are recomputed from the rebuilt plan and compared; list
// entries keep their file order, so Plan::verify() refuses a permuted file
// instead of repairing it; and a rejected record belongs to the residual
// naming the same obligation, in file order.
//
// The reader never certifies a plan: the caller recomputes
// Plan::content_digest(), compares it with plan_digest, and runs
// Plan::verify().  A text whose digest does not match is refused, never
// repaired.

constexpr int kRankPlan = 0;
constexpr int kRankSource = 1;
constexpr int kRankBinding = 2;
constexpr int kRankIndeterminacy = 3;
constexpr int kRankOutOfScope = 4;
constexpr int kRankResidual = 5;
constexpr int kRankRejected = 6;
constexpr int kRankAssignment = 7;

[[nodiscard]] int section_rank(std::string_view keyword) noexcept {
  if (keyword == "plan") return kRankPlan;
  if (keyword == "source") return kRankSource;
  if (keyword == "binding") return kRankBinding;
  if (keyword == "indeterminacy") return kRankIndeterminacy;
  if (keyword == "out_of_scope") return kRankOutOfScope;
  if (keyword == "residual") return kRankResidual;
  if (keyword == "rejected") return kRankRejected;
  if (keyword == "assignment") return kRankAssignment;
  return -1;
}

[[nodiscard]] bool fail_line(std::size_t line_number, std::string detail, std::string& error) {
  error = "line " + std::to_string(line_number) + ": " + std::move(detail);
  return false;
}

// The summary facts the source record states about the plan it describes.
struct SourceSummary {
  std::uint64_t obligations{0};
  std::uint64_t waves{0};
  rep::SafetyVerdict verdict{};
};

[[nodiscard]] bool parse_plan_record(Fields& f, rep::Plan& plan) {
  return f.id("id", plan.id) && f.id("planner", plan.planner) &&
         f.strong("revision", plan.revision) && f.strong("sequence", plan.sequence) &&
         f.strong("epoch", plan.epoch) && f.id("lineage", plan.lineage) &&
         f.id("idempotency_key", plan.idempotency_key) &&
         f.digest("request_digest", plan.request_digest);
}

[[nodiscard]] bool parse_source_record(Fields& f, rep::Plan& plan, SourceSummary& summary) {
  if (!f.id("rack", plan.source_rack) ||
      !f.strong("composition_revision", plan.composition_revision) ||
      !f.enumeration("isolation", plan.isolation, &rep::IsolationKind_from_string)) {
    return false;
  }
  std::string_view kinds;
  if (!f.take("evacuate_kinds", kinds)) {
    return false;
  }
  if (kinds.empty()) return f.reject("field 'evacuate_kinds': at least one kind is required");
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = kinds.find(',', start);
    const std::string_view token =
        comma == std::string_view::npos ? kinds.substr(start) : kinds.substr(start, comma - start);
    const std::optional<rep::ObligationKind> kind = rep::ObligationKind_from_string(token);
    if (!kind.has_value()) {
      return f.reject("field 'evacuate_kinds': unknown token '" + std::string(token) + "'");
    }
    plan.evacuate_kinds.push_back(*kind);
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return f.enumeration("status", plan.status, &rep::PlanStatus_from_string) &&
         f.enumeration("status_verdict", summary.verdict, &rep::SafetyVerdict_from_string) &&
         f.counter("obligations", summary.obligations) && f.counter("waves", summary.waves) &&
         f.digest("plan_digest", plan.plan_digest);
}

[[nodiscard]] bool parse_binding_record(Fields& f, rep::PlanBinding& binding) {
  return f.enumeration("kind", binding.kind, &rep::EvidenceKind_from_string) &&
         f.id("authority", binding.stream.authority) && f.id("stream", binding.stream.stream) &&
         f.strong("generation", binding.generation) && f.strong("epoch", binding.epoch) &&
         f.digest("digest", binding.digest);
}

[[nodiscard]] bool parse_indeterminacy_record(Fields& f, rep::IndeterminacyReason& reason) {
  return f.enumeration("reason", reason, &rep::IndeterminacyReason_from_string);
}

[[nodiscard]] bool parse_out_of_scope_record(Fields& f, rep::ScopeExclusion& exclusion) {
  return f.id("obligation", exclusion.obligation) &&
         f.enumeration("kind", exclusion.kind, &rep::ObligationKind_from_string) &&
         f.enumeration("reason", exclusion.reason, &rep::ScopeExclusionReason_from_string);
}

[[nodiscard]] bool parse_residual_record(Fields& f, rep::Residual& residual) {
  return f.id("obligation", residual.obligation) &&
         f.enumeration("kind", residual.kind, &rep::ObligationKind_from_string) &&
         f.enumeration("reason", residual.reason, &rep::ResidualReason_from_string);
}

[[nodiscard]] bool parse_rejected_record(Fields& f, rep::ObligationId& obligation,
                                         rep::RejectedCandidate& rejected) {
  return f.id("obligation", obligation) && f.id("candidate", rejected.candidate) &&
         f.enumeration("reason", rejected.reason, &rep::RejectionReason_from_string);
}

[[nodiscard]] bool parse_assignment_record(Fields& f, rep::Assignment& assignment) {
  if (!f.counter32("order", assignment.order_index) ||
      !f.id("obligation", assignment.obligation) ||
      !f.enumeration("kind", assignment.kind, &rep::ObligationKind_from_string) ||
      !f.id("candidate", assignment.candidate) ||
      !f.enumeration("action", assignment.action, &rep::ActionKind_from_string)) {
    return false;
  }
  std::string_view destination;
  if (!f.take("destination", destination)) {
    return false;
  }
  const auto parsed = rep::DestinationRef::parse(destination);
  if (!parsed.ok()) {
    return f.reject("field 'destination': " + parsed.error().message);
  }
  assignment.destination = parsed.value();
  return f.id("authority", assignment.authority) && f.counter32("wave", assignment.wave);
}

// Validates one line's bytes and splits it into space separated tokens.
[[nodiscard]] bool check_line(std::string_view line, std::vector<std::string_view>& tokens,
                              std::string& error) {
  for (const char raw : line) {
    const auto byte = static_cast<unsigned char>(raw);
    if (byte < 0x20 || byte > 0x7e) {
      error = "line holds a byte that is not printable ASCII";
      return false;
    }
  }
  return split_tokens(line, tokens, error);
}

[[nodiscard]] bool parse_plan_text(std::string_view text, rep::Plan& plan, std::string& error) {
  if (text.empty() || text.back() != '\n') {
    error = "plan text must be non-empty and end with a line feed";
    return false;
  }
  SourceSummary summary;
  std::map<std::string, std::size_t> residuals;
  bool have_plan = false;
  bool have_source = false;
  int section = -1;
  std::size_t line_number = 0;
  std::size_t position = 0;

  while (position < text.size()) {
    const std::size_t newline = text.find('\n', position);
    const std::string_view line = text.substr(position, newline - position);
    position = newline + 1;
    ++line_number;

    std::vector<std::string_view> tokens;
    std::string detail;
    if (!check_line(line, tokens, detail)) {
      return fail_line(line_number, detail, error);
    }
    const std::string_view keyword = tokens.front();
    const int rank = section_rank(keyword);
    if (rank < 0) {
      return fail_line(line_number, "unknown record keyword '" + std::string(keyword) + "'", error);
    }
    if (rank < section) {
      return fail_line(line_number, "record '" + std::string(keyword) + "' is out of order", error);
    }
    if ((rank == kRankPlan && have_plan) || (rank == kRankSource && have_source)) {
      return fail_line(line_number, "record '" + std::string(keyword) + "' appears twice", error);
    }
    if (line_number == 1u && rank != kRankPlan) {
      return fail_line(line_number, "the first record must be the plan record", error);
    }
    if (line_number == 2u && rank != kRankSource) {
      return fail_line(line_number, "the second record must be the source record", error);
    }
    section = rank;

    Fields fields(tokens);
    if (!fields.valid()) {
      return fail_line(line_number, fields.error(), error);
    }
    bool ok = false;
    switch (rank) {
      case kRankPlan:
        ok = parse_plan_record(fields, plan);
        have_plan = ok;
        break;
      case kRankSource:
        ok = parse_source_record(fields, plan, summary);
        have_source = ok;
        break;
      case kRankBinding: {
        rep::PlanBinding binding;
        ok = parse_binding_record(fields, binding);
        if (ok) {
          plan.bindings.push_back(std::move(binding));
        }
        break;
      }
      case kRankIndeterminacy: {
        rep::IndeterminacyReason reason{};
        ok = parse_indeterminacy_record(fields, reason);
        if (ok) {
          plan.indeterminacy_reasons.push_back(reason);
        }
        break;
      }
      case kRankOutOfScope: {
        rep::ScopeExclusion exclusion;
        ok = parse_out_of_scope_record(fields, exclusion);
        if (ok) {
          plan.out_of_scope.push_back(std::move(exclusion));
        }
        break;
      }
      case kRankResidual: {
        rep::Residual residual;
        ok = parse_residual_record(fields, residual);
        if (ok) {
          residuals.emplace(residual.obligation.str(), plan.residuals.size());
          plan.residuals.push_back(std::move(residual));
        }
        break;
      }
      case kRankRejected: {
        rep::ObligationId obligation;
        rep::RejectedCandidate rejected;
        ok = parse_rejected_record(fields, obligation, rejected);
        if (ok) {
          const auto found = residuals.find(obligation.str());
          if (found == residuals.end()) {
            ok = fields.reject("rejected candidate for '" + obligation.str() +
                               "' has no residual record");
          } else {
            plan.residuals[found->second].rejected.push_back(std::move(rejected));
          }
        }
        break;
      }
      default: {
        rep::Assignment assignment;
        ok = parse_assignment_record(fields, assignment);
        if (ok) {
          plan.assignments.push_back(std::move(assignment));
        }
        break;
      }
    }
    if (!ok || !fields.exhausted()) {
      return fail_line(line_number, fields.error(), error);
    }
  }

  if (!have_plan || !have_source) {
    error = "plan text is missing the plan record or the source record";
    return false;
  }
  if (summary.obligations != static_cast<std::uint64_t>(plan.obligation_count())) {
    error = "source record states obligations=" + std::to_string(summary.obligations) +
            " but the records hold " + std::to_string(plan.obligation_count());
    return false;
  }
  if (summary.waves != static_cast<std::uint64_t>(plan.wave_count())) {
    error = "source record states waves=" + std::to_string(summary.waves) +
            " but the records hold " + std::to_string(plan.wave_count());
    return false;
  }
  if (!(summary.verdict == plan.verdict())) {
    error = "source record states status_verdict=" + std::string(rep::to_string(summary.verdict)) +
            " but the records give " + std::string(rep::to_string(plan.verdict()));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Files and paths
// ---------------------------------------------------------------------------

// Arguments arrive as UTF-8, so they reach the filesystem as u8string, the
// portable C++20 spelling of a UTF-8 path.
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text) {
  const auto* first = reinterpret_cast<const char8_t*>(text.data());
  return std::filesystem::path(std::u8string(first, first + text.size()));
}

[[nodiscard]] bool read_text_file(const std::filesystem::path& path, std::string& text,
                                  std::string& error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    error = "cannot open the file for reading";
    return false;
  }
  std::string content;
  char buffer[4096];
  while (stream.read(buffer, static_cast<std::streamsize>(sizeof(buffer))) ||
         stream.gcount() > 0) {
    content.append(buffer, static_cast<std::size_t>(stream.gcount()));
  }
  if (stream.bad()) {
    error = "read failure";
    return false;
  }
  text = std::move(content);
  return true;
}

[[nodiscard]] bool write_text_file(const std::filesystem::path& path, std::string_view text,
                                   std::string& error) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    error = "cannot open the file for writing";
    return false;
  }
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.flush();
  if (!stream) {
    error = "write failure";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

struct Options {
  std::string command;
  std::optional<std::string> scenario;
  std::optional<std::string> store;
  std::optional<std::string> planner;
  std::optional<std::string> out;
  std::optional<std::string> plan;
};

[[nodiscard]] bool parse_options(const std::vector<std::string>& args, Options& options,
                                 std::string& error) {
  options.command = args.size() > 1 ? args[1] : "help";
  for (std::size_t i = 2; i < args.size(); ++i) {
    const std::string& token = args[i];
    std::optional<std::string>* slot = nullptr;
    if (token == "--scenario") {
      slot = &options.scenario;
    } else if (token == "--store") {
      slot = &options.store;
    } else if (token == "--planner") {
      slot = &options.planner;
    } else if (token == "--out") {
      slot = &options.out;
    } else if (token == "--plan") {
      slot = &options.plan;
    } else {
      error = "unknown argument '" + token + "'";
      return false;
    }
    if (slot->has_value()) {
      error = "argument '" + token + "' is given more than once";
      return false;
    }
    if (i + 1 >= args.size() || args[i + 1].empty() || args[i + 1].rfind("--", 0) == 0) {
      error = "argument '" + token + "' requires a value";
      return false;
    }
    *slot = args[i + 1];
    ++i;
  }
  return true;
}

[[nodiscard]] bool check_flags(const Options& options,
                               std::initializer_list<std::string_view> allowed,
                               std::string& error) {
  const std::pair<std::string_view, bool> flags[] = {
      {"--scenario", options.scenario.has_value()},
      {"--store", options.store.has_value()},
      {"--planner", options.planner.has_value()},
      {"--out", options.out.has_value()},
      {"--plan", options.plan.has_value()},
  };
  for (const std::pair<std::string_view, bool>& flag : flags) {
    if (flag.second && std::find(allowed.begin(), allowed.end(), flag.first) == allowed.end()) {
      error =
          "argument '" + std::string(flag.first) + "' is not accepted by '" + options.command + "'";
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool load_scenario(const Options& options, rep::ScenarioDocument& document,
                                 int& exit_code) {
  if (!options.scenario.has_value()) {
    diag("--scenario is required");
    exit_code = kExitInput;
    return false;
  }
  const auto loaded = rep::load_scenario_file(path_from_utf8(*options.scenario));
  if (!loaded.ok()) {
    diag("--scenario " + *options.scenario + ": " + loaded.error().to_string());
    exit_code = kExitInput;
    return false;
  }
  document = loaded.value();
  return true;
}

void print_error(const rep::Error& error) {
  std::cout << "error: " << error.to_string() << "\n";
  std::cout << "error code: " << rep::to_string(error.code) << "\n";
  if (!error.field.empty()) std::cout << "error field: " << error.field << "\n";
  if (!error.message.empty()) std::cout << "error message: " << error.message << "\n";
}

// A rejection is a verdict about the request; anything else the engine returns
// is an engine failure.
[[nodiscard]] bool is_request_rejection(rep::ErrorCode code) noexcept {
  switch (code) {
    case rep::ErrorCode::InvalidArgument:
    case rep::ErrorCode::InvalidIdentifier:
    case rep::ErrorCode::InvalidEncoding:
    case rep::ErrorCode::LimitExceeded:
    case rep::ErrorCode::DigestMismatch:
    case rep::ErrorCode::StaleEpoch:
    case rep::ErrorCode::StaleGeneration:
    case rep::ErrorCode::IdempotencyConflict:
    case rep::ErrorCode::PreconditionFailed:
    case rep::ErrorCode::Indeterminate:
    case rep::ErrorCode::PolicyDenied:
    case rep::ErrorCode::CapacityExhausted:
      return true;
    default:
      return false;
  }
}

void print_staleness(std::span<const rep::StalenessFinding> findings) {
  std::cout << "staleness findings: " << findings.size() << "\n";
  for (const rep::StalenessFinding& finding : findings) {
    std::cout << "  stream=" << finding.stream.to_text() << " kind=" << rep::to_string(finding.kind)
              << " plan-generation=" << finding.plan_generation.value()
              << " current-generation=" << finding.current_generation.value()
              << " plan-epoch=" << finding.plan_epoch.value()
              << " current-epoch=" << finding.current_epoch.value() << "\n";
  }
}

void print_audit(std::span<const rep::AuditFinding> findings) {
  std::cout << "audit findings: " << findings.size() << "\n";
  for (const rep::AuditFinding& finding : findings) {
    std::cout << "  " << finding.code << ": " << finding.detail << "\n";
  }
}

void print_text(const std::string& text) {
  std::cout << text;
  if (text.empty() || text.back() != '\n') std::cout << "\n";
}

// ---------------------------------------------------------------------------
// Subcommands
// ---------------------------------------------------------------------------

int command_version(const Options& options) {
  std::string error;
  if (!check_flags(options, {}, error)) {
    diag(error);
    return kExitInput;
  }
  const std::string_view version = rep::version_string();
  std::cout << REP_DCCP_BOUNDARY_NAME << " " << version << " (DCCP boundary " << REP_DCCP_BOUNDARY
            << ")\n";
  std::cout << "boundary: " << rep::boundary_string() << "\n";
  std::cout << "header version: " << version << "\n";
  std::cout << "binary version: " << rep::build_version_string() << "\n";
  std::cout << "versions consistent: " << (rep::version_is_consistent() ? "yes" : "no") << "\n";
  return kExitOk;
}

int command_render(const Options& options) {
  std::string error;
  if (!check_flags(options, {"--scenario"}, error)) {
    diag(error);
    return kExitInput;
  }
  rep::ScenarioDocument document;
  int exit_code = kExitInput;
  if (!load_scenario(options, document, exit_code)) return exit_code;
  print_text(rep::render_scenario(document));
  return kExitOk;
}

int command_plan(const Options& options) {
  std::string error;
  if (!check_flags(options, {"--scenario", "--store", "--planner", "--out"}, error)) {
    diag(error);
    return kExitInput;
  }
  rep::ScenarioDocument document;
  int exit_code = kExitInput;
  if (!load_scenario(options, document, exit_code)) return exit_code;

  rep::PlannerId planner = document.planner;
  if (options.planner.has_value()) {
    const auto parsed = rep::PlannerId::parse(*options.planner);
    if (!parsed.ok()) {
      diag("--planner: " + parsed.error().to_string());
      return kExitInput;
    }
    planner = parsed.value();
  }

  // The engine needs one timestamp for the epoch claim.  The planner never
  // reads a clock, and neither does this tool: the scenario's own evaluation
  // time is the only instant in play.
  const rep::UnixNanos evaluation_time = document.request.evaluation_time;
  rep::EngineOptions engine_options;
  engine_options.planner = planner;
  if (options.store.has_value()) engine_options.store_directory = path_from_utf8(*options.store);
  engine_options.wall_clock = [evaluation_time]() { return evaluation_time; };

  auto engine = rep::PlanEngine::open(std::move(engine_options));
  if (!engine.ok()) {
    diag("engine open failed: " + engine.error().to_string());
    return kExitFailure;
  }
  auto submitted = engine.value()->submit(document.request);
  if (!submitted.ok()) {
    print_error(submitted.error());
    diag("submission refused: " + submitted.error().to_string());
    return is_request_rejection(submitted.error().code) ? kExitInput : kExitFailure;
  }

  const rep::PlanOutcome& outcome = submitted.value();
  std::cout << "outcome: " << rep::to_string(outcome.kind) << "\n";
  if (!outcome.committed()) {
    print_error(outcome.error);
    diag("request rejected: " + outcome.error.to_string());
    return kExitInput;
  }

  const rep::Plan& plan = outcome.plan;
  const auto verified = plan.verify();
  if (!verified.ok()) {
    diag("the engine published a plan that does not verify: " + verified.error().to_string());
    return kExitFailure;
  }
  const std::vector<rep::StalenessFinding> findings = plan.staleness(document.request.evidence);
  const rep::SafetyVerdict verdict = plan.verdict(findings);
  const std::vector<rep::AuditFinding> audit = rep::audit_plan(plan, document.request);
  const std::string plan_text = plan.to_text();

  std::cout << "plan-id: " << plan.id.str() << "\n";
  std::cout << "plan-status: " << rep::to_string(plan.status) << "\n";
  std::cout << "verdict: " << rep::to_string(verdict) << "\n";
  print_staleness(findings);
  std::cout << "plan:\n" << plan_text;
  print_audit(audit);
  if (options.out.has_value() && !write_text_file(path_from_utf8(*options.out), plan_text, error)) {
    diag("--out " + *options.out + ": " + error);
    return kExitFailure;
  }
  return verdict == rep::SafetyVerdict::Safe ? kExitOk : kExitNotSafe;
}

int command_verify(const Options& options) {
  std::string error;
  if (!check_flags(options, {"--scenario", "--plan"}, error)) {
    diag(error);
    return kExitInput;
  }
  if (!options.plan.has_value()) {
    diag("--plan is required");
    return kExitInput;
  }
  rep::ScenarioDocument document;
  int exit_code = kExitInput;
  if (!load_scenario(options, document, exit_code)) return exit_code;

  std::string text;
  if (!read_text_file(path_from_utf8(*options.plan), text, error)) {
    diag("--plan " + *options.plan + ": " + error);
    return kExitFailure;
  }
  rep::Plan plan;
  if (!parse_plan_text(text, plan, error)) {
    diag("--plan " + *options.plan + ": " + error);
    return kExitInput;
  }

  std::cout << "plan-id: " << plan.id.str() << "\n";
  const rep::Digest recomputed = plan.content_digest();
  const bool digest_verified = recomputed == plan.plan_digest;
  std::cout << "plan-digest: " << plan.plan_digest.to_hex() << "\n";
  if (!digest_verified) {
    std::cout << "recomputed-digest: " << recomputed.to_hex() << "\n";
    diag("plan digest does not match the recomputed content digest");
  }
  const auto verified = plan.verify();
  if (!verified.ok()) diag("plan verification failed: " + verified.error().to_string());

  const std::vector<rep::StalenessFinding> findings = plan.staleness(document.request.evidence);
  const std::vector<rep::AuditFinding> audit = rep::audit_plan(plan, document.request);
  std::cout << "plan-status: " << rep::to_string(plan.status) << "\n";
  std::cout << "verdict: " << rep::to_string(plan.verdict(findings)) << "\n";
  print_staleness(findings);
  print_audit(audit);

  const bool ok = digest_verified && verified.ok() && findings.empty() && audit.empty();
  std::cout << "verification: " << (ok ? "ok" : "failed") << "\n";
  return ok ? kExitOk : kExitNotSafe;
}

int command_inspect(const Options& options) {
  std::string error;
  if (!check_flags(options, {"--store"}, error)) {
    diag(error);
    return kExitInput;
  }
  if (!options.store.has_value()) {
    diag("--store is required");
    return kExitInput;
  }
  const auto stats = rep::PlanStore::inspect(path_from_utf8(*options.store));
  if (!stats.ok()) {
    diag("--store " + *options.store + ": " + stats.error().to_string());
    return kExitFailure;
  }
  print_text(stats.value().to_text());
  return kExitOk;
}

int run(const std::vector<std::string>& args) {
  Options options;
  std::string error;
  if (!parse_options(args, options, error)) {
    diag(error);
    print_usage(std::cerr);
    return kExitInput;
  }
  if (args.size() <= 1u || options.command == "help") {
    print_usage(std::cout);
    return kExitOk;
  }
  if (options.command == "version") return command_version(options);
  if (options.command == "render") return command_render(options);
  if (options.command == "plan") return command_plan(options);
  if (options.command == "verify") return command_verify(options);
  if (options.command == "inspect") return command_inspect(options);
  diag("unknown subcommand '" + options.command + "'");
  print_usage(std::cerr);
  return kExitInput;
}

}  // namespace

#if defined(_WIN32)
// argv arrives in the process code page; this tool speaks UTF-8, so the wide
// entry point is converted once here and nothing below sees a code page string.
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    const std::u8string utf8 = std::filesystem::path(argv[i]).u8string();
    args.emplace_back(reinterpret_cast<const char*>(utf8.data()), utf8.size());
  }
  return run(args);
}
#else
int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
  return run(args);
}
#endif
