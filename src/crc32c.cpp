// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/crc32c.hpp"

#include <array>

namespace pacing {
namespace {

// Reflected CRC-32C (Castagnoli) polynomial.
constexpr std::uint32_t kPoly = 0x82F63B78u;

constexpr std::array<std::uint32_t, 256> make_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  // A range-based fill keeps every store provably inside the array bounds,
  // which a subscripted loop counter does not guarantee to a static analyser.
  std::uint32_t index = 0;
  for (auto& slot : table) {
    std::uint32_t c = index;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1u) != 0u ? (kPoly ^ (c >> 1)) : (c >> 1);
    }
    slot = c;
    ++index;
  }
  return table;
}

constexpr auto kTable = make_table();

}  // namespace

std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept {
  std::uint32_t c = ~seed;
  for (std::uint8_t b : data) {
    // The index type is std::uint8_t, so the table lookup is provably in range.
    // Masking a wider expression would be equivalent but harder for a static
    // analyser to bound.
    const std::uint8_t index = static_cast<std::uint8_t>(c ^ b);
    c = kTable[index] ^ (c >> 8);
  }
  return ~c;
}

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept {
  return crc32c_extend(0u, data);
}

}  // namespace pacing
