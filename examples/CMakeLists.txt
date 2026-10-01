# Rack Evacuation Planner - examples.
#
# Examples are built from this tree only and are never installed: the walkthrough
# exists to be read and run, not to ship.

add_executable(rep_plan_walkthrough plan_walkthrough.cpp)
target_link_libraries(rep_plan_walkthrough PRIVATE rep::rep)
rep_apply_strict_warnings(rep_plan_walkthrough)
# Every translation unit that links the library must be built with the same
# instrumentation, or MSVC's container annotations disagree at link time.
rep_apply_sanitizers(rep_plan_walkthrough)
