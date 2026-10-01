// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_VERSION_HPP
#define REP_VERSION_HPP

#include <string_view>

#include "rep/export.hpp"

#define REP_VERSION_MAJOR 1
#define REP_VERSION_MINOR 0
#define REP_VERSION_PATCH 1

// Textual form of the compile-time version, e.g. "1.2.3".
#define REP_VERSION_STRING "1.0.1"

// Stable identifier of the boundary this implementation claims to own.
#define REP_DCCP_BOUNDARY "55"
#define REP_DCCP_BOUNDARY_NAME "Rack Evacuation Planner"

namespace rep {

// Version of the headers that were compiled into the caller.
[[nodiscard]] REP_API std::string_view version_string() noexcept;

// Version of the library binary that was linked.  Differs from
// version_string() only if a caller mixed headers and binaries from different
// releases; the engine refuses to start in that case.
[[nodiscard]] REP_API std::string_view build_version_string() noexcept;

// Boundary identity: "DCCP boundary 55 (Rack Evacuation Planner)".
[[nodiscard]] REP_API std::string_view boundary_string() noexcept;

// Verifies that header and binary versions agree.  Returns false when the
// caller compiled against different headers than the linked library.
[[nodiscard]] REP_API bool version_is_consistent() noexcept;

} // namespace rep

#endif // REP_VERSION_HPP
