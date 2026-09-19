# Pacing Fabric test suite.
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

add_library(pf_test_support STATIC framework.cpp)
target_include_directories(pf_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pf_test_support PUBLIC PacingFabric::pacing_fabric)

function(pf_test_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc /WX)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

pf_test_warnings(pf_test_support)

function(pf_add_unit_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE pf_test_support)
  pf_test_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

pf_add_unit_test(test_checked test_checked.cpp)
pf_add_unit_test(test_model test_model.cpp)
pf_add_unit_test(test_hardening test_hardening.cpp)

foreach(extra_test test_lifecycle test_governance test_persistence test_concurrency
                   test_adversarial test_randomized)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${extra_test}.cpp")
    pf_add_unit_test(${extra_test} ${extra_test}.cpp)
  endif()
endforeach()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/multiproc/test_multiproc.cpp")
  add_subdirectory(multiproc)
endif()
