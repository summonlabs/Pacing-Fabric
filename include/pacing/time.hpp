// Pacing Fabric - tick/time abstraction. Pacing arithmetic never reads the
// wall clock directly: every decision is derivable from an injected clock so
// that tick rollover and monotonicity are testable and deterministic.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_TIME_HPP
#define PACING_FABRIC_TIME_HPP

#include <atomic>
#include <compare>
#include <cstdint>

#include "pacing/checked.hpp"

namespace pacing {

using u64 = std::uint64_t;
using i64 = std::int64_t;

inline constexpr u64 kNanosPerSecond = 1'000'000'000ull;

// Monotonic, non-decreasing nanosecond instant. Absolute origin is
// unspecified; only differences are meaningful.
class Instant {
 public:
  constexpr Instant() noexcept = default;
  static constexpr Instant from_ns(u64 ns) noexcept {
    Instant i;
    i.ns_ = ns;
    return i;
  }

  [[nodiscard]] constexpr u64 ns() const noexcept { return ns_; }

  friend constexpr bool operator==(Instant, Instant) noexcept = default;
  friend constexpr auto operator<=>(Instant, Instant) noexcept = default;

 private:
  u64 ns_{0};
};

// Clock source. Implementations must be non-decreasing; the fabric treats a
// regression as a fault rather than a retry hint.
class IClock {
 public:
  virtual ~IClock() = default;
  [[nodiscard]] virtual Instant now() const noexcept = 0;
  [[nodiscard]] virtual const char* name() const noexcept = 0;
};

// Steady-clock implementation used in production paths.
class SteadyClock final : public IClock {
 public:
  [[nodiscard]] Instant now() const noexcept override;
  [[nodiscard]] const char* name() const noexcept override { return "steady"; }
};

// Deterministic clock for tests and replay. Advancing is explicit.
class ManualClock final : public IClock {
 public:
  explicit ManualClock(u64 start_ns = 1) noexcept : now_ns_(start_ns) {}

  [[nodiscard]] Instant now() const noexcept override {
    return Instant::from_ns(now_ns_.load(std::memory_order_relaxed));
  }

  [[nodiscard]] const char* name() const noexcept override { return "manual"; }

  void advance(u64 delta_ns) noexcept { now_ns_.fetch_add(delta_ns, std::memory_order_relaxed); }
  void set(u64 absolute_ns) noexcept { now_ns_.store(absolute_ns, std::memory_order_relaxed); }

  // Deliberately non-monotonic: only used to prove the fabric rejects a clock
  // that moves backwards instead of quietly issuing stale envelopes.
  void force_regression(u64 absolute_ns) noexcept { now_ns_.store(absolute_ns, std::memory_order_relaxed); }

 private:
  std::atomic<u64> now_ns_;
};

// Tick index derived from an instant and a fixed tick period. Tick rollover
// (wrapping the tick counter) must not change any pacing decision, so the
// counter is a wrapping u64 and all downstream arithmetic is modular.
class Tick {
 public:
  constexpr Tick() noexcept = default;
  static constexpr Tick from(u64 index) noexcept {
    Tick t;
    t.index_ = index;
    return t;
  }

  [[nodiscard]] constexpr u64 index() const noexcept { return index_; }
  [[nodiscard]] constexpr Tick next() const noexcept { return Tick::from(index_ + 1); }
  [[nodiscard]] constexpr Tick advance(u64 delta) const noexcept { return Tick::from(index_ + delta); }
  [[nodiscard]] constexpr u64 delta_to(Tick later) const noexcept { return later.index_ - index_; }

  friend constexpr bool operator==(Tick, Tick) noexcept = default;

 private:
  u64 index_{0};
};

// Converts an instant to a tick index for a given tick period. A zero period
// is rejected rather than treated as "one tick per nanosecond".
[[nodiscard]] bool tick_for(Instant instant, u64 tick_period_ns, Tick& out) noexcept;

// Bounded deadline expressed as an instant. none() means "no deadline", which
// callers must treat as UNKNOWN rather than as "infinite authority".
class Deadline {
 public:
  constexpr Deadline() noexcept = default;
  static constexpr Deadline at(Instant instant) noexcept {
    Deadline d;
    d.instant_ = instant;
    d.set_ = true;
    return d;
  }
  static constexpr Deadline none() noexcept { return Deadline{}; }

  [[nodiscard]] constexpr bool is_set() const noexcept { return set_; }
  [[nodiscard]] constexpr Instant instant() const noexcept { return instant_; }
  [[nodiscard]] constexpr bool expired_at(Instant now) const noexcept { return set_ && now >= instant_; }

 private:
  Instant instant_{};
  bool set_{false};
};

}  // namespace pacing

#endif  // PACING_FABRIC_TIME_HPP
