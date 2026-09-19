// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/limits.hpp"

namespace pacing {

bool limits_are_coherent(const Limits& limits) noexcept {
  if (limits.max_rate_bps == 0) return false;
  if (limits.max_quantum_bytes == 0) return false;
  if (limits.max_burst_bytes == 0) return false;
  if (limits.max_burst_packets == 0) return false;
  if (limits.min_window_ns == 0) return false;
  if (limits.min_window_ns > limits.max_window_ns) return false;
  if (limits.max_interval_ns == 0) return false;
  if (limits.max_burst_horizon_ns == 0) return false;
  if (limits.max_envelope_lifetime_ns == 0) return false;
  if (limits.max_service_class_exceptions == 0) return false;
  if (limits.max_policies == 0) return false;
  if (limits.max_flows == 0) return false;
  if (limits.max_envelopes == 0) return false;
  if (limits.max_attempts == 0) return false;
  if (limits.max_backends == 0) return false;
  if (limits.max_resources == 0) return false;
  if (limits.max_journal_records == 0) return false;
  if (limits.max_journal_bytes == 0) return false;
  if (limits.max_durable_record_bytes == 0) return false;
  if (limits.max_durable_record_bytes > limits.max_journal_bytes) return false;
  if (limits.max_compaction_batch == 0) return false;
  if (limits.max_frame_bytes == 0) return false;
  if (limits.max_frame_payload_bytes == 0) return false;
  if (limits.max_frame_payload_bytes >= limits.max_frame_bytes) return false;
  if (limits.max_connections == 0) return false;
  if (limits.max_explanation_bytes == 0) return false;
  if (limits.max_name_bytes == 0) return false;
  if (limits.max_workers == 0) return false;
  return true;
}

}  // namespace pacing
