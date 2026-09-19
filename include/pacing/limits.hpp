// Pacing Fabric - bounded resource limits. Every externally influenced size,
// count or capacity is validated against one of these before use.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_LIMITS_HPP
#define PACING_FABRIC_LIMITS_HPP

#include <cstdint>

#include "pacing/time.hpp"

namespace pacing {

// Hard upper bounds applied to every externally supplied or derived quantity.
// Defaults target control-plane scale (millions of flows, thousands of
// policies) while keeping per-record allocations small and predictable.
struct Limits {
  u64 max_rate_bps{400'000'000'000ull};    // 400 Gbps
  u64 max_quantum_bytes{1ull << 24};       // 16 MiB
  u64 max_burst_bytes{1ull << 30};         // 1 GiB
  u64 max_burst_packets{1ull << 24};       // ~16.7 M packets
  u64 min_window_ns{1'000ull};             // 1 us
  u64 max_window_ns{10ull * kNanosPerSecond};
  u64 max_interval_ns{10ull * kNanosPerSecond};
  u64 max_burst_horizon_ns{100ull * 1'000'000ull};  // 100 ms of rate-derived depth
  u64 max_envelope_lifetime_ns{5ull * kNanosPerSecond};

  u32 max_service_class_exceptions{64};
  u32 max_policies{4096};
  u32 max_flows{1u << 20};
  u32 max_envelopes{1u << 20};
  u32 max_attempts{1u << 21};
  u32 max_backends{64};
  u32 max_resources{1u << 16};

  // Durable-state bounds.
  u32 max_journal_records{1u << 20};
  u64 max_journal_bytes{256ull << 20};  // 256 MiB before mandatory compaction
  u64 max_durable_record_bytes{1ull << 20};
  u32 max_compaction_batch{1u << 16};

  // Transport bounds. A frame larger than this is rejected before allocation.
  u32 max_frame_bytes{1u << 20};
  u32 max_frame_payload_bytes{(1u << 20) - 64};
  u32 max_connections{256};
  u32 max_explanation_bytes{8192};
  u32 max_name_bytes{128};
  u32 max_retries{4};

  // Concurrency bounds.
  u32 max_workers{256};
};

// Validates a Limits set for internal consistency. A misconfigured bound must
// fail loudly at construction rather than produce surprising behaviour later.
[[nodiscard]] bool limits_are_coherent(const Limits& limits) noexcept;

}  // namespace pacing

#endif  // PACING_FABRIC_LIMITS_HPP
