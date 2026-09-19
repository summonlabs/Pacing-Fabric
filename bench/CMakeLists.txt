# Synthetic control-plane benchmark.
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

add_executable(bench_pacing bench_pacing.cpp)
target_link_libraries(bench_pacing PRIVATE PacingFabric::pacing_fabric)
if(MSVC)
  target_compile_options(bench_pacing PRIVATE /W4 /permissive- /utf-8 /EHsc /WX)
  target_compile_definitions(bench_pacing PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(bench_pacing PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
