// Pacing Fabric 1.0.0 - version identity and build provenance surface.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_VERSION_HPP
#define PACING_FABRIC_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace pacing {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";

// Durable-state format version. Bumping this invalidates older snapshots.
inline constexpr std::uint32_t kStateFormatVersion = 1;

// Wire protocol version for the framed control transport.
inline constexpr std::uint16_t kWireProtocolVersion = 1;

}  // namespace pacing

#endif  // PACING_FABRIC_VERSION_HPP
