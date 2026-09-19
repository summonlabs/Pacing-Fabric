// Pacing Fabric - observation surface.
//
// Events are emitted strictly after internal locks are released, so a sink may
// call back into the fabric without deadlocking. Sinks must not throw; a sink
// that throws is a programming error and is contained by the fabric.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_EVENTS_HPP
#define PACING_FABRIC_EVENTS_HPP

#include <string>

#include "pacing/application.hpp"

namespace pacing {

enum class EventKind : u8 {
  EnvelopeDerived = 0,
  EnvelopeRefused,
  EnvelopeRevoked,
  ApplyReserved,
  ApplyAcknowledged,
  ApplyVerified,
  ApplyUnverified,
  ApplyMismatched,
  ApplyRefused,
  AttemptCancelled,
  AttemptRevalidated,
  AttemptStale,
  Fenced,
  EpochAdvanced,
  RecoveryCompleted,
  BackendRegistered,
};

std::string_view to_string(EventKind kind) noexcept;

struct FabricEvent {
  EventKind kind{EventKind::EnvelopeDerived};
  Instant at{};
  FlowId flow{};
  EnvelopeRef envelope{};
  AttemptId attempt{};
  ApplicationState state{ApplicationState::Unknown};
  EffectLabel effect{EffectLabel::None};
  u64 authority_digest{0};
  ErrorCode reason{ErrorCode::Ok};
  std::string detail{};
};

class IEventSink {
 public:
  virtual ~IEventSink() = default;
  virtual void on_event(const FabricEvent& event) noexcept = 0;
};

}  // namespace pacing

#endif  // PACING_FABRIC_EVENTS_HPP
