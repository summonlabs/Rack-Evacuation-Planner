# Rack Evacuation Planner - test suite.
#
# One executable per test file: a fault in one test cannot take another test's
# process down with it, and the suite carries no timeouts anywhere, so a test
# is always allowed to run to natural completion.

add_library(rep_tests STATIC testkit.cpp)
target_include_directories(rep_tests PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
rep_apply_strict_warnings(rep_tests)
rep_apply_sanitizers(rep_tests)

# Creates the executable "test_<name>" from "test_<name>.cpp" and registers it
# with CTest under the short name <name>.  No TIMEOUT property is set here or
# anywhere else in this project.
function(rep_register_test name)
  set(target "test_${name}")
  add_executable(${target} "test_${name}.cpp")
  target_link_libraries(${target} PRIVATE rep::rep rep_tests)
  rep_apply_strict_warnings(${target})
  rep_apply_sanitizers(${target})
  add_test(NAME ${name} COMMAND ${target})
endfunction()

set(REP_TEST_NAMES
  digest
  canonical
  types
  domain
  evidence
  scenario
  planner
  plan
  store
  engine
  property
  concurrency
  multiprocess
  hardening)

foreach(rep_test_name IN LISTS REP_TEST_NAMES)
  rep_register_test(${rep_test_name})
endforeach()

# The multiprocess test starts real independent processes to prove cross-process
# fencing.  The helper is built but never registered as a test of its own.
add_executable(rep_fault_child fault_child.cpp)
target_link_libraries(rep_fault_child PRIVATE rep::rep)
rep_apply_strict_warnings(rep_fault_child)
rep_apply_sanitizers(rep_fault_child)

# Tests created with add_test(NAME ... COMMAND ...) have their properties
# evaluated as generator expressions, so the helper paths below are the real
# build paths for the configuration under test.
set(rep_fault_child_environment
  "REP_FAULT_CHILD=$<TARGET_FILE:rep_fault_child>"
  "REP_TEST_SEED_FILE=$<TARGET_FILE_DIR:rep_fault_child>")
set_tests_properties(multiprocess PROPERTIES ENVIRONMENT "${rep_fault_child_environment}")
