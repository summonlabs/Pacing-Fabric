# Pacing Fabric command line tools.
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

function(pf_tool_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc /WX)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

add_executable(pacingd pacingd.cpp)
target_link_libraries(pacingd PRIVATE PacingFabric::pacing_fabric)
pf_tool_warnings(pacingd)

add_executable(pacing-agent pacing-agent.cpp)
target_link_libraries(pacing-agent PRIVATE PacingFabric::pacing_fabric)
pf_tool_warnings(pacing-agent)

add_executable(pacingctl pacingctl.cpp)
target_link_libraries(pacingctl PRIVATE PacingFabric::pacing_fabric)
pf_tool_warnings(pacingctl)

include(GNUInstallDirs)
install(TARGETS pacingd pacing-agent pacingctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
