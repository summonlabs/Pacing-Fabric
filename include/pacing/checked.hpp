// Pacing Fabric - exact, checked integer arithmetic for externally influenced
// sizes, capacities, rates, counters and time units.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_CHECKED_HPP
#define PACING_FABRIC_CHECKED_HPP

#include <cstdint>
#include <limits>

namespace pacing {

// Fixed-width aliases shared by every header in the library. Declared once
// here so that low-level headers can rely on them without pulling in identity
// or status machinery.
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

namespace checked {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr u64 kU64Max = std::numeric_limits<u64>::max();

constexpr bool add(u64 a, u64 b, u64& out) noexcept {
  if (a > kU64Max - b) return false;
  out = a + b;
  return true;
}

constexpr bool sub(u64 a, u64 b, u64& out) noexcept {
  if (b > a) return false;
  out = a - b;
  return true;
}

constexpr bool mul(u64 a, u64 b, u64& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  if (a > kU64Max / b) return false;
  out = a * b;
  return true;
}

constexpr bool div(u64 a, u64 b, u64& out) noexcept {
  if (b == 0) return false;
  out = a / b;
  return true;
}

// Exact 128-bit product of two 64-bit values split into high/low halves.
// Portable limb decomposition: no compiler intrinsics, no UB.
constexpr void umul128(u64 a, u64 b, u64& hi, u64& lo) noexcept {
  const u64 a_lo = a & 0xFFFFFFFFull;
  const u64 a_hi = a >> 32;
  const u64 b_lo = b & 0xFFFFFFFFull;
  const u64 b_hi = b >> 32;

  u64 t = a_lo * b_lo;
  const u64 w0 = t & 0xFFFFFFFFull;
  u64 k = t >> 32;

  t = a_hi * b_lo + k;
  const u64 w1 = t & 0xFFFFFFFFull;
  const u64 w2 = t >> 32;

  t = a_lo * b_hi + w1;
  k = t >> 32;

  lo = (t << 32) + w0;
  hi = a_hi * b_hi + w2 + k;
}

// Exact (a * b) / d with remainder, computed over the full 128-bit product.
// Fails when d == 0 or the quotient does not fit in 64 bits. Never wraps and
// never rounds: rounding policy belongs to the caller.
constexpr bool mul_div_mod(u64 a, u64 b, u64 d, u64& quotient, u64& remainder) noexcept {
  quotient = 0;
  remainder = 0;
  if (d == 0) return false;

  u64 hi = 0;
  u64 lo = 0;
  umul128(a, b, hi, lo);

  // The 128-bit dividend needs more than 64 quotient bits iff hi >= d.
  if (hi >= d) return false;

  if (hi == 0) {
    quotient = lo / d;
    remainder = lo % d;
    return true;
  }

  // Restoring shift-subtract division over the 128-bit dividend. The running
  // remainder is always strictly below d, so the only overflow risk is the
  // left shift, handled by the carry test and modular subtraction below.
  u64 q = 0;
  u64 r = 0;
  for (int i = 127; i >= 0; --i) {
    const u64 bit =
        (i >= 64) ? ((hi >> (i - 64)) & 1ull) : ((lo >> static_cast<unsigned>(i)) & 1ull);
    u64 r2 = (r << 1) | bit;
    const bool carry = (r >> 63) != 0;
    const bool ge = carry || (r2 >= d);
    if (ge) r2 -= d;  // modular subtraction stays exact even when carry was set
    r = r2;
    if (ge && i < 64) q |= (1ull << static_cast<unsigned>(i));
  }
  quotient = q;
  remainder = r;
  return true;
}

constexpr bool mul_div_floor(u64 a, u64 b, u64 d, u64& out) noexcept {
  u64 r = 0;
  return mul_div_mod(a, b, d, out, r);
}

// Ceiling division: rounds any non-zero remainder up. Used wherever a rate
// must be expressed as a whole number of quanta per tick without silently
// under-delivering below the authorized floor.
constexpr bool mul_div_ceil(u64 a, u64 b, u64 d, u64& out) noexcept {
  u64 q = 0;
  u64 r = 0;
  if (!mul_div_mod(a, b, d, q, r)) return false;
  if (r != 0) {
    if (q == kU64Max) return false;
    ++q;
  }
  out = q;
  return true;
}

// Saturating multiply used only on paths where an upper bound - not an exact
// value - is the contract. Never used for authority-bearing quantities.
constexpr u64 sat_mul(u64 a, u64 b) noexcept {
  u64 out = 0;
  if (!mul(a, b, out)) return kU64Max;
  return out;
}

constexpr u64 sat_add(u64 a, u64 b) noexcept {
  u64 out = 0;
  if (!add(a, b, out)) return kU64Max;
  return out;
}

// Narrowing helpers that refuse lossy conversions instead of truncating.
constexpr bool narrow_u32(u64 v, u32& out) noexcept {
  if (v > 0xFFFFFFFFull) return false;
  out = static_cast<u32>(v);
  return true;
}

constexpr bool narrow_u16(u64 v, std::uint16_t& out) noexcept {
  if (v > 0xFFFFull) return false;
  out = static_cast<std::uint16_t>(v);
  return true;
}

// A fraction of a value expressed in parts-per-million, floored.
constexpr bool ppm_of(u64 value, u64 ppm, u64& out) noexcept {
  return mul_div_floor(value, ppm, 1'000'000ull, out);
}

}  // namespace checked

// FNV-1a 64-bit digest. Used for canonical identity digests over authority
// vectors and evidence records - never as a cryptographic guarantee.
class Digest64 {
 public:
  constexpr Digest64() noexcept = default;

  constexpr void mix_byte(std::uint8_t b) noexcept {
    state_ ^= static_cast<std::uint64_t>(b);
    state_ *= 0x100000001B3ull;
  }

  constexpr void mix_u64(std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
      mix_byte(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFull));
    }
  }

  constexpr void mix_u32(std::uint32_t v) noexcept { mix_u64(static_cast<std::uint64_t>(v)); }

  constexpr void mix_bool(bool v) noexcept { mix_byte(v ? 1u : 0u); }

  constexpr std::uint64_t value() const noexcept { return state_; }

 private:
  std::uint64_t state_{0xCBF29CE484222325ull};
};

}  // namespace pacing

#endif  // PACING_FABRIC_CHECKED_HPP
