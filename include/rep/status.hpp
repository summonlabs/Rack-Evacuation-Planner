// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_STATUS_HPP
#define REP_STATUS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "rep/export.hpp"

namespace rep {

// Every failure the planner can report.  Codes are stable and are part of the
// observable contract: callers may branch on them and they are serialized.
enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument = 1,
  InvalidIdentifier = 2,
  InvalidEncoding = 3,
  Overflow = 4,
  Underflow = 5,
  NotFound = 6,
  AlreadyExists = 7,
  DuplicateIdentity = 8,
  LimitExceeded = 9,
  DigestMismatch = 10,
  Corrupt = 11,
  Truncated = 12,
  UnsupportedVersion = 13,
  IoError = 14,
  Locked = 15,
  ReparsePoint = 16,
  StaleEpoch = 17,
  StaleGeneration = 18,
  IdempotencyConflict = 19,
  PreconditionFailed = 20,
  Indeterminate = 21,
  CycleDetected = 22,
  CapacityExhausted = 23,
  PolicyDenied = 24,
  Internal = 25,
  VersionMismatch = 26,
  NotADirectory = 27,
};

[[nodiscard]] REP_API std::string_view to_string(ErrorCode code) noexcept;

// A failure value.  Never carries an exception; the public API is
// error-code based so that no exception can cross the boundary.
struct Error {
  ErrorCode code{ErrorCode::Ok};
  std::string message;
  std::string field;

  Error() = default;
  Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
  Error(ErrorCode c, std::string msg, std::string f)
      : code(c), message(std::move(msg)), field(std::move(f)) {}

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::Ok; }
  [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] REP_API Error make_error(ErrorCode code, std::string message);
[[nodiscard]] REP_API Error make_error(ErrorCode code, std::string message, std::string field);

// Minimal expected-style result.  T must be movable.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return error_.code == ErrorCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const T& value() const noexcept { return value_.value(); }
  [[nodiscard]] T& value() noexcept { return value_.value(); }
  [[nodiscard]] T&& take() noexcept { return std::move(value_.value()); }

  [[nodiscard]] const Error& error() const noexcept { return error_; }

 private:
  std::optional<T> value_;
  Error error_;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return error_.code == ErrorCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

 private:
  Error error_;
};

using Status = Result<void>;

} // namespace rep

#endif // REP_STATUS_HPP
