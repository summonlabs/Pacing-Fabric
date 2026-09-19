// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/application.hpp"

namespace pacing {

std::string_view to_string(ApplicationState state) noexcept {
  switch (state) {
    case ApplicationState::Unknown: return "UNKNOWN";
    case ApplicationState::Planned: return "PLANNED";
    case ApplicationState::Reserved: return "RESERVED";
    case ApplicationState::Submitted: return "SUBMITTED";
    case ApplicationState::Acknowledged: return "ACKNOWLEDGED";
    case ApplicationState::Applied: return "APPLIED";
    case ApplicationState::AppliedUnverified: return "APPLIED_UNVERIFIED";
    case ApplicationState::Mismatched: return "MISMATCHED";
    case ApplicationState::RequiresRevalidation: return "REQUIRES_REVALIDATION";
    case ApplicationState::Revoked: return "REVOKED";
    case ApplicationState::Fenced: return "FENCED";
    case ApplicationState::Stale: return "STALE";
    case ApplicationState::Failed: return "FAILED";
    case ApplicationState::Cancelled: return "CANCELLED";
    case ApplicationState::Ambiguous: return "AMBIGUOUS";
  }
  return "UNKNOWN";
}

bool is_terminal(ApplicationState state) noexcept {
  switch (state) {
    case ApplicationState::Applied:
    case ApplicationState::AppliedUnverified:
    case ApplicationState::Mismatched:
    case ApplicationState::RequiresRevalidation:
    case ApplicationState::Revoked:
    case ApplicationState::Fenced:
    case ApplicationState::Stale:
    case ApplicationState::Failed:
    case ApplicationState::Cancelled:
    case ApplicationState::Ambiguous:
      return true;
    case ApplicationState::Unknown:
    case ApplicationState::Planned:
    case ApplicationState::Reserved:
    case ApplicationState::Submitted:
    case ApplicationState::Acknowledged:
      return false;
  }
  return true;
}

bool asserts_effect(ApplicationState state) noexcept { return state == ApplicationState::Applied; }

std::string_view to_string(EffectLabel label) noexcept {
  switch (label) {
    case EffectLabel::None: return "NONE";
    case EffectLabel::Verified: return "VERIFIED";
    case EffectLabel::SyntheticVerified: return "SYNTHETIC_VERIFIED";
    case EffectLabel::Unverified: return "UNVERIFIED";
    case EffectLabel::Mismatched: return "MISMATCHED";
  }
  return "NONE";
}

u64 ApplicationRecord::digest() const noexcept {
  Digest64 d;
  d.mix_u64(attempt.value());
  d.mix_u64(envelope.id.value());
  d.mix_u64(envelope.generation.value());
  d.mix_u64(flow.value());
  d.mix_u64(resource.value());
  d.mix_u64(authority.digest());
  d.mix_u64(desired.rate_bps);
  d.mix_u64(desired.quantum_bytes);
  d.mix_u64(desired.interval_ns);
  d.mix_u64(desired.window_ns);
  d.mix_u64(desired.grants_per_window);
  d.mix_u64(desired.burst_bytes);
  d.mix_u64(desired.burst_packets);
  d.mix_u64(static_cast<u64>(state));
  d.mix_u64(static_cast<u64>(effect));
  d.mix_u64(static_cast<u64>(evidence_kind));
  d.mix_u64(observed_rate_bps);
  d.mix_u64(observed_burst_bytes);
  d.mix_u64(backend_seq);
  d.mix_u64(evidence_digest);
  d.mix_bool(evidence_integrity_ok);
  d.mix_u64(backend.id.value());
  d.mix_u64(backend.generation.value());
  d.mix_bool(backend_synthetic);
  d.mix_u64(static_cast<u64>(reason));
  d.mix_u64(static_cast<u64>(drift));
  d.mix_u64(revision);
  d.mix_bool(requires_revalidation);
  return d.value();
}

}  // namespace pacing
