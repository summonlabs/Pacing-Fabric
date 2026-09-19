// Pacing Fabric - rate, quantum, cadence shape and burst allowance.
//
// Separation of concerns enforced here: a Rate is an *authorized ceiling*
// supplied by upstream rate authority, a Cadence is *pacing intent* derived by
// this fabric, and neither can conjure the other into existence.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_RATE_HPP
#define PACING_FABRIC_RATE_HPP

#include <cstdint>
#include <limits>
#include <string>

#include "pacing/checked.hpp"
#include "pacing/id.hpp"
#include "pacing/status.hpp"
#include "pacing/time.hpp"

namespace pacing {

// Bits per second. Zero is a valid *value* but never a positive authorization:
// callers must distinguish "0 bps authorized" from "UNKNOWN".
class RateBps {
 public:
  constexpr RateBps() noexcept = default;
  static constexpr RateBps from_bps(u64 bps) noexcept {
    RateBps r;
    r.bps_ = bps;
    return r;
  }

  [[nodiscard]] constexpr u64 bps() const noexcept { return bps_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return bps_ == 0; }

  friend constexpr bool operator==(RateBps, RateBps) noexcept = default;
  friend constexpr auto operator<=>(RateBps, RateBps) noexcept = default;

 private:
  u64 bps_{0};
};

// Byte counts used for quanta and bursts. Kept distinct from RateBps so that
// a bits/bytes unit error cannot silently compile.
class Bytes {
 public:
  constexpr Bytes() noexcept = default;
  static constexpr Bytes from(u64 v) noexcept {
    Bytes b;
    b.v_ = v;
    return b;
  }

  [[nodiscard]] constexpr u64 value() const noexcept { return v_; }

  friend constexpr bool operator==(Bytes, Bytes) noexcept = default;
  friend constexpr auto operator<=>(Bytes, Bytes) noexcept = default;

 private:
  u64 v_{0};
};

// How pacing grants are distributed inside a window. This is pacing *intent*:
// it says nothing about queue scheduling or admission, which live elsewhere.
enum class CadenceShape : u8 {
  Uniform = 0,      // evenly spaced grants at the derived interval
  Windowed = 1,     // fixed grant count per window, evenly spaced within it
  TokenBucket = 2,  // uniform refill with an explicit burst depth
};

std::string_view to_string(CadenceShape shape) noexcept;
bool parse_cadence_shape(std::string_view text, CadenceShape& out) noexcept;

// Requested burst allowance. Both dimensions are bounded by policy limits;
// an allowance above the bound is a rejection, never a silent clamp, because a
// clamp would under-state a real burst requirement.
struct BurstAllowance {
  u64 bytes{0};
  u64 packets{0};

  friend constexpr bool operator==(const BurstAllowance&, const BurstAllowance&) noexcept = default;
};

// Derived, fully specified cadence for one envelope. Every field is a positive
// integer; no floating point participates anywhere in derivation.
struct Cadence {
  u64 rate_bps{0};
  u64 quantum_bytes{0};
  u64 interval_ns{0};
  u64 window_ns{0};
  u64 grants_per_window{0};
  u64 bytes_per_window{0};
  u64 burst_bytes{0};
  u64 burst_packets{0};
  CadenceShape shape{CadenceShape::Uniform};

  friend constexpr bool operator==(const Cadence&, const Cadence&) noexcept = default;

  // Effective steady-state rate implied by the derived cadence. Because the
  // interval is computed with a ceiling division this is always <= rate_bps,
  // which is exactly the property the ceiling invariant relies on.
  [[nodiscard]] bool effective_rate_bps(u64& out) const noexcept;

  [[nodiscard]] std::string to_string() const;
};

// Verifies the structural well-formedness and bound compliance of a derived
// cadence. Returns InvalidArgument/OutOfRange/BurstExceedsBound on defect.
[[nodiscard]] Status validate_cadence(const Cadence& cadence, u64 max_rate_bps, u64 max_burst_bytes,
                                      u64 max_burst_packets) noexcept;

// Derives the interval in nanoseconds for a quantum at a rate, rounding the
// interval *up* so the resulting cadence can never exceed the input rate.
[[nodiscard]] bool derive_interval_ns(u64 quantum_bytes, u64 rate_bps, u64& interval_ns) noexcept;

// Largest burst (in bytes) that a rate can accumulate over a bounded horizon,
// rounded down. Used to keep token-bucket depth proportional to the authorized
// rate instead of accepting an arbitrary depth.
[[nodiscard]] bool burst_capacity_bytes(u64 rate_bps, u64 horizon_ns, u64& out) noexcept;

}  // namespace pacing

#endif  // PACING_FABRIC_RATE_HPP
