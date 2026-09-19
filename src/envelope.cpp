// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/envelope.hpp"

#include <algorithm>

namespace pacing {
namespace {

constexpr u64 kBitsPerByte = 8;

}  // namespace

u64 Provenance::digest() const noexcept {
  Digest64 d;
  d.mix_u64(id.value());
  d.mix_u64(sequence);
  d.mix_u64(fabric_instance);
  d.mix_u64(binding_digest);
  d.mix_u64(policy_digest);
  d.mix_u64(grant_digest);
  d.mix_u64(cadence_digest);
  return d.value();
}

u64 PacingEnvelope::digest() const noexcept {
  Digest64 d;
  d.mix_u64(ref.id.value());
  d.mix_u64(ref.generation.value());
  d.mix_u64(flow.value());
  d.mix_u64(resource.value());
  d.mix_u64(authority.digest());
  d.mix_u64(cadence.rate_bps);
  d.mix_u64(cadence.quantum_bytes);
  d.mix_u64(cadence.interval_ns);
  d.mix_u64(cadence.window_ns);
  d.mix_u64(cadence.grants_per_window);
  d.mix_u64(cadence.bytes_per_window);
  d.mix_u64(cadence.burst_bytes);
  d.mix_u64(cadence.burst_packets);
  d.mix_u64(static_cast<u64>(cadence.shape));
  d.mix_u64(requested_rate_bps);
  d.mix_u64(ceiling_bps);
  d.mix_u64(floor_bps);
  d.mix_bool(clamped_to_ceiling);
  d.mix_bool(clamped_to_floor);
  d.mix_u64(service_class.value());
  d.mix_u64(provenance.digest());
  d.mix_u64(derived_at.ns());
  d.mix_bool(valid_until.is_set());
  d.mix_u64(valid_until.is_set() ? valid_until.instant().ns() : 0);
  d.mix_bool(upstream_synthetic);
  d.mix_bool(policy_synthetic);
  return d.value();
}

std::string PacingEnvelope::to_string() const {
  std::string out;
  out.reserve(320);
  out += "envelope=";
  out += std::to_string(ref.id.value());
  out += "@";
  out += std::to_string(ref.generation.value());
  out += " flow=";
  out += std::to_string(flow.value());
  out += " requested=";
  out += std::to_string(requested_rate_bps);
  out += "bps ceiling=";
  out += std::to_string(ceiling_bps);
  out += "bps floor=";
  out += std::to_string(floor_bps);
  out += "bps ";
  out += cadence.to_string();
  if (clamped_to_ceiling) out += " clamped=ceiling";
  if (clamped_to_floor) out += " clamped=floor";
  if (upstream_synthetic || policy_synthetic) out += " synthetic=true";
  return out;
}

StatusOr<DerivationOutcome> derive_cadence(const DerivationInput& input, const Limits& limits) noexcept {
  // --- 1. bound and shape validation -------------------------------------
  if (input.ceiling_bps == 0) {
    return Status::of(ErrorCode::UnknownAuthority, "upstream ceiling is absent or zero");
  }
  if (input.ceiling_bps > limits.max_rate_bps) {
    return Status::of(ErrorCode::OutOfRange, "upstream ceiling above configured limit");
  }
  if (input.floor_bps > input.ceiling_bps) {
    return Status::of(ErrorCode::FloorExceedsCeiling, "upstream floor exceeds upstream ceiling");
  }
  // A policy floor above the authorized ceiling would require the fabric to
  // manufacture bandwidth. It refuses instead: pacing never mints entitlement.
  if (input.min_rate_bps > input.ceiling_bps) {
    return Status::of(ErrorCode::EntitlementMint,
                      "policy floor exceeds upstream ceiling; pacing cannot mint entitlement");
  }
  if (input.min_rate_bps > limits.max_rate_bps) {
    return Status::of(ErrorCode::OutOfRange, "policy floor above configured limit");
  }
  if (input.requested_rate_bps > limits.max_rate_bps) {
    return Status::of(ErrorCode::OutOfRange, "requested rate above configured limit");
  }
  if (input.rate_share_ppm > 1'000'000ull) {
    return Status::of(ErrorCode::OutOfRange, "rate share exceeds 1000000 ppm");
  }
  if (input.quantum_bytes == 0 || input.quantum_bytes > limits.max_quantum_bytes) {
    return Status::of(ErrorCode::OutOfRange, "quantum out of range");
  }
  if (input.window_ns < limits.min_window_ns || input.window_ns > limits.max_window_ns) {
    return Status::of(ErrorCode::OutOfRange, "window out of range");
  }
  if (input.burst_bytes > limits.max_burst_bytes) {
    return Status::of(ErrorCode::BurstExceedsBound, "requested burst bytes above bound");
  }
  if (input.burst_packets > limits.max_burst_packets) {
    return Status::of(ErrorCode::BurstExceedsBound, "requested burst packets above bound");
  }
  if (input.flow_max_burst_bytes > limits.max_burst_bytes) {
    return Status::of(ErrorCode::BurstExceedsBound, "flow burst byte bound above limit");
  }
  if (input.flow_max_burst_packets > limits.max_burst_packets) {
    return Status::of(ErrorCode::BurstExceedsBound, "flow burst packet bound above limit");
  }
  if (input.grants_per_window_hint > kNanosPerSecond) {
    return Status::of(ErrorCode::OutOfRange, "grants-per-window hint above bound");
  }

  // --- 2. target rate -----------------------------------------------------
  u64 target = 0;
  if (input.requested_rate_bps != 0) {
    target = input.requested_rate_bps;
  } else if (!checked::ppm_of(input.ceiling_bps, input.rate_share_ppm, target)) {
    return Status::of(ErrorCode::ArithmeticOverflow, "rate share computation overflowed");
  }

  bool clamped_to_ceiling = false;
  bool clamped_to_floor = false;
  if (target > input.ceiling_bps) {
    target = input.ceiling_bps;
    clamped_to_ceiling = true;
  }
  const u64 lower = std::max(input.floor_bps, input.min_rate_bps);
  if (target < lower) {
    target = lower;
    clamped_to_floor = true;
  }
  if (target == 0) {
    return Status::of(ErrorCode::OutOfRange, "derived pacing rate is zero; nothing is authorized");
  }
  if (target > limits.max_rate_bps) {
    return Status::of(ErrorCode::OutOfRange, "derived rate above configured limit");
  }

  // --- 3. interval --------------------------------------------------------
  u64 interval_ns = 0;
  if (!derive_interval_ns(input.quantum_bytes, target, interval_ns)) {
    return Status::of(ErrorCode::ArithmeticOverflow, "interval derivation overflowed");
  }
  if (input.shape == CadenceShape::Windowed && input.grants_per_window_hint != 0) {
    u64 hinted_interval = 0;
    if (!checked::mul_div_ceil(input.window_ns, 1, input.grants_per_window_hint, hinted_interval)) {
      return Status::of(ErrorCode::ArithmeticOverflow, "windowed interval derivation overflowed");
    }
    // Take the sparser of the two: never denser than the authorized rate.
    if (hinted_interval > interval_ns) interval_ns = hinted_interval;
  }
  if (interval_ns == 0) {
    return Status::of(ErrorCode::Internal, "derived interval collapsed to zero");
  }
  if (interval_ns > limits.max_interval_ns) {
    return Status::of(ErrorCode::OutOfRange, "derived interval above configured limit");
  }

  // --- 4. window accounting ----------------------------------------------
  const u64 grants_per_window = input.window_ns / interval_ns;
  u64 bytes_per_window = 0;
  if (!checked::mul(grants_per_window, input.quantum_bytes, bytes_per_window)) {
    return Status::of(ErrorCode::ArithmeticOverflow, "window byte accounting overflowed");
  }

  // --- 5. burst allowance -------------------------------------------------
  u64 burst_bytes = input.burst_bytes;
  u64 burst_packets = input.burst_packets;
  if (input.flow_max_burst_bytes != 0 &&
      (burst_bytes == 0 || input.flow_max_burst_bytes < burst_bytes)) {
    burst_bytes = input.flow_max_burst_bytes;
  }
  if (input.flow_max_burst_packets != 0 &&
      (burst_packets == 0 || input.flow_max_burst_packets < burst_packets)) {
    burst_packets = input.flow_max_burst_packets;
  }
  if (burst_bytes == 0) burst_bytes = input.quantum_bytes;
  if (burst_packets == 0) burst_packets = 1;
  if (burst_bytes < input.quantum_bytes) {
    return Status::of(ErrorCode::InvalidArgument, "burst allowance is smaller than one quantum");
  }
  if (burst_bytes > limits.max_burst_bytes) {
    return Status::of(ErrorCode::BurstExceedsBound, "derived burst bytes above bound");
  }
  if (burst_packets > limits.max_burst_packets) {
    return Status::of(ErrorCode::BurstExceedsBound, "derived burst packets above bound");
  }
  if (input.shape == CadenceShape::TokenBucket) {
    u64 capacity = 0;
    if (!burst_capacity_bytes(target, limits.max_burst_horizon_ns, capacity)) {
      return Status::of(ErrorCode::ArithmeticOverflow, "burst capacity derivation overflowed");
    }
    if (burst_bytes > capacity) {
      return Status::of(ErrorCode::BurstExceedsBound,
                        "token bucket depth exceeds the capacity implied by the authorized rate");
    }
  }

  // --- 6. assemble and re-check ------------------------------------------
  Cadence cadence{};
  cadence.rate_bps = target;
  cadence.quantum_bytes = input.quantum_bytes;
  cadence.interval_ns = interval_ns;
  cadence.window_ns = input.window_ns;
  cadence.grants_per_window = grants_per_window;
  cadence.bytes_per_window = bytes_per_window;
  cadence.burst_bytes = burst_bytes;
  cadence.burst_packets = burst_packets;
  cadence.shape = input.shape;

  Status check = validate_cadence(cadence, limits.max_rate_bps, limits.max_burst_bytes,
                                  limits.max_burst_packets);
  if (!check) return check;

  u64 effective = 0;
  if (!cadence.effective_rate_bps(effective)) {
    return Status::of(ErrorCode::ArithmeticOverflow, "effective rate computation overflowed");
  }
  if (effective > input.ceiling_bps) {
    return Status::of(ErrorCode::CeilingExceeded,
                      "realized cadence would exceed the upstream authorized ceiling");
  }

  DerivationOutcome outcome{};
  outcome.cadence = cadence;
  outcome.requested_rate_bps = input.requested_rate_bps;
  outcome.clamped_to_ceiling = clamped_to_ceiling;
  outcome.clamped_to_floor = clamped_to_floor;
  return outcome;
}

}  // namespace pacing
