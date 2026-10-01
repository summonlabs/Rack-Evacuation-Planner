cmake_minimum_required(VERSION 3.20)

project(RackEvacuationPlanner
  VERSION 1.0.1
  DESCRIPTION "Deterministic rack evacuation planning (DCCP boundary 55)"
  HOMEPAGE_URL "https://github.com/summonlabs/Rack-Evacuation-Planner"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(REP_BUILD_TESTS "Build the Rack Evacuation Planner test suite" ON)
option(REP_BUILD_TOOLS "Build the Rack Evacuation Planner command line tools" ON)
option(REP_BUILD_EXAMPLES "Build the Rack Evacuation Planner examples" ON)
option(REP_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(REP_ENABLE_ASAN "Build with AddressSanitizer (MSVC /fsanitize=address)" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  # Deterministic, standards-conformant translation units.
  add_compile_options(/utf-8 /Zc:__cplusplus)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# ---------------------------------------------------------------------------
# First-party warning policy.  Applies to every target owned by this project.
# ---------------------------------------------------------------------------
function(rep_apply_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(REP_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(BUILD_SHARED_LIBS)
      # Exported classes carry STL members; the DLL-interface warning is not
      # actionable for header-only instantiations in the public API.
      target_compile_options(${target} PRIVATE /wd4251)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast)
    if(REP_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(rep_apply_sanitizers target)
  if(NOT REP_ENABLE_ASAN)
    return()
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /fsanitize=address)
    target_link_options(${target} PRIVATE /fsanitize=address)
  else()
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endfunction()

if(MSVC AND REP_ENABLE_ASAN)
  # /RTC1 is incompatible with /fsanitize=address; drop it from Debug flags.
  string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(REP_PUBLIC_HEADERS
  include/rep/candidate.hpp
  include/rep/canonical.hpp
  include/rep/digest.hpp
  include/rep/engine.hpp
  include/rep/evidence.hpp
  include/rep/export.hpp
  include/rep/obligation.hpp
  include/rep/plan.hpp
  include/rep/policy.hpp
  include/rep/rep.hpp
  include/rep/request.hpp
  include/rep/scenario.hpp
  include/rep/status.hpp
  include/rep/store.hpp
  include/rep/types.hpp
  include/rep/version.hpp)

set(REP_SOURCES
  src/adjacent.cpp
  src/canonical.cpp
  src/candidate.cpp
  src/digest.cpp
  src/engine.cpp
  src/evidence.cpp
  src/obligation.cpp
  src/plan.cpp
  src/planner.cpp
  src/platform.cpp
  src/policy.cpp
  src/request.cpp
  src/scenario.cpp
  src/status.cpp
  src/store.cpp
  src/store_format.cpp
  src/types.cpp
  src/version.cpp)

add_library(rep ${REP_SOURCES} ${REP_PUBLIC_HEADERS})
add_library(rep::rep ALIAS rep)
set_target_properties(rep PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
target_include_directories(rep PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(rep PRIVATE
  REP_BUILD_PROJECT_VERSION="${PROJECT_VERSION}")
rep_apply_strict_warnings(rep)
rep_apply_sanitizers(rep)

if(WIN32)
  target_compile_definitions(rep PRIVATE UNICODE _UNICODE)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS rep
  EXPORT RackEvacuationPlannerTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(REP_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/RackEvacuationPlanner)
install(EXPORT RackEvacuationPlannerTargets
  FILE RackEvacuationPlannerTargets.cmake
  NAMESPACE rep::
  DESTINATION ${REP_CMAKE_INSTALL_DIR})

configure_package_config_file(
  cmake/RackEvacuationPlannerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/RackEvacuationPlannerConfig.cmake
  INSTALL_DESTINATION ${REP_CMAKE_INSTALL_DIR})
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/RackEvacuationPlannerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/RackEvacuationPlannerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/RackEvacuationPlannerConfigVersion.cmake
  DESTINATION ${REP_CMAKE_INSTALL_DIR})

if(REP_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(REP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(REP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
