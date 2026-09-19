// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/events.hpp"

namespace pacing {

std::string_view to_string(EventKind kind) noexcept {
  switch (kind) {
    case EventKind::EnvelopeDerived: return "envelope.derived";
    case EventKind::EnvelopeRefused: return "envelope.refused";
    case EventKind::EnvelopeRevoked: return "envelope.revoked";
    case EventKind::ApplyReserved: return "apply.reserved";
    case EventKind::ApplyAcknowledged: return "apply.acknowledged";
    case EventKind::ApplyVerified: return "apply.verified";
    case EventKind::ApplyUnverified: return "apply.unverified";
    case EventKind::ApplyMismatched: return "apply.mismatched";
    case EventKind::ApplyRefused: return "apply.refused";
    case EventKind::AttemptCancelled: return "attempt.cancelled";
    case EventKind::AttemptRevalidated: return "attempt.revalidated";
    case EventKind::AttemptStale: return "attempt.stale";
    case EventKind::Fenced: return "authority.fenced";
    case EventKind::EpochAdvanced: return "epoch.advanced";
    case EventKind::RecoveryCompleted: return "recovery.completed";
    case EventKind::BackendRegistered: return "backend.registered";
  }
  return "unknown";
}

}  // namespace pacing
