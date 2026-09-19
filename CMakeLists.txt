cmake_minimum_required(VERSION 3.25)

# Pacing Fabric 1.0.0 - open-source, vendor-neutral C++20 runtime for
# generation-bound packet and flow pacing policy, pacing envelopes, burst
# smoothing, lifecycle, authority and verified application state.
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

project(PacingFabric
  VERSION 1.0.0
  DESCRIPTION "Generation-bound packet and flow pacing policy runtime with verified application state"
  HOMEPAGE_URL "https://github.com/summonlabs/Pacing-Fabric"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PACING_FABRIC_BUILD_TESTS "Build the Pacing Fabric test suite" ON)
option(PACING_FABRIC_BUILD_TOOLS "Build the pacingctl command line tool" ON)
option(PACING_FABRIC_BUILD_BENCH "Build the synthetic control-plane benchmark" ON)
option(PACING_FABRIC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(PACING_FABRIC_ENABLE_ASAN "Enable AddressSanitizer on supported toolchains" OFF)

set(PACING_FABRIC_SOURCES
  src/application.cpp
  src/backend.cpp
  src/backends.cpp
  src/binding.cpp
  src/control.cpp
  src/crc32c.cpp
  src/durable.cpp
  src/envelope.cpp
  src/events.cpp
  src/explain.cpp
  src/fabric.cpp
  src/id.cpp
  src/limits.cpp
  src/policy.cpp
  src/rate.cpp
  src/serialize.cpp
  src/status.cpp
  src/time.cpp
  src/transport.cpp
  src/upstream.cpp
)

add_library(pacing_fabric STATIC ${PACING_FABRIC_SOURCES})
add_library(PacingFabric::pacing_fabric ALIAS pacing_fabric)

target_include_directories(pacing_fabric
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

target_compile_features(pacing_fabric PUBLIC cxx_std_20)

if(MSVC)
  # The portable stdio/CRT surface (fopen, _commit) is used deliberately so the
  # durability layer is identical on Windows and POSIX.
  target_compile_definitions(pacing_fabric PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(pacing_fabric PRIVATE /W4 /permissive- /utf-8 /EHsc)
  if(PACING_FABRIC_WARNINGS_AS_ERRORS)
    target_compile_options(pacing_fabric PRIVATE /WX)
  endif()
  if(PACING_FABRIC_ENABLE_ASAN)
    target_compile_options(pacing_fabric PUBLIC /fsanitize=address /Zi)
    target_link_options(pacing_fabric PUBLIC /INCREMENTAL:NO /DEBUG)
  endif()
else()
  target_compile_options(pacing_fabric PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  if(PACING_FABRIC_WARNINGS_AS_ERRORS)
    target_compile_options(pacing_fabric PRIVATE -Werror)
  endif()
  if(PACING_FABRIC_ENABLE_ASAN)
    target_compile_options(pacing_fabric PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(pacing_fabric PUBLIC -fsanitize=address)
  endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(pacing_fabric PUBLIC Threads::Threads)

# The framed control transport uses the platform stream socket API directly.
if(WIN32)
  target_link_libraries(pacing_fabric PUBLIC ws2_32)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS pacing_fabric
  EXPORT PacingFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/pacing DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT PacingFabricTargets
  FILE PacingFabricTargets.cmake
  NAMESPACE PacingFabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PacingFabric
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PacingFabricConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/PacingFabricConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PacingFabric
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/PacingFabricConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/PacingFabricConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/PacingFabricConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PacingFabric
)

if(PACING_FABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(PACING_FABRIC_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(PACING_FABRIC_BUILD_BENCH)
  add_subdirectory(bench)
endif()
