// Pacing Fabric - the pacing envelope: the authorized answer to "what pacing
// is permitted right now, for exactly which evidence".
//
// An envelope is intent plus authority. It is not an effect, it is not
// entitlement, and it is invalid the instant any generation it binds moves.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_ENVELOPE_HPP
#define PACING_FABRIC_ENVELOPE_HPP

#include <string>

#include "pacing/id.hpp"
#include "pacing/limits.hpp"
#include "pacing/rate.hpp"
#include "pacing/status.hpp"
#include "pacing/time.hpp"

namespace pacing {

// Chain of derivation inputs that produced an envelope. Every field is a
// digest of exact evidence, so two envelopes with the same provenance had
// byte-identical inputs.
struct Provenance {
  ProvenanceId id{};
  u64 sequence{0};
  u64 fabric_instance{0};
  u64 binding_digest{0};
  u64 policy_digest{0};
  u64 grant_digest{0};
  u64 cadence_digest{0};

  [[nodiscard]] bool valid() const noexcept { return id.valid(); }
  [[nodiscard]] u64 digest() const noexcept;
};

struct PacingEnvelope {
  EnvelopeRef ref{};
  FlowId flow{};
  ResourceId resource{};
  AuthorityVector authority{};
  Cadence cadence{};

  // Request accounting: what the policy asked for, and what upstream allowed.
  u64 requested_rate_bps{0};
  u64 ceiling_bps{0};
  u64 floor_bps{0};
  bool clamped_to_ceiling{false};
  bool clamped_to_floor{false};

  ServiceClassId service_class{};
  Provenance provenance{};

  Instant derived_at{};
  Deadline valid_until{};

  // Honesty flags propagated from the inputs, never inferred at display time.
  // upstream_synthetic: the rate ceiling came from a synthetic stand-in rather
  //   than a real external arbiter.
  // policy_synthetic: the governing policy was authored without provenance, so
  //   it is a locally configured stand-in rather than a traceable publication.
  bool upstream_synthetic{false};
  bool policy_synthetic{false};

  [[nodiscard]] u64 digest() const noexcept;
  [[nodiscard]] bool expired_at(Instant now) const noexcept { return valid_until.expired_at(now); }

  [[nodiscard]] std::string to_string() const;
};

// Derives a cadence from a resolved policy, an upstream grant and the flow
// declared bounds. Pure function: no clock, no I/O, no state. Returns a
// refusal rather than an approximation when authority is insufficient.
// Derivation rules that are deliberate, not incidental:
//   * the upstream ceiling is a hard upper bound and is never exceeded, not
//     even by one quantum of rounding;
//   * the interval is a ceiling division, so the realized rate is <= the
//     requested rate by construction;
//   * a zero burst allowance is interpreted as "one quantum in flight" and a
//     non-zero allowance below one quantum is rejected, never silently raised;
//   * a flow-declared bound may only tighten the policy allowance.
struct DerivationInput {
  u64 requested_rate_bps{0};
  u64 rate_share_ppm{1'000'000ull};
  u64 min_rate_bps{0};
  u64 ceiling_bps{0};
  u64 floor_bps{0};
  u64 quantum_bytes{0};
  u64 window_ns{0};
  u64 burst_bytes{0};
  u64 burst_packets{0};
  u64 flow_max_burst_bytes{0};
  u64 flow_max_burst_packets{0};
  CadenceShape shape{CadenceShape::Uniform};
  u64 grants_per_window_hint{0};
};

struct DerivationOutcome {
  Cadence cadence{};
  u64 requested_rate_bps{0};
  bool clamped_to_ceiling{false};
  bool clamped_to_floor{false};
};

[[nodiscard]] StatusOr<DerivationOutcome> derive_cadence(const DerivationInput& input,
                                                         const Limits& limits) noexcept;

}  // namespace pacing

#endif  // PACING_FABRIC_ENVELOPE_HPP
