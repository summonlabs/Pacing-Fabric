// Pacing Fabric - CRC-32C (Castagnoli) integrity check.
//
// Used for durable record framing and control-plane frame framing. This is a
// corruption/torn-write detector, not a cryptographic authenticator; that
// distinction is documented wherever it is relied upon.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_CRC32C_HPP
#define PACING_FABRIC_CRC32C_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace pacing {

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept;
std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept;

}  // namespace pacing

#endif  // PACING_FABRIC_CRC32C_HPP
