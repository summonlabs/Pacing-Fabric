// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/time.hpp"

#include <chrono>

namespace pacing {

Instant SteadyClock::now() const noexcept {
  const auto tp = std::chrono::steady_clock::now().time_since_epoch();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count();
  if (ns <= 0) return Instant::from_ns(1);
  return Instant::from_ns(static_cast<u64>(ns));
}

bool tick_for(Instant instant, u64 tick_period_ns, Tick& out) noexcept {
  if (tick_period_ns == 0) return false;
  out = Tick::from(instant.ns() / tick_period_ns);
  return true;
}

}  // namespace pacing
