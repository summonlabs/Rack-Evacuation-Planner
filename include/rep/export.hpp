// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef REP_EXPORT_HPP
#define REP_EXPORT_HPP

// The library is built as a static archive by default.  When it is built as a
// shared library on Windows the public surface still needs explicit import /
// export decoration, so REP_API carries the platform spelling.
#if defined(_WIN32) && defined(REP_SHARED)
#if defined(REP_BUILDING_LIBRARY)
#define REP_API __declspec(dllexport)
#else
#define REP_API __declspec(dllimport)
#endif
#else
#define REP_API
#endif

#endif // REP_EXPORT_HPP
