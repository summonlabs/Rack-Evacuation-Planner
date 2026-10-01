# Rack Evacuation Planner - command line tools.

add_executable(rep_cli rep_cli.cpp)
add_executable(rep::cli ALIAS rep_cli)
target_link_libraries(rep_cli PRIVATE rep::rep)
rep_apply_strict_warnings(rep_cli)
rep_apply_sanitizers(rep_cli)

install(TARGETS rep_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
