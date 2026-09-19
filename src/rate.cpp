// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/rate.hpp"

#include <string>

namespace pacing {
namespace {

constexpr u64 kBitsPerByte = 8;

}  // namespace

std::string_view to_string(CadenceShape shape) noexcept {
  switch (shape) {
    case CadenceShape::Uniform: return "uniform";
    case CadenceShape::Windowed: return "windowed";
    case CadenceShape::TokenBucket: return "token-bucket";
  }
  return "unknown";
}

bool parse_cadence_shape(std::string_view text, CadenceShape& out) noexcept {
  if (text == "uniform") {
    out = CadenceShape::Uniform;
    return true;
  }
  if (text == "windowed") {
    out = CadenceShape::Windowed;
    return true;
  }
  if (text == "token-bucket") {
    out = CadenceShape::TokenBucket;
    return true;
  }
  return false;
}

bool derive_interval_ns(u64 quantum_bytes, u64 rate_bps, u64& interval_ns) noexcept {
  if (quantum_bytes == 0 || rate_bps == 0) return false;
  // interval = ceil(quantum_bytes * 8 * 1e9 / rate_bps). Rounding the interval
  // up guarantees the realized rate never exceeds the requested rate.
  u64 numerator_ns = 0;
  if (!checked::mul(quantum_bytes, kBitsPerByte * kNanosPerSecond, numerator_ns)) return false;
  return checked::mul_div_ceil(numerator_ns, 1, rate_bps, interval_ns) && interval_ns != 0;
}

bool burst_capacity_bytes(u64 rate_bps, u64 horizon_ns, u64& out) noexcept {
  if (rate_bps == 0) {
    out = 0;
    return true;
  }
  u64 bits = 0;
  if (!checked::mul(rate_bps, horizon_ns, bits)) return false;
  return checked::mul_div_floor(bits, 1, kBitsPerByte * kNanosPerSecond, out);
}

bool Cadence::effective_rate_bps(u64& out) const noexcept {
  if (window_ns == 0) return false;
  u64 bits = 0;
  if (!checked::mul(bytes_per_window, kBitsPerByte * kNanosPerSecond, bits)) return false;
  return checked::mul_div_floor(bits, 1, window_ns, out);
}

std::string Cadence::to_string() const {
  std::string out;
  out.reserve(160);
  out += "rate=";
  out += std::to_string(rate_bps);
  out += "bps quantum=";
  out += std::to_string(quantum_bytes);
  out += "B interval=";
  out += std::to_string(interval_ns);
  out += "ns window=";
  out += std::to_string(window_ns);
  out += "ns grants/window=";
  out += std::to_string(grants_per_window);
  out += " bytes/window=";
  out += std::to_string(bytes_per_window);
  out += " burst=";
  out += std::to_string(burst_bytes);
  out += "B/";
  out += std::to_string(burst_packets);
  out += "pkt shape=";
  out.append(pacing::to_string(shape));
  return out;
}

Status validate_cadence(const Cadence& cadence, u64 max_rate_bps, u64 max_burst_bytes,
                        u64 max_burst_packets) noexcept {
  if (cadence.rate_bps == 0) return Status::of(ErrorCode::OutOfRange, "cadence rate is zero");
  if (cadence.rate_bps > max_rate_bps) return Status::of(ErrorCode::OutOfRange, "cadence rate above limit");
  if (cadence.quantum_bytes == 0) return Status::of(ErrorCode::InvalidArgument, "cadence quantum is zero");
  if (cadence.interval_ns == 0) return Status::of(ErrorCode::InvalidArgument, "cadence interval is zero");
  if (cadence.window_ns == 0) return Status::of(ErrorCode::InvalidArgument, "cadence window is zero");
  if (cadence.burst_bytes == 0) return Status::of(ErrorCode::InvalidArgument, "cadence burst is zero");
  if (cadence.burst_packets == 0) return Status::of(ErrorCode::InvalidArgument, "cadence burst packets is zero");
  if (cadence.burst_bytes > max_burst_bytes) {
    return Status::of(ErrorCode::BurstExceedsBound, "cadence burst exceeds byte bound");
  }
  if (cadence.burst_packets > max_burst_packets) {
    return Status::of(ErrorCode::BurstExceedsBound, "cadence burst exceeds packet bound");
  }
  u64 bytes_per_window = 0;
  if (!checked::mul(cadence.grants_per_window, cadence.quantum_bytes, bytes_per_window)) {
    return Status::of(ErrorCode::ArithmeticOverflow, "cadence window bytes overflow");
  }
  if (bytes_per_window != cadence.bytes_per_window) {
    return Status::of(ErrorCode::InvalidState, "cadence window accounting is inconsistent");
  }
  u64 effective = 0;
  if (!cadence.effective_rate_bps(effective)) {
    return Status::of(ErrorCode::ArithmeticOverflow, "cadence effective rate overflow");
  }
  if (effective > cadence.rate_bps) {
    return Status::of(ErrorCode::CeilingExceeded, "cadence effective rate exceeds authorized rate");
  }
  return Status::success();
}

}  // namespace pacing
