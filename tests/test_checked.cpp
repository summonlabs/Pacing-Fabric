// Checked-arithmetic verification, including the 128-bit product/divide identity.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <limits>

#include "framework.hpp"
#include "pacing/checked.hpp"

using namespace pacing;

namespace {
constexpr u64 kMax = std::numeric_limits<u64>::max();
}  // namespace

PF_TEST(checked, add_sub_mul_div_overflow) {
  u64 out = 0;
  PF_CHECK(checked::add(1, 2, out));
  PF_CHECK_EQ(out, 3ull);
  PF_CHECK(checked::add(kMax, 0, out));
  PF_CHECK_EQ(out, kMax);
  PF_CHECK(!checked::add(kMax, 1, out));
  PF_CHECK(!checked::add(kMax, kMax, out));

  PF_CHECK(checked::sub(5, 3, out));
  PF_CHECK_EQ(out, 2ull);
  PF_CHECK(checked::sub(3, 3, out));
  PF_CHECK_EQ(out, 0ull);
  PF_CHECK(!checked::sub(3, 4, out));

  PF_CHECK(checked::mul(0, kMax, out));
  PF_CHECK_EQ(out, 0ull);
  PF_CHECK(checked::mul(kMax, 1, out));
  PF_CHECK_EQ(out, kMax);
  PF_CHECK(checked::mul(1ull << 31, 2, out));
  PF_CHECK_EQ(out, 1ull << 32);
  PF_CHECK(!checked::mul(1ull << 32, 1ull << 32, out));
  PF_CHECK(!checked::mul(kMax, 2, out));

  PF_CHECK(!checked::div(1, 0, out));
  PF_CHECK(checked::div(kMax, 1, out));
  PF_CHECK_EQ(out, kMax);
}

PF_TEST(checked, umul128_matches_known_products) {
  u64 hi = 0;
  u64 lo = 0;
  checked::umul128(0, kMax, hi, lo);
  PF_CHECK_EQ(hi, 0ull);
  PF_CHECK_EQ(lo, 0ull);

  checked::umul128(kMax, kMax, hi, lo);
  PF_CHECK_EQ(hi, kMax - 1);
  PF_CHECK_EQ(lo, 1ull);

  checked::umul128(1ull << 63, 2, hi, lo);
  PF_CHECK_EQ(hi, 1ull);
  PF_CHECK_EQ(lo, 0ull);

  checked::umul128(0x1'0000'0000ull, 0x1'0000'0000ull, hi, lo);
  PF_CHECK_EQ(hi, 1ull);
  PF_CHECK_EQ(lo, 0ull);

  // (2^32 - 1)^2 = 2^64 - 2^33 + 1
  checked::umul128(0xFFFFFFFFull, 0xFFFFFFFFull, hi, lo);
  PF_CHECK_EQ(hi, 0ull);
  PF_CHECK_EQ(lo, 0xFFFFFFFE00000001ull);

  checked::umul128(0x1'0000'0000ull, 0xFFFFFFFFull, hi, lo);
  PF_CHECK_EQ(hi, 0ull);
  PF_CHECK_EQ(lo, 0xFFFFFFFF00000000ull);
}

PF_TEST(checked, umul128_is_commutative_and_consistent_with_mul) {
  tf::Rng rng(0x5EEDull);
  for (int i = 0; i < 5000; ++i) {
    const u64 a = rng.next();
    const u64 b = rng.next();
    u64 hi1 = 0;
    u64 lo1 = 0;
    u64 hi2 = 0;
    u64 lo2 = 0;
    checked::umul128(a, b, hi1, lo1);
    checked::umul128(b, a, hi2, lo2);
    PF_CHECK_EQ(hi1, hi2);
    PF_CHECK_EQ(lo1, lo2);
    u64 product = 0;
    if (checked::mul(a, b, product)) {
      PF_CHECK_EQ(hi1, 0ull);
      PF_CHECK_EQ(lo1, product);
    } else {
      PF_CHECK(hi1 != 0);
    }
  }
}

PF_TEST(checked, mul_div_identity_holds) {
  tf::Rng rng(0xC0FFEEull);
  for (int i = 0; i < 20000; ++i) {
    const u64 a = rng.next() >> rng.below(64);
    const u64 b = rng.next() >> rng.below(64);
    u64 d = rng.next() >> rng.below(63);
    if (d == 0) d = 1;
    u64 hi = 0;
    u64 lo = 0;
    checked::umul128(a, b, hi, lo);
    u64 q = 0;
    u64 r = 0;
    const bool ok = checked::mul_div_mod(a, b, d, q, r);
    if (hi >= d) {
      PF_CHECK(!ok);
      continue;
    }
    PF_CHECK(ok);
    PF_CHECK(r < d);
    u64 phi = 0;
    u64 plo = 0;
    checked::umul128(q, d, phi, plo);
    const u64 carry = (plo > kMax - r) ? 1ull : 0ull;
    PF_CHECK_EQ(plo + r, lo);
    PF_CHECK_EQ(phi + carry, hi);
  }
}

PF_TEST(checked, mul_div_zero_divisor_and_exact_values) {
  u64 q = 0;
  u64 r = 0;
  PF_CHECK(!checked::mul_div_mod(1, 1, 0, q, r));
  PF_CHECK(!checked::mul_div_mod(kMax, kMax, 1, q, r));
  PF_CHECK(checked::mul_div_mod(kMax, kMax, kMax, q, r));
  PF_CHECK_EQ(q, kMax);
  PF_CHECK_EQ(r, 0ull);
  PF_CHECK(checked::mul_div_mod(1ull << 63, 2, 3, q, r));
  PF_CHECK_EQ(q, 6148914691236517205ull);
  PF_CHECK_EQ(r, 1ull);
}

PF_TEST(checked, mul_div_ceil_rounds_up) {
  u64 out = 0;
  PF_CHECK(checked::mul_div_ceil(10, 1, 3, out));
  PF_CHECK_EQ(out, 4ull);
  PF_CHECK(checked::mul_div_ceil(9, 1, 3, out));
  PF_CHECK_EQ(out, 3ull);
  PF_CHECK(checked::mul_div_ceil(0, 1, 3, out));
  PF_CHECK_EQ(out, 0ull);
  PF_CHECK(!checked::mul_div_ceil(5, 1, 0, out));
}

PF_TEST(checked, ppm_of_floors) {
  u64 out = 0;
  PF_CHECK(checked::ppm_of(1'000'000ull, 1'000'000ull, out));
  PF_CHECK_EQ(out, 1'000'000ull);
  PF_CHECK(checked::ppm_of(1'000'000'001ull, 1'000'000ull, out));
  PF_CHECK_EQ(out, 1'000'000'001ull);
  PF_CHECK(checked::ppm_of(3, 500'000ull, out));
  PF_CHECK_EQ(out, 1ull);
  // 1'999'999 * 0.5 == 999'999.5, floored to 999'999.
  PF_CHECK(checked::ppm_of(1'999'999ull, 500'000ull, out));
  PF_CHECK_EQ(out, 999'999ull);
}

PF_TEST(checked, narrowing_refuses_loss) {
  u32 small = 0;
  PF_CHECK(checked::narrow_u32(0xFFFFFFFFull, small));
  PF_CHECK_EQ(small, 0xFFFFFFFFu);
  PF_CHECK(!checked::narrow_u32(0x1'0000'0000ull, small));
  std::uint16_t tiny = 0;
  PF_CHECK(checked::narrow_u16(0xFFFFull, tiny));
  PF_CHECK(!checked::narrow_u16(0x10000ull, tiny));
}

PF_TEST(checked, digest_is_deterministic_and_order_sensitive) {
  Digest64 a;
  a.mix_u64(1);
  a.mix_u64(2);
  Digest64 b;
  b.mix_u64(1);
  b.mix_u64(2);
  PF_CHECK_EQ(a.value(), b.value());
  Digest64 c;
  c.mix_u64(2);
  c.mix_u64(1);
  PF_CHECK_NE(a.value(), c.value());
  Digest64 d;
  d.mix_bool(true);
  Digest64 e;
  e.mix_bool(false);
  PF_CHECK_NE(d.value(), e.value());
}
