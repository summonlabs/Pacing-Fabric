// Pacing Fabric - the coordinator.
//
// Owns pacing-policy authority and pacing application-state governance for a
// set of generation-bound flows and resources. It does not own bandwidth
// arbitration, rate entitlement, admission, path placement, queue scheduling,
// congestion state or devices; those enter only through IRateAuthority and
// IPacingBackend.
//
// Concurrency contract
// -------------------
// * All public methods are thread-safe.
// * No external code (backend, rate authority, clock, event sink) is ever
//   invoked while an internal lock is held. Backend and authority calls are
//   made outside the state lock and their results are folded back in with a
//   re-validation step that discards late, stale or cancelled work.
// * Events are emitted after the state lock is released.
// * Lock order is fabric state lock -> durable store lock. The durable store
//   never calls back into the fabric.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_FABRIC_HPP
#define PACING_FABRIC_FABRIC_HPP

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pacing/binding.hpp"
#include "pacing/durable.hpp"
#include "pacing/events.hpp"
#include "pacing/explain.hpp"
#include "pacing/policy.hpp"
#include "pacing/serialize.hpp"
#include "pacing/upstream.hpp"

namespace pacing {

struct FabricConfig {
  Limits limits{};
  std::string durable_directory{};   // empty: process-local state only
  u64 fabric_instance{1};            // distinct per coordinator instance
  u64 tick_period_ns{1'000'000ull};  // control-plane tick, >= 1us
  u64 envelope_lifetime_ns{1'000'000'000ull};
  bool enable_durability{true};
  u32 max_pending_attempts{4096};
};

struct FabricStats {
  u64 envelopes_derived{0};
  u64 envelopes_refused{0};
  u64 applies_requested{0};
  u64 applies_reserved{0};
  u64 applies_committed{0};
  u64 applies_unverified{0};
  u64 applies_mismatched{0};
  u64 applies_refused{0};
  u64 duplicate_applies{0};
  u64 late_completions_discarded{0};
  u64 cancellations{0};
  u64 revocations{0};
  u64 fences{0};
  u64 stale_rejections{0};
  u64 revalidations{0};
  u64 readback_unavailable{0};
  u64 events_emitted{0};
  u64 journal_appends{0};
  u64 checkpoints{0};
  u64 envelopes_evicted{0};
  u64 attempts_evicted{0};
};

struct RecoveryReport {
  bool performed{false};
  bool durable_directory_used{false};
  u64 bindings_restored{0};
  u64 policies_restored{0};
  u64 envelopes_invalidated{0};
  u64 attempts_requiring_revalidation{0};
  u64 attempts_ambiguous{0};
  u64 backends_requiring_registration{0};
  u64 records_replayed{0};
  u64 records_rejected{0};
  Epoch epoch_before{};
  Epoch epoch_after{};
  BootId boot_before{};
  BootId boot_after{};
  ErrorCode code{ErrorCode::Ok};
  std::string detail{};
};

// Monotonic identity minting. Separate counters per domain keep ids readable
// and guarantee that an id is never reused within a fabric instance.
class IdMinter {
 public:
  u64 next() noexcept { return ++counter_; }
  u64 peek() const noexcept { return counter_; }
  void set_floor(u64 value) noexcept {
    if (value > counter_) counter_ = value;
  }

 private:
  u64 counter_{0};
};

class PacingFabric {
 public:
  PacingFabric(FabricConfig config, IRateAuthority& rate_authority, IClock& clock);
  ~PacingFabric();

  PacingFabric(const PacingFabric&) = delete;
  PacingFabric& operator=(const PacingFabric&) = delete;

  // --- lifecycle ---------------------------------------------------------
  [[nodiscard]] Status initialize();
  Status shutdown();
  [[nodiscard]] bool running() const noexcept;

  [[nodiscard]] Epoch epoch() const noexcept;
  [[nodiscard]] BootId boot() const noexcept;
  [[nodiscard]] RecoveryReport recovery_report() const;
  [[nodiscard]] DurabilityReport durability_report() const;
  [[nodiscard]] Tick current_tick() const noexcept;

  // Advances the coordinator epoch, fences every record stamped with an older
  // epoch, and instructs registered backends to discard stale-epoch state.
  [[nodiscard]] Status advance_epoch();

  // --- bindings ----------------------------------------------------------
  [[nodiscard]] Status bind_flow(FlowBinding binding);
  [[nodiscard]] Status unbind_flow(FlowId flow);
  [[nodiscard]] StatusOr<FlowBinding> binding(FlowId flow) const;

  // --- backends ----------------------------------------------------------
  [[nodiscard]] Status register_backend(BackendPtr backend);
  Status unregister_backend(BackendId id);
  [[nodiscard]] std::vector<BackendDescriptor> list_backends() const;

  // --- policy ------------------------------------------------------------
  [[nodiscard]] StatusOr<PolicyRef> publish_policy(PacingPolicy policy);
  Status remove_policy(PolicyId id);
  [[nodiscard]] StatusOr<PacingPolicy> policy(PolicyId id) const;

  // --- derivation --------------------------------------------------------
  [[nodiscard]] StatusOr<PacingEnvelope> derive_envelope(FlowId flow);
  [[nodiscard]] StatusOr<PacingEnvelope> envelope(EnvelopeId id) const;

  // --- application -------------------------------------------------------
  // Idempotent: repeating a call for the same envelope and backend (or
  // supplying an attempt id that already exists) returns the existing attempt
  // and performs no second backend mutation.
  [[nodiscard]] StatusOr<AttemptId> apply_envelope(EnvelopeRef envelope, BackendId backend,
                                                   AttemptId requested = {});
  [[nodiscard]] Status verify_attempt(AttemptId attempt);
  [[nodiscard]] Status cancel_attempt(AttemptId attempt, std::string_view reason);
  [[nodiscard]] StatusOr<ApplicationRecord> attempt(AttemptId id) const;

  // --- governance --------------------------------------------------------
  [[nodiscard]] Status revoke_envelope(EnvelopeRef envelope, std::string_view reason);
  [[nodiscard]] Status fence_below(Epoch epoch);
  // Re-establishes that a previously applied record is still in force, by
  // re-checking live authority and re-reading the backend. Only a positive,
  // matching readback clears requires_revalidation.
  [[nodiscard]] Status revalidate(AttemptId attempt);

  // --- observation -------------------------------------------------------
  [[nodiscard]] StatusOr<Explanation> explain(FlowId flow) const;
  [[nodiscard]] FabricStats stats() const;
  [[nodiscard]] std::vector<ApplicationRecord> attempts_for_flow(FlowId flow) const;
  [[nodiscard]] std::vector<PacingEnvelope> envelopes_for_flow(FlowId flow) const;

  void set_event_sink(IEventSink* sink) noexcept;
  [[nodiscard]] const Limits& limits() const noexcept { return config_.limits; }

 private:
  struct EnvelopeEntry {
    PacingEnvelope envelope{};
    bool revoked{false};
    bool live{true};
    std::string revoke_reason{};
  };

  struct AttemptEntry {
    ApplicationRecord record{};
    bool live{true};
  };

  // Authority inputs captured from a lock-free fetch phase.
  struct AuthorityInputs {
    FlowBinding binding{};
    PacingPolicy policy{};
    RateGrant grant{};
    AuthorityVector vector{};
  };

  // Fetches binding + policy under the lock, performs the external rate
  // authority call with no lock held, then re-acquires to stamp epoch/boot.
  [[nodiscard]] StatusOr<AuthorityInputs> fetch_authority(FlowId flow) const;

  // Samples the injected clock and remembers the highest instant seen. A clock
  // that moves backwards makes time untrustworthy, so authorization is refused
  // rather than derived from a possibly resurrected expiry.
  [[nodiscard]] Instant sample_clock() const noexcept;
  [[nodiscard]] bool clock_regressed() const noexcept;
  [[nodiscard]] Status require_trustworthy_clock() const;

 public:
  // True once the injected clock has been observed moving backwards. While set,
  // no pacing authority is derivable, because an envelope expiry computed from
  // an untrustworthy instant could resurrect authority that had lapsed.
  [[nodiscard]] bool observed_clock_regression() const noexcept { return clock_regressed(); }

 private:

  [[nodiscard]] StatusOr<PacingEnvelope> derive_envelope_internal(const AuthorityInputs& inputs,
                                                                  Instant now);
  [[nodiscard]] std::size_t find_attempt(AttemptId id) const;
  [[nodiscard]] std::size_t find_envelope(EnvelopeId id) const;
  [[nodiscard]] std::size_t find_backend(BackendId id) const;
  [[nodiscard]] BackendPtr backend_ptr(BackendId id) const;

  [[nodiscard]] Status journal_record(DurableRecordType type, std::vector<u8> payload, u64 timestamp_ns);
  [[nodiscard]] Status maybe_checkpoint_locked();
  [[nodiscard]] Status recover_locked();
  [[nodiscard]] Status advance_epoch_locked(Instant now);
  void evict_and_compact_locked();
  void compact_envelopes_locked();
  void compact_attempts_locked();
  [[nodiscard]] std::size_t find_existing_attempt(EnvelopeRef envelope, BackendId backend) const;
  [[nodiscard]] StatusOr<std::vector<u8>> encode_attempt_payload(const ApplicationRecord& record);
  [[nodiscard]] StatusOr<std::vector<u8>> encode_envelope_payload(const PacingEnvelope& envelope);
  [[nodiscard]] Status finalize_evidence_locked(AttemptEntry& entry, const PacingEnvelope& envelope,
                                                const ReadbackEvidence& evidence, bool readback_ok,
                                                ErrorCode readback_code, Instant now);

  void emit(FabricEvent event) noexcept;
  void apply_transition(ApplicationRecord& record, ApplicationState next, Instant now, ErrorCode reason,
                        std::string detail);

  // RAII guard proving that a coordinator operation is in flight. Shutdown
  // waits for the count to drain, releasing the lock while it waits, so an
  // in-flight backend call can never race the durable store closing.
  struct OperationGuard {
    PacingFabric* fabric;
    explicit OperationGuard(PacingFabric* f) noexcept : fabric(f) {
      std::lock_guard<std::mutex> lock(fabric->mu_);
      ++fabric->active_operations_;
    }
    ~OperationGuard() {
      std::lock_guard<std::mutex> lock(fabric->mu_);
      --fabric->active_operations_;
      if (fabric->active_operations_ == 0) fabric->idle_cv_.notify_all();
    }
    OperationGuard(const OperationGuard&) = delete;
    OperationGuard& operator=(const OperationGuard&) = delete;
  };

  FabricConfig config_{};
  IRateAuthority* rate_authority_{nullptr};
  IClock* clock_{nullptr};

  mutable std::mutex mu_{};  // guards everything below
  std::condition_variable idle_cv_{};

  bool initialized_{false};
  bool running_{false};
  u32 active_operations_{0};
  u32 pending_attempts_{0};
  u64 dead_envelopes_{0};
  u64 dead_attempts_{0};
  std::size_t envelope_scan_hint_{0};
  std::size_t attempt_scan_hint_{0};
  BootId boot_{};
  Epoch epoch_{};
  u64 boot_nonce_counter_{0};
  u64 durable_sequence_{0};
  mutable std::mutex clock_mu_{};
  mutable u64 last_observed_ns_{0};
  mutable bool clock_regressed_{false};

  BindingRegistry bindings_;
  PolicyStore policies_;
  std::vector<EnvelopeEntry> envelopes_{};
  std::unordered_map<u64, std::size_t> envelope_index_{};
  std::unordered_map<u64, std::size_t> latest_envelope_by_flow_{};
  std::vector<AttemptEntry> attempts_{};
  std::unordered_map<u64, std::size_t> attempt_index_{};
  std::unordered_map<u64, std::vector<AttemptId>> attempts_by_envelope_{};

  std::vector<std::pair<BackendId, BackendPtr>> backends_{};

  IdMinter envelope_ids_{};
  IdMinter attempt_ids_{};
  IdMinter provenance_ids_{};

  mutable FabricStats stats_{};
  RecoveryReport recovery_{};
  std::unique_ptr<DurableStore> durable_{};
  IEventSink* event_sink_{nullptr};
};

}  // namespace pacing

#endif  // PACING_FABRIC_HPP
