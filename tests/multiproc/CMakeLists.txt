# Multiprocess coordinator tests (real OS processes, real framed transport).
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

add_executable(test_multiproc test_multiproc.cpp process.cpp)
target_link_libraries(test_multiproc PRIVATE pf_test_support)
pf_test_warnings(test_multiproc)
add_test(NAME test_multiproc
  COMMAND test_multiproc --daemon $<TARGET_FILE:pacingd> --agent $<TARGET_FILE:pacing-agent>)
