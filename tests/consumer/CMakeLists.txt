# Rack Evacuation Planner - downstream consumer of the installed package.
#
# This is a standalone project, configured against an install prefix and never
# against the library's build tree:
#
#   cmake -S tests/consumer -B <build-dir> -DCMAKE_PREFIX_PATH=<install-prefix>
#
# That is what makes it a proof of the package: it sees exactly what an
# unrelated consumer sees, and nothing else.

cmake_minimum_required(VERSION 3.20)

project(RackEvacuationPlannerConsumer
  VERSION 1.0.0
  DESCRIPTION "Independent consumer of the Rack Evacuation Planner package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(RackEvacuationPlanner 1.0 REQUIRED)

add_executable(rep_consumer consumer.cpp)
target_link_libraries(rep_consumer PRIVATE rep::rep)

if(MSVC)
  target_compile_options(rep_consumer PRIVATE /W4 /WX /permissive-)
else()
  target_compile_options(rep_consumer PRIVATE -Wall -Wextra -Werror)
endif()

message(STATUS "rep_consumer: RackEvacuationPlanner ${RackEvacuationPlanner_VERSION} found")
