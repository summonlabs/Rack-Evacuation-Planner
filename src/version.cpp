// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rep/version.hpp"

#include <string>

namespace rep {
namespace {

// Supplied by the build system.  Keeping the value in the binary lets a
// consumer detect a header/binary mismatch instead of silently mixing
// releases.
#ifndef REP_BUILD_PROJECT_VERSION
#define REP_BUILD_PROJECT_VERSION "0.0.0-unknown"
#endif

constexpr std::string_view kBuildVersion = REP_BUILD_PROJECT_VERSION;
constexpr std::string_view kHeaderVersion = REP_VERSION_STRING;

static_assert(kHeaderVersion == "1.0.0",
              "Header version must be kept in step with the CMake project version.");

} // namespace

std::string_view version_string() noexcept { return kHeaderVersion; }

std::string_view build_version_string() noexcept { return kBuildVersion; }

std::string_view boundary_string() noexcept {
  return "DCCP boundary " REP_DCCP_BOUNDARY " (" REP_DCCP_BOUNDARY_NAME ")";
}

bool version_is_consistent() noexcept { return kHeaderVersion == kBuildVersion; }

} // namespace rep
