# Independent downstream consumer of the installed Pacing Fabric package.
#
# This project is deliberately separate from the Pacing Fabric build tree: it
# resolves the package with find_package(PacingFabric CONFIG REQUIRED) from an
# install prefix and links the exported target only.
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.25)
project(PacingFabricDownstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PacingFabric 1.0 CONFIG REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE PacingFabric::pacing_fabric)

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /permissive- /utf-8 /WX)
  target_compile_definitions(downstream_consumer PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
add_test(NAME downstream_consumer COMMAND downstream_consumer)
