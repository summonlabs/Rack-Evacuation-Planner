// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/status.hpp"

#include <array>
#include <string>
#include <utility>

namespace rep {
namespace {

struct CodeName {
  ErrorCode code;
  std::string_view name;
};

constexpr std::array<CodeName, 28> kErrorNames = {{
    {ErrorCode::Ok, "ok"},
    {ErrorCode::InvalidArgument, "invalid_argument"},
    {ErrorCode::InvalidIdentifier, "invalid_identifier"},
    {ErrorCode::InvalidEncoding, "invalid_encoding"},
    {ErrorCode::Overflow, "overflow"},
    {ErrorCode::Underflow, "underflow"},
    {ErrorCode::NotFound, "not_found"},
    {ErrorCode::AlreadyExists, "already_exists"},
    {ErrorCode::DuplicateIdentity, "duplicate_identity"},
    {ErrorCode::LimitExceeded, "limit_exceeded"},
    {ErrorCode::DigestMismatch, "digest_mismatch"},
    {ErrorCode::Corrupt, "corrupt"},
    {ErrorCode::Truncated, "truncated"},
    {ErrorCode::UnsupportedVersion, "unsupported_version"},
    {ErrorCode::IoError, "io_error"},
    {ErrorCode::Locked, "locked"},
    {ErrorCode::ReparsePoint, "reparse_point"},
    {ErrorCode::StaleEpoch, "stale_epoch"},
    {ErrorCode::StaleGeneration, "stale_generation"},
    {ErrorCode::IdempotencyConflict, "idempotency_conflict"},
    {ErrorCode::PreconditionFailed, "precondition_failed"},
    {ErrorCode::Indeterminate, "indeterminate"},
    {ErrorCode::CycleDetected, "cycle_detected"},
    {ErrorCode::CapacityExhausted, "capacity_exhausted"},
    {ErrorCode::PolicyDenied, "policy_denied"},
    {ErrorCode::Internal, "internal"},
    {ErrorCode::VersionMismatch, "version_mismatch"},
    {ErrorCode::NotADirectory, "not_a_directory"},
}};

} // namespace

std::string_view to_string(ErrorCode code) noexcept {
  for (const CodeName& entry : kErrorNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "unknown";
}

std::string Error::to_string() const {
  std::string result(rep::to_string(code));
  if (!field.empty()) {
    result += " [" + field + "]";
  }
  if (!message.empty()) {
    result += ": " + message;
  }
  return result;
}

Error make_error(ErrorCode code, std::string message) {
  return Error{code, std::move(message), std::string()};
}

Error make_error(ErrorCode code, std::string message, std::string field) {
  return Error{code, std::move(message), std::move(field)};
}

} // namespace rep
