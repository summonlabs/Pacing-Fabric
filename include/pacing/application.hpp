// Pacing Fabric - application state governance.
//
// The ledger is the authoritative record of what pacing was *intended*, what
// was *acknowledged* and what was actually *observed in force*. Those three
// are different states on purpose and are never collapsed into one flag.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_APPLICATION_HPP
#define PACING_FABRIC_APPLICATION_HPP

#include <string>

#include "pacing/backend.hpp"
#include "pacing/envelope.hpp"

namespace pacing {

// Lifecycle of one application attempt.
//
//   Planned -> Reserved -> Submitted -> Acknowledged -> Applied
//                                                    \-> AppliedUnverified
//                                                    \-> Mismatched
//
// Terminal without effect: Cancelled, Failed, Fenced, Stale, Revoked.
// Ambiguous marks an attempt whose outcome is genuinely unknown (worker died
// mid-apply); it is never resolved by assumption.
enum class ApplicationState : u8 {
  Unknown = 0,
  Planned,
  Reserved,
  Submitted,
  Acknowledged,
  Applied,
  AppliedUnverified,
  Mismatched,
  RequiresRevalidation,
  Revoked,
  Fenced,
  Stale,
  Failed,
  Cancelled,
  Ambiguous,
};

std::string_view to_string(ApplicationState state) noexcept;

// True when the state is terminal: no later transition may occur.
bool is_terminal(ApplicationState state) noexcept;

// True when the state asserts that pacing is in force. Only Applied does, and
// only until evidence is invalidated.
bool asserts_effect(ApplicationState state) noexcept;

// Provenance of a claimed effect. Backend acknowledgement is NOT an effect.
enum class EffectLabel : u8 {
  None = 0,               // no effect claimed (UNKNOWN)
  Verified = 1,           // readback verified on a REAL backend
  SyntheticVerified = 2,  // readback verified on a SYNTHETIC backend
  Unverified = 3,         // backend acknowledged, no readback available
  Mismatched = 4,         // readback disagrees with the authorized envelope
};

std::string_view to_string(EffectLabel label) noexcept;

struct ApplicationRecord {
  AttemptId attempt{};
  EnvelopeRef envelope{};
  FlowId flow{};
  ResourceId resource{};
  AuthorityVector authority{};
  Cadence desired{};
  ApplicationState state{ApplicationState::Unknown};
  // Preserved when recovery rewrites state, so lineage is never lost.
  ApplicationState last_known_state{ApplicationState::Unknown};
  EffectLabel effect{EffectLabel::None};
  EvidenceKind evidence_kind{EvidenceKind::None};

  u64 observed_rate_bps{0};
  u64 observed_quantum_bytes{0};
  u64 observed_burst_bytes{0};
  u64 observed_interval_ns{0};
  u64 backend_seq{0};
  u64 evidence_digest{0};
  bool evidence_integrity_ok{false};

  BackendRef backend{};
  WorkerBoot worker{};
  bool backend_synthetic{true};

  ErrorCode reason{ErrorCode::Ok};
  AuthorityDrift drift{AuthorityDrift::None};
  std::string detail{};

  u64 revision{0};
  u64 created_ns{0};
  u64 updated_ns{0};
  // Set whenever durable state was reloaded: no persisted effect is current
  // until it has been revalidated against live authority and a live backend.
  bool requires_revalidation{false};

  [[nodiscard]] bool terminal() const noexcept { return is_terminal(state); }
  [[nodiscard]] bool claims_effect() const noexcept {
    return asserts_effect(state) && !requires_revalidation;
  }
  [[nodiscard]] u64 digest() const noexcept;
};

}  // namespace pacing

#endif  // PACING_FABRIC_APPLICATION_HPP
