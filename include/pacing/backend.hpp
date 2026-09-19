// Pacing Fabric - narrow pacing backend abstraction.
//
// A backend is the only thing that can turn intent into an effect, and the
// fabric never assumes that an acknowledgement is that effect. Backends are
// labelled REAL or SYNTHETIC; the label travels with every piece of evidence.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_BACKEND_HPP
#define PACING_FABRIC_BACKEND_HPP

#include <memory>
#include <string>
#include <string_view>

#include "pacing/envelope.hpp"
#include "pacing/status.hpp"

namespace pacing {

enum class BackendNature : u8 {
  Real = 0,       // drives an actual pacing mechanism that this process can observe
  Synthetic = 1,  // software stand-in: bookkeeping only, no physical effect claimed
};

std::string_view to_string(BackendNature nature) noexcept;

struct BackendDescriptor {
  BackendRef ref{};
  std::string name{};
  BackendNature nature{BackendNature::Synthetic};
  bool supports_readback{false};
  u64 max_rate_bps{0};
  u64 max_burst_bytes{0};
  // Free-form capability bits; the fabric treats unknown bits as unsupported.
  u64 capability_flags{0};

  [[nodiscard]] bool usable() const noexcept { return ref.known() && !name.empty(); }
};

struct ApplyRequest {
  AttemptId attempt{};
  EnvelopeRef envelope{};
  FlowId flow{};
  AuthorityVector authority{};
  Cadence cadence{};
  WorkerBoot worker{};
  u64 issued_at_ns{0};
  u64 deadline_ns{0};
};

struct ApplyAck {
  AttemptId attempt{};
  bool accepted{false};
  u64 backend_seq{0};
  std::string detail{};
};

// Classification of the evidence behind a claimed effect.
enum class EvidenceKind : u8 {
  None = 0,                // no evidence at all: effect is UNKNOWN
  BackendReadback = 1,     // readback from a REAL backend
  SyntheticReadback = 2,   // readback from a SYNTHETIC backend
  Injected = 3,            // evidence supplied by a fault-injection harness
};

std::string_view to_string(EvidenceKind kind) noexcept;

struct ReadbackEvidence {
  bool present{false};
  EvidenceKind kind{EvidenceKind::None};
  BackendRef backend{};
  AttemptId attempt{};
  u64 observed_rate_bps{0};
  u64 observed_quantum_bytes{0};
  u64 observed_burst_bytes{0};
  u64 observed_interval_ns{0};
  u64 backend_seq{0};
  u64 observed_at_ns{0};
  bool integrity_ok{false};
  u64 digest{0};
};

struct BackendStats {
  u64 applies_accepted{0};
  u64 applies_rejected{0};
  u64 readbacks_served{0};
  u64 readbacks_unavailable{0};
  u64 revokes{0};
  u64 epoch_discards{0};
};

// The narrow contract. Implementations must be thread-safe: the fabric calls
// them without holding any of its own locks, from arbitrary worker threads.
class IPacingBackend {
 public:
  virtual ~IPacingBackend() = default;

  [[nodiscard]] virtual BackendDescriptor describe() const = 0;

  // Attempt to install pacing. An accepted acknowledgement asserts only that
  // the request was recorded by the backend - never that an effect occurred.
  [[nodiscard]] virtual Status apply(const ApplyRequest& request, ApplyAck& ack) = 0;

  // Read back the effect actually in force. Returns ReadbackUnavailable when
  // the backend cannot observe its own effect; that is a legitimate answer and
  // must never be turned into a success.
  [[nodiscard]] virtual Status readback(const ApplyRequest& request, ReadbackEvidence& evidence) = 0;

  // Withdraw pacing. Idempotent: revoking an unknown attempt is success.
  [[nodiscard]] virtual Status revoke(const ApplyRequest& request, std::string_view reason) = 0;

  // Coordinator epoch changed: discard anything attributable to older epochs.
  // This never resurrects liveness - it only destroys stale authority.
  [[nodiscard]] virtual Status rebind_epoch(Epoch new_epoch, u64& discarded) = 0;

  [[nodiscard]] virtual BackendStats stats() const = 0;
};

using BackendPtr = std::shared_ptr<IPacingBackend>;

}  // namespace pacing

#endif  // PACING_FABRIC_BACKEND_HPP
