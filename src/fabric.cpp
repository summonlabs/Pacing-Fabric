// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/fabric.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace pacing {
namespace {

constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);
// Number of live entries examined per eviction attempt. Bounds the work a
// single derivation can do when the envelope table is at capacity.
constexpr std::size_t kEvictionScanWindow = 4096;
// A table is compacted once dead entries outnumber live ones.
constexpr u64 kCompactionTrigger = 2;

u64 splitmix64(u64 x) noexcept {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

u64 make_nonce(u64 salt) noexcept {
  static std::atomic<u64> counter{0};
  const u64 ticks = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
  const u64 seq = counter.fetch_add(1, std::memory_order_relaxed);
  u64 x = splitmix64(salt ^ ticks);
  x = splitmix64(x ^ (seq * 0x9E3779B97F4A7C15ull));
  return x == 0 ? 1 : x;
}

ErrorCode drift_code(AuthorityDrift drift) noexcept {
  switch (drift) {
    case AuthorityDrift::None: return ErrorCode::Ok;
    case AuthorityDrift::Flow: return ErrorCode::StaleGeneration;
    case AuthorityDrift::Resource: return ErrorCode::StaleResource;
    case AuthorityDrift::Policy: return ErrorCode::StalePolicy;
    case AuthorityDrift::RateGrant: return ErrorCode::StaleRateGrant;
    case AuthorityDrift::Path: return ErrorCode::StalePath;
    case AuthorityDrift::Epoch: return ErrorCode::StaleEpoch;
    case AuthorityDrift::CoordinatorBoot: return ErrorCode::StaleBoot;
    case AuthorityDrift::Incomplete: return ErrorCode::UnknownAuthority;
  }
  return ErrorCode::Internal;
}

u64 policy_digest(const PacingPolicy& policy) noexcept {
  Digest64 d;
  d.mix_u64(policy.ref.id.value());
  d.mix_u64(policy.ref.generation.value());
  d.mix_u64(static_cast<u64>(policy.layer));
  d.mix_u64(policy.resource.value());
  d.mix_u64(policy.flow.value());
  d.mix_u64(policy.rate_share_ppm);
  d.mix_u64(policy.rate_bps);
  d.mix_u64(policy.min_rate_bps);
  d.mix_u64(policy.burst_bytes);
  d.mix_u64(policy.burst_packets);
  d.mix_u64(policy.quantum_bytes);
  d.mix_u64(policy.window_ns);
  d.mix_u64(static_cast<u64>(policy.shape));
  d.mix_u64(policy.grants_per_window_hint);
  d.mix_u64(policy.path.id.value());
  d.mix_u64(policy.path.generation.value());
  d.mix_u64(policy.default_service_class.value());
  d.mix_u64(policy.tenant.value());
  d.mix_u64(policy.provenance.value());
  d.mix_u64(static_cast<u64>(policy.exceptions.size()));
  for (const auto& ex : policy.exceptions) {
    d.mix_u64(ex.service_class.value());
    d.mix_u64(ex.min_rate_bps);
    d.mix_u64(ex.rate_share_ppm);
    d.mix_u64(ex.burst_bytes);
    d.mix_u64(ex.burst_packets);
    d.mix_u64(static_cast<u64>(ex.priority));
  }
  return d.value();
}

u64 cadence_digest(const Cadence& cadence) noexcept {
  Digest64 d;
  d.mix_u64(cadence.rate_bps);
  d.mix_u64(cadence.quantum_bytes);
  d.mix_u64(cadence.interval_ns);
  d.mix_u64(cadence.window_ns);
  d.mix_u64(cadence.grants_per_window);
  d.mix_u64(cadence.bytes_per_window);
  d.mix_u64(cadence.burst_bytes);
  d.mix_u64(cadence.burst_packets);
  d.mix_u64(static_cast<u64>(cadence.shape));
  return d.value();
}

u64 evidence_digest(const ReadbackEvidence& evidence) noexcept {
  Digest64 d;
  d.mix_u64(evidence.backend.id.value());
  d.mix_u64(evidence.backend.generation.value());
  d.mix_u64(evidence.attempt.value());
  d.mix_u64(evidence.observed_rate_bps);
  d.mix_u64(evidence.observed_quantum_bytes);
  d.mix_u64(evidence.observed_burst_bytes);
  d.mix_u64(evidence.observed_interval_ns);
  d.mix_u64(evidence.backend_seq);
  d.mix_u64(evidence.observed_at_ns);
  d.mix_bool(evidence.integrity_ok);
  d.mix_u64(static_cast<u64>(evidence.kind));
  return d.value();
}

}  // namespace

PacingFabric::PacingFabric(FabricConfig config, IRateAuthority& rate_authority, IClock& clock)
    : config_(std::move(config)),
      rate_authority_(&rate_authority),
      clock_(&clock),
      bindings_(config_.limits),
      policies_(config_.limits) {}

PacingFabric::~PacingFabric() { (void)shutdown(); }

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

Status PacingFabric::initialize() {
  const Instant now = sample_clock();
  FabricEvent event{};
  bool have_event = false;
  Status result = Status::success();

  std::unique_lock<std::mutex> lock(mu_);
  if (initialized_) {
    result = Status::of(ErrorCode::InvalidState, "fabric is already initialized");
  } else if (!limits_are_coherent(config_.limits)) {
    result = Status::of(ErrorCode::InvalidArgument, "configured limits are incoherent");
  } else if (config_.tick_period_ns == 0 || config_.tick_period_ns > config_.limits.max_interval_ns) {
    result = Status::of(ErrorCode::InvalidArgument, "tick period is zero or above the configured bound");
  } else if (config_.envelope_lifetime_ns == 0 ||
             config_.envelope_lifetime_ns > config_.limits.max_envelope_lifetime_ns) {
    result = Status::of(ErrorCode::InvalidArgument, "envelope lifetime is zero or above the bound");
  } else if (config_.fabric_instance == 0) {
    result = Status::of(ErrorCode::InvalidArgument, "fabric instance identity must be non-zero");
  } else if (config_.max_pending_attempts == 0) {
    result = Status::of(ErrorCode::InvalidArgument, "pending attempt bound must be non-zero");
  } else {
    boot_ = BootId{};
    boot_.counter = 1;
    boot_.nonce = make_nonce(config_.fabric_instance);
    epoch_ = Epoch::from(1);
    recovery_ = RecoveryReport{};
    recovery_.boot_after = boot_;
    recovery_.epoch_after = epoch_;

    if (config_.enable_durability && !config_.durable_directory.empty()) {
      durable_ = std::make_unique<DurableStore>(config_.durable_directory, config_.limits);
      Status opened = durable_->open();
      if (!opened) {
        recovery_.code = opened.code();
        recovery_.detail = std::string(opened.detail());
        durable_.reset();
        result = opened;
      } else {
        recovery_.durable_directory_used = true;
        result = recover_locked();
        if (!result) {
          recovery_.code = result.code();
          if (recovery_.detail.empty()) recovery_.detail = std::string(result.detail());
        }
      }
    }

    if (result.ok()) {
      initialized_ = true;
      running_ = true;
      event.kind = EventKind::RecoveryCompleted;
      event.at = now;
      event.authority_digest = 0;
      event.detail = recovery_.performed ? "durable state recovered; all prior effects require revalidation"
                                         : "no durable state; coordinator started fresh";
      have_event = true;
    }
  }
  lock.unlock();

  if (have_event) emit(std::move(event));
  return result;
}

Status PacingFabric::shutdown() {
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (!initialized_) return Status::success();
    // Stop accepting new work, then wait for in-flight coordinator operations
    // to drain. The wait releases the lock, so an operation that is between
    // its backend call and its commit can still finish.
    running_ = false;
    idle_cv_.wait(lock, [this] { return active_operations_ == 0; });
  }

  // Close durable state with no lock held and no operation in flight.
  std::unique_ptr<DurableStore> store;
  {
    std::lock_guard<std::mutex> lock(mu_);
    store = std::move(durable_);
    initialized_ = false;
  }
  if (store) (void)store->close();

  std::lock_guard<std::mutex> lock(mu_);
  backends_.clear();
  return Status::success();
}

bool PacingFabric::running() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  return running_;
}

Epoch PacingFabric::epoch() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  return epoch_;
}

BootId PacingFabric::boot() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  return boot_;
}

RecoveryReport PacingFabric::recovery_report() const {
  std::lock_guard<std::mutex> lock(mu_);
  return recovery_;
}

DurabilityReport PacingFabric::durability_report() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!durable_) return DurabilityReport{};
  DurabilityReport report = durable_->report();
  // Overlay the live counters so a caller sees current growth, not just the
  // state observed at load time.
  report.journal_records = durable_->journal_records();
  return report;
}

Tick PacingFabric::current_tick() const noexcept {
  const Instant now = sample_clock();
  Tick tick{};
  if (!tick_for(now, config_.tick_period_ns, tick)) return Tick{};
  return tick;
}

Status PacingFabric::advance_epoch() {
  OperationGuard guard(this);
  const Instant now = sample_clock();
  FabricEvent event{};
  std::vector<std::pair<BackendId, BackendPtr>> backends;
  Epoch new_epoch{};

  {
    std::unique_lock<std::mutex> lock(mu_);
    if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
    Status s = advance_epoch_locked(now);
    if (!s) return s;
    new_epoch = epoch_;
    backends = backends_;
    event.kind = EventKind::EpochAdvanced;
    event.at = now;
    event.detail = "coordinator epoch advanced; all older-epoch authority fenced";
  }

  // Backends are told to destroy stale-epoch state with no lock held. This is
  // destruction of stale authority only: it never restores liveness.
  for (const auto& entry : backends) {
    if (!entry.second) continue;
    u64 discarded = 0;
    (void)entry.second->rebind_epoch(new_epoch, discarded);
  }
  emit(std::move(event));
  return Status::success();
}

Status PacingFabric::advance_epoch_locked(Instant now) {
  bool exhausted = false;
  Epoch next = epoch_.advanced(exhausted);
  if (exhausted) return Status::of(ErrorCode::ResourceExhausted, "coordinator epoch is exhausted");
  epoch_ = next;

  // Fence every envelope and every non-closed attempt stamped with an older
  // epoch. Fencing is a state change in its own right: it is recorded durably
  // before it is reported.
  for (auto& entry : envelopes_) {
    if (!entry.live) continue;
    if (entry.envelope.authority.epoch < epoch_ && !entry.revoked) {
      entry.revoked = true;
      entry.revoke_reason = "coordinator epoch advanced";
      stats_.fences += 1;
    }
  }
  for (auto& entry : attempts_) {
    if (!entry.live) continue;
    if (entry.record.authority.epoch >= epoch_) continue;
    switch (entry.record.state) {
      case ApplicationState::Revoked:
      case ApplicationState::Fenced:
      case ApplicationState::Stale:
      case ApplicationState::Failed:
      case ApplicationState::Cancelled:
      case ApplicationState::Ambiguous:
        continue;
      default:
        break;
    }
    // This includes records that were applied under the old epoch: their
    // authority is gone and the backend is instructed to discard that epoch's
    // state, so no effect may still be asserted.
    apply_transition(entry.record, ApplicationState::Fenced, now, ErrorCode::Fenced,
                     "coordinator epoch advanced past this attempt");
    stats_.fences += 1;
    auto payload = encode_attempt_payload(entry.record);
    if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
  }

  std::vector<u8> payload;
  {
    ByteWriter w(256);
    encode_epoch(w, epoch_, boot_);
    if (!w.ok()) return w.status();
    payload = w.take();
  }
  return journal_record(DurableRecordType::EpochAdvance, std::move(payload), now.ns());
}

// ---------------------------------------------------------------------------
// bindings, policy, backends
// ---------------------------------------------------------------------------

Status PacingFabric::bind_flow(FlowBinding binding) {
  OperationGuard guard(this);
  const Instant now = sample_clock();
  std::unique_lock<std::mutex> lock(mu_);
  if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
  Status s = bindings_.bind(binding);
  if (!s) return s;
  auto stored = bindings_.get(binding.flow);
  if (!stored) return stored.status();
  std::vector<u8> payload;
  {
    ByteWriter w(1024);
    encode_binding(w, stored.value());
    if (!w.ok()) return w.status();
    payload = w.take();
  }
  return journal_record(DurableRecordType::FlowBinding, std::move(payload), now.ns());
}

Status PacingFabric::unbind_flow(FlowId flow) {
  OperationGuard guard(this);
  const Instant now = sample_clock();
  std::unique_lock<std::mutex> lock(mu_);
  if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
  Status s = bindings_.unbind(flow);
  if (!s) return s;
  std::vector<u8> payload;
  {
    ByteWriter w(64);
    encode_identity_u64(w, flow.value());
    if (!w.ok()) return w.status();
    payload = w.take();
  }
  return journal_record(DurableRecordType::FlowUnbound, std::move(payload), now.ns());
}

StatusOr<FlowBinding> PacingFabric::binding(FlowId flow) const {
  std::lock_guard<std::mutex> lock(mu_);
  return bindings_.get(flow);
}

Status PacingFabric::register_backend(BackendPtr backend) {
  OperationGuard guard(this);
  const Instant now = sample_clock();
  if (!backend) return Status::of(ErrorCode::InvalidArgument, "backend handle is null");
  const BackendDescriptor desc = backend->describe();
  if (!desc.usable()) return Status::of(ErrorCode::InvalidArgument, "backend descriptor is incomplete");
  if (desc.name.size() > config_.limits.max_name_bytes) {
    return Status::of(ErrorCode::Oversized, "backend name above configured bound");
  }

  std::unique_lock<std::mutex> lock(mu_);
  if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
  const std::size_t existing = find_backend(desc.ref.id);
  if (existing != kNoIndex) {
    backends_[existing].second = std::move(backend);
  } else {
    if (backends_.size() >= config_.limits.max_backends) {
      return Status::of(ErrorCode::ResourceExhausted, "backend registry is full");
    }
    backends_.emplace_back(desc.ref.id, std::move(backend));
  }

  std::vector<u8> payload;
  {
    ByteWriter w(1024);
    encode_backend_descriptor(w, desc);
    if (!w.ok()) return w.status();
    payload = w.take();
  }
  Status s = journal_record(DurableRecordType::BackendRegistration, std::move(payload), now.ns());
  if (!s) return s;
  lock.unlock();

  FabricEvent event{};
  event.kind = EventKind::BackendRegistered;
  event.at = now;
  event.detail = desc.name;
  emit(std::move(event));
  return Status::success();
}

Status PacingFabric::unregister_backend(BackendId id) {
  OperationGuard guard(this);
  std::lock_guard<std::mutex> lock(mu_);
  const std::size_t idx = find_backend(id);
  if (idx == kNoIndex) return Status::of(ErrorCode::NotFound, "backend is not registered");
  backends_.erase(backends_.begin() + static_cast<std::ptrdiff_t>(idx));
  return Status::success();
}

std::vector<BackendDescriptor> PacingFabric::list_backends() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<BackendDescriptor> out;
  out.reserve(backends_.size());
  for (const auto& entry : backends_) {
    if (entry.second) out.push_back(entry.second->describe());
  }
  return out;
}

StatusOr<PolicyRef> PacingFabric::publish_policy(PacingPolicy policy) {
  OperationGuard guard(this);
  const Instant now = sample_clock();
  std::unique_lock<std::mutex> lock(mu_);
  if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
  if (!policy.published_at.ns()) policy.published_at = now;
  auto published = policies_.publish(std::move(policy));
  if (!published) return published.status();
  auto stored = policies_.get(published.value().id);
  if (!stored) return stored.status();
  std::vector<u8> payload;
  {
    ByteWriter w(kEncodeCapacityBytes);
    encode_policy(w, stored.value());
    if (!w.ok()) return w.status();
    payload = w.take();
  }
  Status s = journal_record(DurableRecordType::Policy, std::move(payload), now.ns());
  if (!s) return s;
  return published;
}

Status PacingFabric::remove_policy(PolicyId id) {
  OperationGuard guard(this);
  const Instant now = sample_clock();
  std::unique_lock<std::mutex> lock(mu_);
  if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
  Status s = policies_.remove(id);
  if (!s) return s;
  std::vector<u8> payload;
  {
    ByteWriter w(64);
    encode_identity_u64(w, id.value());
    if (!w.ok()) return w.status();
    payload = w.take();
  }
  return journal_record(DurableRecordType::PolicyRemoved, std::move(payload), now.ns());
}

StatusOr<PacingPolicy> PacingFabric::policy(PolicyId id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return policies_.get(id);
}

// ---------------------------------------------------------------------------
// authority fetch
// ---------------------------------------------------------------------------

Instant PacingFabric::sample_clock() const noexcept {
  const Instant now = clock_->now();
  std::lock_guard<std::mutex> lock(clock_mu_);
  if (now.ns() < last_observed_ns_) {
    clock_regressed_ = true;
  } else {
    last_observed_ns_ = now.ns();
  }
  return now;
}

bool PacingFabric::clock_regressed() const noexcept {
  std::lock_guard<std::mutex> lock(clock_mu_);
  return clock_regressed_;
}

Status PacingFabric::require_trustworthy_clock() const {
  if (!clock_regressed()) return Status::success();
  return Status::of(ErrorCode::ClockRegression,
                    "the monotonic clock moved backwards; pacing authority is not derivable");
}

StatusOr<PacingFabric::AuthorityInputs> PacingFabric::fetch_authority(FlowId flow) const {
  Status clock_status = require_trustworthy_clock();
  if (!clock_status) return clock_status;
  AuthorityInputs inputs{};

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto found_binding = bindings_.get(flow);
    if (!found_binding) return found_binding.status();
    inputs.binding = found_binding.value();
    if (!inputs.binding.usable()) {
      return Status::of(ErrorCode::UnknownAuthority, "flow binding is quiesced or not usable");
    }
    auto resolved = policies_.resolve(flow, inputs.binding.resource.id);
    if (!resolved) return resolved.status();
    inputs.policy = resolved.value();
    if (!(inputs.policy.path == inputs.binding.path)) {
      return Status::of(ErrorCode::StalePath,
                        "policy path generation differs from the binding path generation");
    }
  }

  // External call with no fabric lock held.
  auto fetched = rate_authority_->fetch(inputs.binding.resource.id);
  if (!fetched) return fetched.status();
  inputs.grant = fetched.value();
  if (!inputs.grant.usable()) {
    return Status::of(ErrorCode::UnknownAuthority,
                      "upstream rate authority is absent, zero or not authoritative");
  }

  const Instant now = sample_clock();
  if (inputs.grant.expired_at(now)) {
    return Status::of(ErrorCode::StaleRateGrant, "upstream rate grant has expired");
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    inputs.vector = AuthorityVector{};
    inputs.vector.flow = FlowRef{flow, inputs.binding.generation};
    inputs.vector.resource = inputs.binding.resource;
    inputs.vector.policy = inputs.policy.ref;
    inputs.vector.rate_grant = inputs.grant.ref;
    inputs.vector.path = inputs.binding.path;
    inputs.vector.epoch = epoch_;
    inputs.vector.coordinator_boot = boot_;
    if (!inputs.vector.complete()) {
      return Status::of(ErrorCode::UnknownAuthority, "authority vector is incomplete");
    }
  }
  return inputs;
}

// ---------------------------------------------------------------------------
// derivation
// ---------------------------------------------------------------------------

StatusOr<PacingEnvelope> PacingFabric::derive_envelope(FlowId flow) {
  OperationGuard guard(this);
  Status clock_status = require_trustworthy_clock();
  if (!clock_status) return clock_status;
  const Instant now = sample_clock();
  auto inputs = fetch_authority(flow);
  if (!inputs) {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.envelopes_refused += 1;
    if (is_staleness(inputs.code())) stats_.stale_rejections += 1;
    return inputs.status();
  }

  FabricEvent event{};
  std::unique_lock<std::mutex> lock(mu_);
  if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
  auto derived = derive_envelope_internal(inputs.value(), now);
  if (!derived) return derived.status();

  event.kind = EventKind::EnvelopeDerived;
  event.at = now;
  event.flow = flow;
  event.envelope = derived.value().ref;
  event.authority_digest = derived.value().authority.digest();
  event.detail = derived.value().to_string();
  lock.unlock();
  emit(std::move(event));
  return derived;
}

StatusOr<std::vector<u8>> PacingFabric::encode_attempt_payload(const ApplicationRecord& record) {
  ByteWriter w(kEncodeCapacityBytes);
  encode_attempt(w, record);
  if (!w.ok()) return w.status();
  return w.take();
}

StatusOr<std::vector<u8>> PacingFabric::encode_envelope_payload(const PacingEnvelope& envelope) {
  ByteWriter w(kEncodeCapacityBytes);
  encode_envelope(w, envelope);
  if (!w.ok()) return w.status();
  return w.take();
}

StatusOr<PacingEnvelope> PacingFabric::derive_envelope_internal(const AuthorityInputs& inputs,
                                                                Instant now) {
  const Limits& limits = config_.limits;

  DerivationInput di{};
  di.requested_rate_bps = inputs.policy.rate_bps;
  di.rate_share_ppm = inputs.policy.rate_share_ppm;
  di.min_rate_bps = inputs.policy.min_rate_bps;
  di.ceiling_bps = inputs.grant.ceiling_bps;
  di.floor_bps = inputs.grant.floor_bps;
  di.quantum_bytes = inputs.policy.quantum_bytes;
  di.window_ns = inputs.policy.window_ns;
  di.burst_bytes = inputs.policy.burst_bytes;
  di.burst_packets = inputs.policy.burst_packets;
  di.flow_max_burst_bytes = inputs.binding.declared_max_burst_bytes;
  di.flow_max_burst_packets = inputs.binding.declared_max_burst_packets;
  di.shape = inputs.policy.shape;
  di.grants_per_window_hint = inputs.policy.grants_per_window_hint;

  if (const ServiceClassException* ex = find_exception(inputs.policy, inputs.binding.service_class);
      ex != nullptr) {
    if (ex->min_rate_bps > di.min_rate_bps) di.min_rate_bps = ex->min_rate_bps;
    if (ex->rate_share_ppm != 0) di.rate_share_ppm = ex->rate_share_ppm;
    if (ex->burst_bytes > di.burst_bytes) di.burst_bytes = ex->burst_bytes;
    if (ex->burst_packets > di.burst_packets) di.burst_packets = ex->burst_packets;
  }

  auto outcome = derive_cadence(di, limits);
  if (!outcome) {
    stats_.envelopes_refused += 1;
    return outcome.status();
  }

  u64 lifetime = config_.envelope_lifetime_ns;
  if (lifetime > limits.max_envelope_lifetime_ns) lifetime = limits.max_envelope_lifetime_ns;
  u64 expiry_ns = 0;
  if (!checked::add(now.ns(), lifetime, expiry_ns)) {
    return Status::of(ErrorCode::ArithmeticOverflow, "envelope expiry computation overflowed");
  }
  if (inputs.grant.valid_until.is_set() &&
      inputs.grant.valid_until.instant().ns() < expiry_ns) {
    expiry_ns = inputs.grant.valid_until.instant().ns();
  }
  if (expiry_ns <= now.ns()) {
    return Status::of(ErrorCode::StaleRateGrant,
                      "upstream rate grant expires before any envelope could be valid");
  }

  PacingEnvelope envelope{};
  envelope.ref.id = EnvelopeId::from(envelope_ids_.next());
  envelope.ref.generation = Generation::first();
  envelope.flow = inputs.binding.flow;
  envelope.resource = inputs.binding.resource.id;
  envelope.authority = inputs.vector;
  envelope.cadence = outcome.value().cadence;
  envelope.requested_rate_bps = outcome.value().requested_rate_bps;
  envelope.ceiling_bps = inputs.grant.ceiling_bps;
  envelope.floor_bps = inputs.grant.floor_bps;
  envelope.clamped_to_ceiling = outcome.value().clamped_to_ceiling;
  envelope.clamped_to_floor = outcome.value().clamped_to_floor;
  envelope.service_class = inputs.binding.service_class;
  envelope.derived_at = now;
  envelope.valid_until = Deadline::at(Instant::from_ns(expiry_ns));
  envelope.upstream_synthetic = rate_authority_->synthetic();
  envelope.policy_synthetic = !inputs.policy.provenance.valid();

  envelope.provenance.id = ProvenanceId::from(provenance_ids_.next());
  envelope.provenance.sequence = envelope.ref.id.value();
  envelope.provenance.fabric_instance = config_.fabric_instance;
  envelope.provenance.binding_digest = inputs.binding.digest();
  envelope.provenance.policy_digest = policy_digest(inputs.policy);
  envelope.provenance.grant_digest = inputs.grant.digest();
  envelope.provenance.cadence_digest = cadence_digest(envelope.cadence);

  auto payload = encode_envelope_payload(envelope);
  if (!payload) return payload.status();
  Status durable = journal_record(DurableRecordType::Envelope, payload.value(), now.ns());
  if (!durable) return durable;

  evict_and_compact_locked();
  if (envelopes_.size() >= limits.max_envelopes) {
    return Status::of(ErrorCode::ResourceExhausted, "envelope table is at capacity and nothing is evictable");
  }

  EnvelopeEntry entry{};
  entry.envelope = envelope;
  entry.revoked = false;
  entry.live = true;
  envelopes_.push_back(std::move(entry));
  const std::size_t index = envelopes_.size() - 1;
  envelope_index_[envelope.ref.id.value()] = index;
  latest_envelope_by_flow_[envelope.flow.value()] = index;
  stats_.envelopes_derived += 1;
  return envelope;
}

StatusOr<PacingEnvelope> PacingFabric::envelope(EnvelopeId id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const std::size_t idx = find_envelope(id);
  if (idx == kNoIndex) return Status::of(ErrorCode::NotFound, "envelope not found");
  return envelopes_[idx].envelope;
}

std::vector<PacingEnvelope> PacingFabric::envelopes_for_flow(FlowId flow) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<PacingEnvelope> out;
  for (const auto& entry : envelopes_) {
    if (entry.live && entry.envelope.flow == flow) out.push_back(entry.envelope);
  }
  return out;
}

std::vector<ApplicationRecord> PacingFabric::attempts_for_flow(FlowId flow) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<ApplicationRecord> out;
  for (const auto& entry : attempts_) {
    if (entry.live && entry.record.flow == flow) out.push_back(entry.record);
  }
  return out;
}

// ---------------------------------------------------------------------------
// application
// ---------------------------------------------------------------------------

StatusOr<AttemptId> PacingFabric::apply_envelope(EnvelopeRef envelope_ref, BackendId backend_id,
                                                 AttemptId requested) {
  OperationGuard guard(this);
  Status clock_status = require_trustworthy_clock();
  if (!clock_status) return clock_status;
  const Instant now = sample_clock();
  if (!envelope_ref.id.valid() || !envelope_ref.generation.known()) {
    return Status::of(ErrorCode::InvalidArgument, "envelope reference is not generation-bound");
  }
  if (!backend_id.valid()) return Status::of(ErrorCode::InvalidArgument, "backend id is zero");

  // --- phase A: resolve envelope and backend -----------------------------
  PacingEnvelope envelope{};
  BackendPtr backend;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
    const std::size_t idx = find_envelope(envelope_ref.id);
    if (idx == kNoIndex) return Status::of(ErrorCode::NotFound, "envelope not found");
    const EnvelopeEntry& entry = envelopes_[idx];
    if (entry.envelope.ref.generation != envelope_ref.generation) {
      stats_.stale_rejections += 1;
      return Status::of(ErrorCode::StaleGeneration, "envelope generation does not match the ledger");
    }
    if (entry.revoked) {
      stats_.applies_refused += 1;
      return Status::of(ErrorCode::Revoked, "envelope has been revoked");
    }
    if (entry.envelope.expired_at(now)) {
      stats_.applies_refused += 1;
      return Status::of(ErrorCode::StaleGeneration, "envelope lifetime has expired");
    }
    envelope = entry.envelope;
    backend = backend_ptr(backend_id);
    if (!backend) return Status::of(ErrorCode::NotFound, "backend is not registered");
    stats_.applies_requested += 1;
  }

  // --- phase B: re-derive authority with no lock held --------------------
  auto current = fetch_authority(envelope.flow);
  if (!current) {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.applies_refused += 1;
    if (is_staleness(current.code())) stats_.stale_rejections += 1;
    return current.status();
  }
  const AuthorityDrift drift = compare_authority(envelope.authority, current.value().vector);
  if (drift != AuthorityDrift::None) {
    FabricEvent event{};
    {
      std::lock_guard<std::mutex> lock(mu_);
      stats_.applies_refused += 1;
      stats_.stale_rejections += 1;
      event.kind = EventKind::ApplyRefused;
      event.at = now;
      event.flow = envelope.flow;
      event.envelope = envelope_ref;
      event.reason = drift_code(drift);
      event.detail = std::string("authority drift: ") + std::string(to_string(drift));
    }
    emit(std::move(event));
    return Status::of(drift_code(drift), "authority drifted since the envelope was derived");
  }

  // --- phase C: reserve durably ------------------------------------------
  AttemptId attempt_id{};
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
    const std::size_t idx = find_envelope(envelope_ref.id);
    if (idx == kNoIndex) return Status::of(ErrorCode::NotFound, "envelope not found");
    if (envelopes_[idx].revoked) {
      stats_.applies_refused += 1;
      return Status::of(ErrorCode::Revoked, "envelope has been revoked");
    }

    const std::size_t existing = find_existing_attempt(envelope_ref, backend_id);
    if (existing != kNoIndex) {
      stats_.duplicate_applies += 1;
      return attempts_[existing].record.attempt;
    }
    if (requested.valid()) {
      const std::size_t known = find_attempt(requested);
      if (known != kNoIndex) {
        const ApplicationRecord& prior = attempts_[known].record;
        if (!(prior.envelope == envelope_ref) || !(prior.backend.id == backend_id)) {
          return Status::of(ErrorCode::Duplicate,
                            "attempt id is already bound to different work");
        }
        stats_.duplicate_applies += 1;
        return prior.attempt;
      }
    }
    if (pending_attempts_ >= config_.max_pending_attempts) {
      stats_.applies_refused += 1;
      return Status::of(ErrorCode::ResourceExhausted, "pending attempt bound reached");
    }
    evict_and_compact_locked();
    if (attempts_.size() >= config_.limits.max_attempts) {
      stats_.applies_refused += 1;
      return Status::of(ErrorCode::ResourceExhausted, "attempt ledger is at capacity");
    }

    attempt_id = requested.valid() ? requested : AttemptId::from(attempt_ids_.next());
    if (find_attempt(attempt_id) != kNoIndex) {
      return Status::of(ErrorCode::AlreadyExists, "attempt id already exists");
    }
    attempt_ids_.set_floor(attempt_id.value());

    const BackendDescriptor desc = backend->describe();
    ApplicationRecord record{};
    record.attempt = attempt_id;
    record.envelope = envelope_ref;
    record.flow = envelope.flow;
    record.resource = envelope.resource;
    record.authority = envelope.authority;
    record.desired = envelope.cadence;
    record.state = ApplicationState::Reserved;
    record.last_known_state = ApplicationState::Unknown;
    record.effect = EffectLabel::None;
    record.evidence_kind = EvidenceKind::None;
    record.backend = desc.ref;
    record.backend_synthetic = (desc.nature == BackendNature::Synthetic);
    record.worker = WorkerBoot{config_.fabric_instance, boot_.nonce};
    record.reason = ErrorCode::Ok;
    record.drift = AuthorityDrift::None;
    record.revision = 1;
    record.created_ns = now.ns();
    record.updated_ns = now.ns();

    auto payload = encode_attempt_payload(record);
    if (!payload) return payload.status();
    // Durable before authoritative: the reservation is not visible to any
    // reader until the journal append has reached stable storage.
    Status durable = journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
    if (!durable) {
      stats_.applies_refused += 1;
      return durable;
    }

    AttemptEntry entry{};
    entry.record = record;
    entry.live = true;
    attempts_.push_back(std::move(entry));
    attempt_index_[attempt_id.value()] = attempts_.size() - 1;
    attempts_by_envelope_[envelope_ref.id.value()].push_back(attempt_id);
    pending_attempts_ += 1;
    stats_.applies_reserved += 1;
    // In-memory only: Submitted means "handed to the backend in this process".
    // A crash in this window is recovered as Ambiguous, which is exactly the
    // truth about an attempt whose backend outcome was never observed.
    attempts_.back().record.state = ApplicationState::Submitted;
  }

  FabricEvent reserved{};
  {
    std::lock_guard<std::mutex> lock(mu_);
    reserved.kind = EventKind::ApplyReserved;
    reserved.at = now;
    reserved.flow = envelope.flow;
    reserved.envelope = envelope_ref;
    reserved.attempt = attempt_id;
    reserved.state = ApplicationState::Submitted;
    reserved.authority_digest = envelope.authority.digest();
  }
  emit(std::move(reserved));

  ApplyRequest request{};
  request.attempt = attempt_id;
  request.envelope = envelope_ref;
  request.flow = envelope.flow;
  request.authority = envelope.authority;
  request.cadence = envelope.cadence;
  request.worker = WorkerBoot{config_.fabric_instance, boot_.nonce};
  request.issued_at_ns = now.ns();
  request.deadline_ns = envelope.valid_until.is_set() ? envelope.valid_until.instant().ns() : 0;

  // --- phase D: backend apply with no lock held --------------------------
  ApplyAck ack{};
  const Status apply_status = backend->apply(request, ack);

  bool closed_early = false;
  bool acknowledged = false;
  ErrorCode closed_code = ErrorCode::Cancelled;
  std::string closed_detail;
  {
    std::unique_lock<std::mutex> lock(mu_);
    const std::size_t idx = find_attempt(attempt_id);
    if (idx == kNoIndex) return Status::of(ErrorCode::Internal, "attempt vanished during apply");
    AttemptEntry& entry = attempts_[idx];
    if (entry.record.state != ApplicationState::Submitted) {
      // The attempt was cancelled, revoked or fenced while the backend was
      // working. The completion must not commit.
      closed_early = true;
      closed_code = entry.record.reason == ErrorCode::Ok ? ErrorCode::Cancelled : entry.record.reason;
      closed_detail = entry.record.detail;
      stats_.late_completions_discarded += 1;
    } else if (find_backend(entry.record.backend.id) == kNoIndex) {
      // The coordinator no longer governs this backend, so an effect installed
      // on it could never be revoked, fenced, or rebound. The completion is
      // discarded and the handle already held is used to withdraw it.
      apply_transition(entry.record, ApplicationState::Failed, now, ErrorCode::BackendUnavailable,
                       "backend was unregistered while the attempt was in flight");
      stats_.applies_refused += 1;
      auto payload = encode_attempt_payload(entry.record);
      if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
      closed_early = true;
      closed_code = ErrorCode::BackendUnavailable;
      closed_detail = entry.record.detail;
    } else if (!apply_status) {
      apply_transition(entry.record, ApplicationState::Failed, now, apply_status.code(),
                       std::string(apply_status.detail()));
      stats_.applies_refused += 1;
      auto payload = encode_attempt_payload(entry.record);
      if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
    } else if (!ack.accepted) {
      apply_transition(entry.record, ApplicationState::Failed, now, ErrorCode::BackendRejected,
                       ack.detail);
      stats_.applies_refused += 1;
      auto payload = encode_attempt_payload(entry.record);
      if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
    } else if (ack.attempt != attempt_id) {
      apply_transition(entry.record, ApplicationState::Failed, now, ErrorCode::BackendFailure,
                       "backend acknowledged a different attempt identity");
      stats_.applies_refused += 1;
      auto payload = encode_attempt_payload(entry.record);
      if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
    } else {
      entry.record.backend_seq = ack.backend_seq;
      // An acknowledgement is explicitly NOT an effect: the record stops at
      // Acknowledged until a readback proves what is actually in force.
      apply_transition(entry.record, ApplicationState::Acknowledged, now, ErrorCode::Ok, ack.detail);
      acknowledged = true;
      auto payload = encode_attempt_payload(entry.record);
      if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
    }
  }

  if (closed_early) {
    // Compensating withdrawal with no lock held.
    (void)backend->revoke(request, "late backend completion discarded");
    FabricEvent event{};
    event.kind = EventKind::ApplyRefused;
    event.at = now;
    event.flow = envelope.flow;
    event.envelope = envelope_ref;
    event.attempt = attempt_id;
    event.reason = closed_code;
    event.detail = closed_detail.empty() ? "attempt closed before backend completion" : closed_detail;
    emit(std::move(event));
    return Status::of(closed_code, "attempt was closed before the backend completed; completion discarded");
  }

  if (!acknowledged) {
    // The record already reached a terminal state in phase D (the backend
    // failed, rejected the request, or answered for a different attempt). A
    // readback would ask the backend about pacing it never installed, so the
    // attempt is reported as its recorded outcome instead.
    FabricEvent event{};
    event.kind = EventKind::ApplyRefused;
    event.at = now;
    event.flow = envelope.flow;
    event.envelope = envelope_ref;
    event.attempt = attempt_id;
    {
      std::lock_guard<std::mutex> lock(mu_);
      const std::size_t idx = find_attempt(attempt_id);
      if (idx != kNoIndex) {
        event.reason = attempts_[idx].record.reason;
        event.state = attempts_[idx].record.state;
        event.detail = attempts_[idx].record.detail;
      }
    }
    emit(std::move(event));
    return attempt_id;
  }

  // --- phase E: readback and commit --------------------------------------
  const BackendDescriptor desc = backend->describe();
  if (!desc.supports_readback) {
    // Authority is re-checked before the record is reported at all, even though
    // no effect is claimed: a record must never describe authority that has
    // already moved.
    auto unverified_authority = fetch_authority(envelope.flow);
    const AuthorityDrift unverified_drift =
        unverified_authority
            ? compare_authority(envelope.authority, unverified_authority.value().vector)
            : AuthorityDrift::Incomplete;
    const ErrorCode unverified_drift_code =
        unverified_authority ? drift_code(unverified_drift) : unverified_authority.code();

    FabricEvent event{};
    bool withdrew = false;
    {
      std::unique_lock<std::mutex> lock(mu_);
      const std::size_t idx = find_attempt(attempt_id);
      if (idx == kNoIndex) return Status::of(ErrorCode::Internal, "attempt vanished during readback");
      AttemptEntry& entry = attempts_[idx];
      if (entry.record.state != ApplicationState::Acknowledged) {
        stats_.late_completions_discarded += 1;
      } else if (unverified_drift != AuthorityDrift::None) {
        apply_transition(entry.record, ApplicationState::Stale, now, unverified_drift_code,
                         "authority drifted while the attempt was being applied");
        stats_.stale_rejections += 1;
        auto payload = encode_attempt_payload(entry.record);
        if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
        event.kind = EventKind::AttemptStale;
        event.at = now;
        event.flow = envelope.flow;
        event.envelope = envelope_ref;
        event.attempt = attempt_id;
        event.state = entry.record.state;
        event.reason = unverified_drift_code;
        event.detail = entry.record.detail;
        withdrew = true;
      } else {
        apply_transition(entry.record, ApplicationState::AppliedUnverified, now,
                         ErrorCode::ReadbackUnavailable,
                         "backend does not support readback; effect is UNKNOWN");
        entry.record.evidence_kind = EvidenceKind::None;
        stats_.applies_unverified += 1;
        stats_.readback_unavailable += 1;
        auto payload = encode_attempt_payload(entry.record);
        if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
        event.kind = EventKind::ApplyUnverified;
        event.at = now;
        event.flow = envelope.flow;
        event.envelope = envelope_ref;
        event.attempt = attempt_id;
        event.state = entry.record.state;
        event.effect = entry.record.effect;
        event.reason = ErrorCode::ReadbackUnavailable;
      }
    }
    if (withdrew) {
      (void)backend->revoke(request, "authority drifted during apply");
    }
    if (event.kind == EventKind::ApplyUnverified || event.kind == EventKind::AttemptStale) {
      emit(std::move(event));
    }
    return attempt_id;
  }

  ReadbackEvidence evidence{};
  const Status readback_status = backend->readback(request, evidence);

  // Authority is re-checked immediately before an effect is asserted. A rate
  // grant, policy, binding or path that moved while the backend was working
  // means the envelope under which this attempt was authorized no longer
  // exists, so the completion must not commit an effect.
  auto current_after = fetch_authority(envelope.flow);
  const AuthorityDrift drift_after =
      current_after ? compare_authority(envelope.authority, current_after.value().vector)
                    : AuthorityDrift::Incomplete;
  const ErrorCode drift_after_code = current_after ? drift_code(drift_after) : current_after.code();

  FabricEvent final_event{};
  bool have_final = false;
  bool unsafe_effect = false;
  bool stale_effect = false;
  {
    std::unique_lock<std::mutex> lock(mu_);
    const std::size_t idx = find_attempt(attempt_id);
    if (idx == kNoIndex) return Status::of(ErrorCode::Internal, "attempt vanished during readback");
    AttemptEntry& entry = attempts_[idx];
    if (entry.record.state != ApplicationState::Acknowledged) {
      stats_.late_completions_discarded += 1;
      lock.unlock();
      (void)backend->revoke(request, "late readback discarded");
      return Status::of(ErrorCode::Cancelled, "attempt was closed before readback completed");
    }
    if (find_backend(entry.record.backend.id) == kNoIndex) {
      apply_transition(entry.record, ApplicationState::Stale, now, ErrorCode::BackendUnavailable,
                       "backend was unregistered while the attempt was in flight");
      stats_.stale_rejections += 1;
      auto payload = encode_attempt_payload(entry.record);
      if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
      final_event.kind = EventKind::AttemptStale;
      final_event.at = now;
      final_event.flow = envelope.flow;
      final_event.envelope = envelope_ref;
      final_event.attempt = attempt_id;
      final_event.state = entry.record.state;
      final_event.reason = ErrorCode::BackendUnavailable;
      final_event.detail = entry.record.detail;
      have_final = true;
      stale_effect = true;
      lock.unlock();
      (void)backend->revoke(request, "backend unregistered during apply");
      emit(std::move(final_event));
      return Status::of(ErrorCode::BackendUnavailable,
                        "backend was unregistered while the attempt was in flight");
    }
    if (drift_after != AuthorityDrift::None) {
      apply_transition(entry.record, ApplicationState::Stale, now, drift_after_code,
                       "authority drifted while the attempt was being applied");
      stats_.stale_rejections += 1;
      auto payload = encode_attempt_payload(entry.record);
      if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
      final_event.kind = EventKind::AttemptStale;
      final_event.at = now;
      final_event.flow = envelope.flow;
      final_event.envelope = envelope_ref;
      final_event.attempt = attempt_id;
      final_event.state = entry.record.state;
      final_event.effect = entry.record.effect;
      final_event.reason = drift_after_code;
      final_event.detail = entry.record.detail;
      have_final = true;
      stale_effect = true;
      lock.unlock();
      (void)backend->revoke(request, "authority drifted during apply");
      emit(std::move(final_event));
      return Status::of(drift_after_code,
                        "authority drifted during apply; the completion was not committed");
    }
    const bool readback_ok = readback_status.ok() && evidence.present && evidence.integrity_ok;
    const ErrorCode readback_code =
        !readback_status.ok() ? readback_status.code()
                              : (evidence.present ? ErrorCode::IntegrityFailure
                                                  : ErrorCode::ReadbackUnavailable);
    Status finalized = finalize_evidence_locked(entry, envelope, evidence, readback_ok, readback_code, now);
    auto payload = encode_attempt_payload(entry.record);
    if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());

    final_event.kind = entry.record.state == ApplicationState::Applied
                           ? EventKind::ApplyVerified
                           : (entry.record.state == ApplicationState::Mismatched ? EventKind::ApplyMismatched
                                                                                : EventKind::ApplyUnverified);
    final_event.at = now;
    final_event.flow = envelope.flow;
    final_event.envelope = envelope_ref;
    final_event.attempt = attempt_id;
    final_event.state = entry.record.state;
    final_event.effect = entry.record.effect;
    final_event.authority_digest = envelope.authority.digest();
    final_event.reason = entry.record.reason;
    final_event.detail = entry.record.detail;
    have_final = true;
    unsafe_effect = (entry.record.state == ApplicationState::Mismatched &&
                     evidence.observed_rate_bps > envelope.cadence.rate_bps);
    if (!finalized.ok()) {
      // The transition itself failed validation: report it rather than
      // pretending the record reached a state it did not.
      lock.unlock();
      if (have_final) emit(std::move(final_event));
      return finalized;
    }
  }

  if (unsafe_effect) {
    // The backend is pacing above the authorized ceiling. Withdraw it rather
    // than leaving an over-rate effect in place.
    (void)backend->revoke(request, "readback exceeded the authorized ceiling");
  }
  if (have_final) emit(std::move(final_event));
  return attempt_id;
}

Status PacingFabric::finalize_evidence_locked(AttemptEntry& entry, const PacingEnvelope& envelope,
                                              const ReadbackEvidence& evidence, bool readback_ok,
                                              ErrorCode readback_code, Instant now) {
  ApplicationRecord& record = entry.record;
  record.evidence_kind = evidence.kind;
  record.observed_rate_bps = evidence.observed_rate_bps;
  record.observed_quantum_bytes = evidence.observed_quantum_bytes;
  record.observed_burst_bytes = evidence.observed_burst_bytes;
  record.observed_interval_ns = evidence.observed_interval_ns;
  record.backend_seq = evidence.backend_seq;
  record.evidence_integrity_ok = evidence.integrity_ok;
  record.evidence_digest = evidence.digest != 0 ? evidence.digest : evidence_digest(evidence);

  if (!readback_ok) {
    apply_transition(record, ApplicationState::AppliedUnverified, now, readback_code,
                     readback_code == ErrorCode::IntegrityFailure
                         ? "readback evidence failed its integrity check; effect is UNKNOWN"
                         : "no readback evidence available; effect is UNKNOWN");
    stats_.applies_unverified += 1;
    stats_.readback_unavailable += 1;
    return Status::success();
  }

  // The fabric authorised one exact cadence. Evidence counts as a match only
  // when it reproduces that cadence exactly: a backend pacing below authority
  // is safe but is still not what was authorised, and reporting it as a match
  // would hide a real divergence between intent and effect.
  const bool identity_ok = evidence.attempt == record.attempt && evidence.backend.id == record.backend.id;
  const bool quantum_ok = evidence.observed_quantum_bytes == envelope.cadence.quantum_bytes;
  const bool burst_ok = evidence.observed_burst_bytes == envelope.cadence.burst_bytes;
  const bool rate_ok = evidence.observed_rate_bps == envelope.cadence.rate_bps;
  const bool interval_ok = evidence.observed_interval_ns == envelope.cadence.interval_ns;

  if (identity_ok && quantum_ok && burst_ok && rate_ok && interval_ok) {
    apply_transition(record, ApplicationState::Applied, now, ErrorCode::Ok,
                     record.backend_synthetic
                         ? "synthetic backend readback matches the authorized envelope"
                         : "backend readback matches the authorized envelope");
    stats_.applies_committed += 1;
    return Status::success();
  }

  if (evidence.observed_rate_bps > envelope.cadence.rate_bps) {
    apply_transition(record, ApplicationState::Mismatched, now, ErrorCode::CeilingExceeded,
                     "readback rate exceeds the authorized envelope rate");
  } else {
    apply_transition(record, ApplicationState::Mismatched, now, ErrorCode::ReadbackMismatch,
                     "readback does not match the authorized envelope");
  }
  stats_.applies_mismatched += 1;
  return Status::success();
}

Status PacingFabric::verify_attempt(AttemptId id) {
  OperationGuard guard(this);
  Status clock_status = require_trustworthy_clock();
  if (!clock_status) return clock_status;
  const Instant now = sample_clock();

  ApplicationRecord record{};
  PacingEnvelope envelope{};
  BackendPtr backend;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
    const std::size_t idx = find_attempt(id);
    if (idx == kNoIndex) return Status::of(ErrorCode::NotFound, "attempt not found");
    record = attempts_[idx].record;
    if (record.state == ApplicationState::Cancelled) {
      return Status::of(ErrorCode::Cancelled, "attempt was cancelled");
    }
    if (record.state == ApplicationState::Revoked) {
      return Status::of(ErrorCode::Revoked, "attempt was revoked");
    }
    if (record.state == ApplicationState::Fenced) {
      return Status::of(ErrorCode::Fenced, "attempt was fenced");
    }
    if (record.state == ApplicationState::Stale) {
      return Status::of(ErrorCode::StaleGeneration, "attempt was invalidated by authority drift");
    }
    const std::size_t eidx = find_envelope(record.envelope.id);
    if (eidx == kNoIndex) return Status::of(ErrorCode::NotFound, "envelope not found");
    envelope = envelopes_[eidx].envelope;
    backend = backend_ptr(record.backend.id);
    if (!backend) return Status::of(ErrorCode::BackendUnavailable, "backend is not registered");
  }

  auto current = fetch_authority(record.flow);
  if (!current) {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.stale_rejections += 1;
    return current.status();
  }
  const AuthorityDrift drift = compare_authority(record.authority, current.value().vector);
  if (drift != AuthorityDrift::None) {
    FabricEvent event{};
    {
      std::unique_lock<std::mutex> lock(mu_);
      const std::size_t idx = find_attempt(id);
      if (idx != kNoIndex) {
        apply_transition(attempts_[idx].record, ApplicationState::Stale, now, drift_code(drift),
                         std::string("authority drift: ") + std::string(to_string(drift)));
        auto payload = encode_attempt_payload(attempts_[idx].record);
        if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
      }
      stats_.stale_rejections += 1;
      event.kind = EventKind::AttemptStale;
      event.at = now;
      event.attempt = id;
      event.flow = record.flow;
      event.envelope = record.envelope;
      event.reason = drift_code(drift);
    }
    emit(std::move(event));
    return Status::of(drift_code(drift), "authority drifted; attempt is stale");
  }

  ApplyRequest request{};
  request.attempt = id;
  request.envelope = record.envelope;
  request.flow = record.flow;
  request.authority = record.authority;
  request.cadence = envelope.cadence;
  request.worker = record.worker;
  request.issued_at_ns = now.ns();
  request.deadline_ns = envelope.valid_until.is_set() ? envelope.valid_until.instant().ns() : 0;

  ReadbackEvidence evidence{};
  const Status readback_status = backend->readback(request, evidence);
  const bool readback_ok = readback_status.ok() && evidence.present && evidence.integrity_ok;
  const ErrorCode readback_code =
      !readback_status.ok() ? readback_status.code()
                            : (evidence.present ? ErrorCode::IntegrityFailure
                                                : ErrorCode::ReadbackUnavailable);

  FabricEvent event{};
  Status result = Status::success();
  {
    std::unique_lock<std::mutex> lock(mu_);
    const std::size_t idx = find_attempt(id);
    if (idx == kNoIndex) return Status::of(ErrorCode::NotFound, "attempt not found");
    result = finalize_evidence_locked(attempts_[idx], envelope, evidence, readback_ok, readback_code, now);
    auto payload = encode_attempt_payload(attempts_[idx].record);
    if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
    event.kind = attempts_[idx].record.state == ApplicationState::Applied ? EventKind::ApplyVerified
                                                                          : EventKind::ApplyUnverified;
    event.at = now;
    event.attempt = id;
    event.flow = record.flow;
    event.envelope = record.envelope;
    event.state = attempts_[idx].record.state;
    event.effect = attempts_[idx].record.effect;
    event.reason = attempts_[idx].record.reason;
  }
  emit(std::move(event));
  return result;
}

Status PacingFabric::cancel_attempt(AttemptId id, std::string_view reason) {
  OperationGuard guard(this);
  const Instant now = sample_clock();
  if (reason.size() > config_.limits.max_explanation_bytes) {
    return Status::of(ErrorCode::Oversized, "cancellation reason above configured bound");
  }

  ApplyRequest request{};
  BackendPtr backend;
  bool withdraw_backend = false;
  FabricEvent event{};
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
    const std::size_t idx = find_attempt(id);
    if (idx == kNoIndex) return Status::of(ErrorCode::NotFound, "attempt not found");
    AttemptEntry& entry = attempts_[idx];
    const std::string detail(reason);

    switch (entry.record.state) {
      case ApplicationState::Cancelled:
      case ApplicationState::Revoked:
      case ApplicationState::Fenced:
      case ApplicationState::Stale:
      case ApplicationState::Failed:
      case ApplicationState::Ambiguous:
        // Already terminal without a lasting effect: cancellation is a no-op.
        event.kind = EventKind::AttemptCancelled;
        event.at = now;
        event.attempt = id;
        event.state = entry.record.state;
        lock.unlock();
        emit(std::move(event));
        return Status::success();
      case ApplicationState::Applied:
      case ApplicationState::AppliedUnverified:
      case ApplicationState::Mismatched:
      case ApplicationState::RequiresRevalidation:
      case ApplicationState::Planned:
      case ApplicationState::Reserved:
      case ApplicationState::Submitted:
      case ApplicationState::Acknowledged:
        // A compensating withdrawal is issued in every case, including for an
        // attempt the backend may not have installed yet: the backend's revoke
        // is required to be idempotent, and a withdrawal that is not needed is
        // harmless whereas a missing one leaves unauthorized pacing in force.
        withdraw_backend = true;
        break;
      default:
        break;
    }

    const std::size_t eidx = find_envelope(entry.record.envelope.id);
    if (eidx != kNoIndex) {
      request.cadence = envelopes_[eidx].envelope.cadence;
      request.envelope = entry.record.envelope;
    } else {
      request.cadence = entry.record.desired;
      request.envelope = entry.record.envelope;
    }
    request.attempt = id;
    request.flow = entry.record.flow;
    request.authority = entry.record.authority;
    request.worker = entry.record.worker;
    request.issued_at_ns = now.ns();
    backend = backend_ptr(entry.record.backend.id);

    if (entry.record.state == ApplicationState::Applied ||
        entry.record.state == ApplicationState::AppliedUnverified ||
        entry.record.state == ApplicationState::Mismatched ||
        entry.record.state == ApplicationState::RequiresRevalidation) {
      apply_transition(entry.record, ApplicationState::Revoked, now, ErrorCode::Revoked,
                       detail.empty() ? "revoked by operator" : detail);
      stats_.revocations += 1;
    } else {
      apply_transition(entry.record, ApplicationState::Cancelled, now, ErrorCode::Cancelled,
                       detail.empty() ? "cancelled by operator" : detail);
      stats_.cancellations += 1;
    }
    auto payload = encode_attempt_payload(entry.record);
    if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());

    event.kind = EventKind::AttemptCancelled;
    event.at = now;
    event.attempt = id;
    event.flow = entry.record.flow;
    event.envelope = entry.record.envelope;
    event.state = entry.record.state;
    event.reason = entry.record.reason;
    event.detail = entry.record.detail;
  }

  if (withdraw_backend && backend) {
    (void)backend->revoke(request, reason);
  }
  emit(std::move(event));
  return Status::success();
}

StatusOr<ApplicationRecord> PacingFabric::attempt(AttemptId id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const std::size_t idx = find_attempt(id);
  if (idx == kNoIndex) return Status::of(ErrorCode::NotFound, "attempt not found");
  return attempts_[idx].record;
}

// ---------------------------------------------------------------------------
// governance
// ---------------------------------------------------------------------------

Status PacingFabric::revoke_envelope(EnvelopeRef envelope_ref, std::string_view reason) {
  OperationGuard guard(this);
  const Instant now = sample_clock();
  if (!envelope_ref.id.valid()) return Status::of(ErrorCode::InvalidArgument, "envelope id is zero");
  if (reason.size() > config_.limits.max_explanation_bytes) {
    return Status::of(ErrorCode::Oversized, "revocation reason above configured bound");
  }

  std::vector<std::pair<BackendPtr, ApplyRequest>> withdrawals;
  FabricEvent event{};
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
    const std::size_t idx = find_envelope(envelope_ref.id);
    if (idx == kNoIndex) return Status::of(ErrorCode::NotFound, "envelope not found");
    EnvelopeEntry& entry = envelopes_[idx];
    if (entry.envelope.ref.generation != envelope_ref.generation) {
      stats_.stale_rejections += 1;
      return Status::of(ErrorCode::StaleGeneration, "envelope generation does not match");
    }
    if (!entry.revoked) {
      entry.revoked = true;
      entry.revoke_reason.assign(reason);
      std::vector<u8> payload;
      {
        ByteWriter w(512);
        encode_revocation(w, envelope_ref, reason);
        if (!w.ok()) return w.status();
        payload = w.take();
      }
      Status durable = journal_record(DurableRecordType::EnvelopeRevocation, std::move(payload), now.ns());
      if (!durable) {
        entry.revoked = false;
        entry.revoke_reason.clear();
        return durable;
      }
    }

    const auto found = attempts_by_envelope_.find(envelope_ref.id.value());
    if (found != attempts_by_envelope_.end()) {
      for (const AttemptId attempt_id : found->second) {
        const std::size_t aidx = find_attempt(attempt_id);
        if (aidx == kNoIndex) continue;
        AttemptEntry& aentry = attempts_[aidx];
        switch (aentry.record.state) {
          case ApplicationState::Cancelled:
          case ApplicationState::Revoked:
          case ApplicationState::Fenced:
          case ApplicationState::Stale:
          case ApplicationState::Failed:
          case ApplicationState::Ambiguous:
            continue;
          default:
            break;
        }
        BackendPtr backend = backend_ptr(aentry.record.backend.id);
        if (backend) {
          ApplyRequest request{};
          request.attempt = aentry.record.attempt;
          request.envelope = aentry.record.envelope;
          request.flow = aentry.record.flow;
          request.authority = aentry.record.authority;
          request.cadence = entry.envelope.cadence;
          request.worker = aentry.record.worker;
          request.issued_at_ns = now.ns();
          withdrawals.emplace_back(backend, std::move(request));
        }
        apply_transition(aentry.record, ApplicationState::Revoked, now, ErrorCode::Revoked,
                         std::string(reason));
        stats_.revocations += 1;
        auto payload = encode_attempt_payload(aentry.record);
        if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
      }
    }

    event.kind = EventKind::EnvelopeRevoked;
    event.at = now;
    event.envelope = envelope_ref;
    event.flow = entry.envelope.flow;
    event.reason = ErrorCode::Revoked;
    event.detail = std::string(reason);
  }

  for (auto& withdrawal : withdrawals) {
    (void)withdrawal.first->revoke(withdrawal.second, reason);
  }
  emit(std::move(event));
  return Status::success();
}

Status PacingFabric::fence_below(Epoch epoch) {
  OperationGuard guard(this);
  const Instant now = sample_clock();
  if (!epoch.valid()) return Status::of(ErrorCode::InvalidArgument, "fence epoch is not valid");

  std::size_t fenced = 0;
  std::vector<std::pair<BackendPtr, ApplyRequest>> withdrawals;
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
    for (auto& entry : envelopes_) {
      if (!entry.live || entry.revoked) continue;
      if (entry.envelope.authority.epoch < epoch) {
        entry.revoked = true;
        entry.revoke_reason = "fenced by coordinator epoch";
        fenced += 1;
      }
    }
    for (auto& entry : attempts_) {
      if (!entry.live) continue;
      if (entry.record.authority.epoch >= epoch) continue;
      switch (entry.record.state) {
        case ApplicationState::Revoked:
        case ApplicationState::Fenced:
        case ApplicationState::Stale:
        case ApplicationState::Failed:
        case ApplicationState::Cancelled:
        case ApplicationState::Ambiguous:
          continue;
        default:
          break;
      }
      const bool claimed_effect = entry.record.claims_effect() ||
                                  entry.record.state == ApplicationState::AppliedUnverified ||
                                  entry.record.state == ApplicationState::Mismatched;
      if (claimed_effect) {
        BackendPtr backend = backend_ptr(entry.record.backend.id);
        if (backend) {
          ApplyRequest request{};
          request.attempt = entry.record.attempt;
          request.envelope = entry.record.envelope;
          request.flow = entry.record.flow;
          request.authority = entry.record.authority;
          request.cadence = entry.record.desired;
          request.worker = entry.record.worker;
          request.issued_at_ns = now.ns();
          withdrawals.emplace_back(backend, std::move(request));
        }
      }
      // A fenced record includes anything that was applied under the fenced
      // epoch: its authority is gone, so no effect may still be asserted and
      // the backend is told to withdraw it.
      apply_transition(entry.record, ApplicationState::Fenced, now, ErrorCode::Fenced,
                       "fenced by coordinator epoch");
      auto payload = encode_attempt_payload(entry.record);
      if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
      fenced += 1;
    }
    stats_.fences += fenced;
  }

  for (auto& withdrawal : withdrawals) {
    (void)withdrawal.first->revoke(withdrawal.second, "fenced by coordinator epoch");
  }

  FabricEvent event{};
  event.kind = EventKind::Fenced;
  event.at = now;
  event.detail = "fenced " + std::to_string(fenced) + " records below epoch " +
                 std::to_string(epoch.value());
  emit(std::move(event));
  return Status::success();
}

Status PacingFabric::revalidate(AttemptId id) {
  OperationGuard guard(this);
  Status clock_status = require_trustworthy_clock();
  if (!clock_status) return clock_status;
  const Instant now = sample_clock();

  ApplicationRecord record{};
  PacingEnvelope envelope{};
  BackendPtr backend;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!running_) return Status::of(ErrorCode::InvalidState, "fabric is not running");
    const std::size_t idx = find_attempt(id);
    if (idx == kNoIndex) return Status::of(ErrorCode::NotFound, "attempt not found");
    record = attempts_[idx].record;
    switch (record.state) {
      case ApplicationState::Cancelled:
      case ApplicationState::Revoked:
      case ApplicationState::Fenced:
      case ApplicationState::Failed:
        return Status::of(ErrorCode::InvalidState, "attempt is closed; it cannot be revalidated");
      default:
        break;
    }
    const std::size_t eidx = find_envelope(record.envelope.id);
    if (eidx == kNoIndex) {
      return Status::of(ErrorCode::RequiresRevalidation,
                        "governing envelope is no longer present; derive and re-apply");
    }
    envelope = envelopes_[eidx].envelope;
    backend = backend_ptr(record.backend.id);
  }

  auto current = fetch_authority(record.flow);
  if (!current) {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.stale_rejections += 1;
    return current.status();
  }
  const AuthorityDrift drift = compare_authority(record.authority, current.value().vector);
  if (drift != AuthorityDrift::None) {
    FabricEvent event{};
    {
      std::unique_lock<std::mutex> lock(mu_);
      const std::size_t idx = find_attempt(id);
      if (idx != kNoIndex) {
        apply_transition(attempts_[idx].record, ApplicationState::Stale, now, drift_code(drift),
                         std::string("authority drift: ") + std::string(to_string(drift)));
        attempts_[idx].record.requires_revalidation = true;
        auto payload = encode_attempt_payload(attempts_[idx].record);
        if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
      }
      stats_.stale_rejections += 1;
      event.kind = EventKind::AttemptStale;
      event.at = now;
      event.attempt = id;
      event.flow = record.flow;
      event.envelope = record.envelope;
      event.reason = drift_code(drift);
      event.detail = "revalidation rejected: authority drifted";
    }
    emit(std::move(event));
    return Status::of(drift_code(drift), "authority drifted; the recorded effect is not current");
  }

  if (!backend) {
    std::lock_guard<std::mutex> lock(mu_);
    return Status::of(ErrorCode::BackendUnavailable,
                      "backend is not registered; pacing must be re-applied against a live backend");
  }

  ApplyRequest request{};
  request.attempt = id;
  request.envelope = record.envelope;
  request.flow = record.flow;
  request.authority = record.authority;
  request.cadence = envelope.cadence;
  request.worker = record.worker;
  request.issued_at_ns = now.ns();
  request.deadline_ns = envelope.valid_until.is_set() ? envelope.valid_until.instant().ns() : 0;

  ReadbackEvidence evidence{};
  const Status readback_status = backend->readback(request, evidence);
  const bool readback_ok = readback_status.ok() && evidence.present && evidence.integrity_ok;

  if (!readback_ok) {
    {
      std::unique_lock<std::mutex> lock(mu_);
      const std::size_t idx = find_attempt(id);
      if (idx != kNoIndex) {
        attempts_[idx].record.requires_revalidation = true;
        if (!is_terminal(attempts_[idx].record.state)) {
          apply_transition(attempts_[idx].record, ApplicationState::RequiresRevalidation, now,
                           ErrorCode::RequiresRevalidation,
                           "revalidation could not obtain positive backend evidence");
        }
        auto payload = encode_attempt_payload(attempts_[idx].record);
        if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
      }
    }
    return Status::of(ErrorCode::RequiresRevalidation,
                      "no positive backend evidence; pacing must be re-applied");
  }

  FabricEvent event{};
  Status result = Status::success();
  {
    std::unique_lock<std::mutex> lock(mu_);
    const std::size_t idx = find_attempt(id);
    if (idx == kNoIndex) return Status::of(ErrorCode::NotFound, "attempt not found");
    AttemptEntry& entry = attempts_[idx];
    result = finalize_evidence_locked(entry, envelope, evidence, true, ErrorCode::Ok, now);
    entry.record.requires_revalidation = (entry.record.state != ApplicationState::Applied);
    stats_.revalidations += 1;
    auto payload = encode_attempt_payload(entry.record);
    if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());

    event.kind = entry.record.state == ApplicationState::Applied ? EventKind::AttemptRevalidated
                                                                 : EventKind::ApplyMismatched;
    event.at = now;
    event.attempt = id;
    event.flow = record.flow;
    event.envelope = record.envelope;
    event.state = entry.record.state;
    event.effect = entry.record.effect;
    event.reason = entry.record.reason;
    event.detail = entry.record.detail;
  }
  emit(std::move(event));
  if (!result) return result;
  // Revalidation reports RequiresRevalidation whenever the effect still cannot
  // be asserted, so a caller can never mistake "we looked" for "it is in force".
  {
    std::lock_guard<std::mutex> lock(mu_);
    const std::size_t idx = find_attempt(id);
    if (idx == kNoIndex) return Status::of(ErrorCode::NotFound, "attempt not found");
    if (attempts_[idx].record.state != ApplicationState::Applied) {
      return Status::of(ErrorCode::RequiresRevalidation, "readback did not confirm a matching effect");
    }
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// observation
// ---------------------------------------------------------------------------

StatusOr<Explanation> PacingFabric::explain(FlowId flow) const {
  if (!flow.valid()) return Status::of(ErrorCode::InvalidArgument, "flow id is zero");
  Explanation out{};
  out.flow = flow;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto found_binding = bindings_.get(flow);
    if (found_binding) {
      out.binding = found_binding.value();
      out.has_binding = true;
      out.flow_generation = out.binding.generation;
      out.resource = out.binding.resource.id;
    }
    const auto latest = latest_envelope_by_flow_.find(flow.value());
    if (latest != latest_envelope_by_flow_.end() && latest->second < envelopes_.size()) {
      const EnvelopeEntry& entry = envelopes_[latest->second];
      if (entry.live) {
        out.envelope = entry.envelope;
        out.has_envelope = true;
        out.flow_generation = entry.envelope.authority.flow.generation;
        out.resource = entry.envelope.resource;
        out.burst_budget_bytes = entry.envelope.cadence.burst_bytes;
        out.burst_budget_packets = entry.envelope.cadence.burst_packets;
        if (entry.revoked) out.stale_or_revoke_reason = entry.revoke_reason;
        const auto attempts = attempts_by_envelope_.find(entry.envelope.ref.id.value());
        if (attempts != attempts_by_envelope_.end()) {
          bool have = false;
          ApplicationRecord newest{};
          for (const AttemptId id : attempts->second) {
            const std::size_t idx = find_attempt(id);
            if (idx == kNoIndex) continue;
            const ApplicationRecord& record = attempts_[idx].record;
            if (!have || record.attempt.value() > newest.attempt.value()) {
              newest = record;
              have = true;
            }
          }
          if (have) {
            out.attempt = newest;
            out.has_attempt = true;
          }
        }
      }
    }
  }

  if (out.resource.valid()) {
    // External call with no lock held.
    auto grant = rate_authority_->fetch(out.resource);
    if (grant) {
      out.grant = grant.value();
      out.has_grant = true;
    }
  }

  auto current = fetch_authority(flow);
  if (current) {
    out.current_authority = current.value().vector;
    if (out.has_envelope) {
      out.drift = compare_authority(out.envelope.authority, out.current_authority);
    } else if (out.has_attempt) {
      out.drift = compare_authority(out.attempt.authority, out.current_authority);
    }
  } else {
    out.refusal = current.code();
    out.refusal_detail = std::string(current.status().detail());
  }

  if (out.drift != AuthorityDrift::None) {
    out.refusal = drift_code(out.drift);
    out.refusal_detail = std::string("authority drift: ") + std::string(to_string(out.drift));
    out.stale_or_revoke_reason = out.refusal_detail;
    out.effect = EffectLabel::None;
    return out;
  }

  if (out.has_attempt) {
    if (out.attempt.requires_revalidation) {
      // The recorded effect is history, not current fact.
      out.effect = EffectLabel::None;
      if (out.refusal == ErrorCode::Ok) {
        out.refusal = ErrorCode::RequiresRevalidation;
        out.refusal_detail = "recorded effect is not current until it is revalidated";
      }
    } else {
      out.effect = out.attempt.effect;
    }
    if (out.refusal == ErrorCode::Ok && out.attempt.reason != ErrorCode::Ok &&
        out.attempt.effect == EffectLabel::None) {
      out.refusal = out.attempt.reason;
      out.refusal_detail = out.attempt.detail;
    }
  } else if (out.has_envelope && out.refusal == ErrorCode::Ok) {
    out.effect = EffectLabel::None;
  }

  if (out.refusal == ErrorCode::Ok) out.refusal_detail.clear();
  return out;
}

FabricStats PacingFabric::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

void PacingFabric::set_event_sink(IEventSink* sink) noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  event_sink_ = sink;
}

void PacingFabric::emit(FabricEvent event) noexcept {
  IEventSink* sink = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.events_emitted += 1;
    sink = event_sink_;
  }
  if (sink != nullptr) {
    // The sink is invoked with no lock held. A sink that throws is a caller
    // defect and is contained here rather than being allowed to corrupt the
    // coordinator's state lock.
    try {
      sink->on_event(event);
    } catch (...) {
    }
  }
}

void PacingFabric::apply_transition(ApplicationRecord& record, ApplicationState next, Instant now,
                                    ErrorCode reason, std::string detail) {
  const bool was_pending = !is_terminal(record.state);
  record.last_known_state = record.state;
  record.state = next;
  record.reason = reason;
  record.detail = std::move(detail);
  if (record.revision != checked::kU64Max) record.revision += 1;
  record.updated_ns = now.ns();

  switch (next) {
    case ApplicationState::Applied:
      record.effect = record.backend_synthetic ? EffectLabel::SyntheticVerified : EffectLabel::Verified;
      record.requires_revalidation = false;
      break;
    case ApplicationState::AppliedUnverified:
      record.effect = EffectLabel::Unverified;
      record.requires_revalidation = false;
      break;
    case ApplicationState::Mismatched:
      record.effect = EffectLabel::Mismatched;
      record.requires_revalidation = false;
      break;
    case ApplicationState::RequiresRevalidation:
      record.effect = EffectLabel::None;
      record.requires_revalidation = true;
      break;
    default:
      record.effect = EffectLabel::None;
      record.requires_revalidation = false;
      break;
  }

  const bool is_pending = !is_terminal(next);
  if (was_pending && !is_pending) {
    if (pending_attempts_ > 0) pending_attempts_ -= 1;
  } else if (!was_pending && is_pending) {
    pending_attempts_ += 1;
  }
}

// ---------------------------------------------------------------------------
// indexes, eviction, compaction
// ---------------------------------------------------------------------------

std::size_t PacingFabric::find_envelope(EnvelopeId id) const {
  const auto it = envelope_index_.find(id.value());
  if (it == envelope_index_.end()) return kNoIndex;
  if (it->second >= envelopes_.size()) return kNoIndex;
  if (!envelopes_[it->second].live) return kNoIndex;
  return it->second;
}

std::size_t PacingFabric::find_attempt(AttemptId id) const {
  const auto it = attempt_index_.find(id.value());
  if (it == attempt_index_.end()) return kNoIndex;
  if (it->second >= attempts_.size()) return kNoIndex;
  if (!attempts_[it->second].live) return kNoIndex;
  return it->second;
}

std::size_t PacingFabric::find_backend(BackendId id) const {
  for (std::size_t i = 0; i < backends_.size(); ++i) {
    if (backends_[i].first == id) return i;
  }
  return kNoIndex;
}

BackendPtr PacingFabric::backend_ptr(BackendId id) const {
  const std::size_t idx = find_backend(id);
  if (idx == kNoIndex) return nullptr;
  return backends_[idx].second;
}

std::size_t PacingFabric::find_existing_attempt(EnvelopeRef envelope, BackendId backend) const {
  const auto found = attempts_by_envelope_.find(envelope.id.value());
  if (found == attempts_by_envelope_.end()) return kNoIndex;
  for (const AttemptId id : found->second) {
    const std::size_t idx = find_attempt(id);
    if (idx == kNoIndex) continue;
    const ApplicationRecord& record = attempts_[idx].record;
    if (record.backend.id == backend) return idx;
  }
  return kNoIndex;
}

void PacingFabric::evict_and_compact_locked() {
  const Limits& limits = config_.limits;

  if ((dead_envelopes_ * kCompactionTrigger) > envelopes_.size() && envelopes_.size() > 1024) {
    compact_envelopes_locked();
  }
  if ((dead_attempts_ * kCompactionTrigger) > attempts_.size() && attempts_.size() > 1024) {
    compact_attempts_locked();
  }

  if (envelopes_.size() < limits.max_envelopes) return;

  const std::size_t begin = std::min(envelope_scan_hint_, envelopes_.size());
  const std::size_t end = std::min(envelopes_.size(), begin + kEvictionScanWindow);
  for (std::size_t i = begin; i < end; ++i) {
    EnvelopeEntry& candidate = envelopes_[i];
    if (!candidate.live) continue;
    bool evictable = true;
    const auto found = attempts_by_envelope_.find(candidate.envelope.ref.id.value());
    if (found != attempts_by_envelope_.end()) {
      for (const AttemptId id : found->second) {
        const std::size_t idx = find_attempt(id);
        if (idx == kNoIndex) continue;
        if (!is_terminal(attempts_[idx].record.state)) {
          evictable = false;
          break;
        }
      }
    }
    if (evictable) {
      candidate.live = false;
      candidate.revoke_reason = "evicted: envelope table at capacity";
      envelope_index_.erase(candidate.envelope.ref.id.value());
      const auto latest = latest_envelope_by_flow_.find(candidate.envelope.flow.value());
      if (latest != latest_envelope_by_flow_.end() && latest->second == i) {
        latest_envelope_by_flow_.erase(latest);
      }
      dead_envelopes_ += 1;
      stats_.envelopes_evicted += 1;
      envelope_scan_hint_ = i + 1;
      return;
    }
  }
  envelope_scan_hint_ = end >= envelopes_.size() ? 0 : end;
}

void PacingFabric::compact_envelopes_locked() {
  std::vector<EnvelopeEntry> compacted;
  compacted.reserve(envelopes_.size());
  envelope_index_.clear();
  latest_envelope_by_flow_.clear();
  for (auto& entry : envelopes_) {
    if (!entry.live) continue;
    compacted.push_back(std::move(entry));
  }
  envelopes_ = std::move(compacted);
  for (std::size_t i = 0; i < envelopes_.size(); ++i) {
    envelope_index_[envelopes_[i].envelope.ref.id.value()] = i;
    latest_envelope_by_flow_[envelopes_[i].envelope.flow.value()] = i;
  }
  dead_envelopes_ = 0;
  envelope_scan_hint_ = 0;
}

void PacingFabric::compact_attempts_locked() {
  std::vector<AttemptEntry> compacted;
  compacted.reserve(attempts_.size());
  attempt_index_.clear();
  for (auto& entry : attempts_) {
    if (!entry.live) continue;
    compacted.push_back(std::move(entry));
  }
  attempts_ = std::move(compacted);
  for (std::size_t i = 0; i < attempts_.size(); ++i) {
    attempt_index_[attempts_[i].record.attempt.value()] = i;
  }
  dead_attempts_ = 0;
  attempt_scan_hint_ = 0;
}

// ---------------------------------------------------------------------------
// durability
// ---------------------------------------------------------------------------

Status PacingFabric::journal_record(DurableRecordType type, std::vector<u8> payload, u64 timestamp_ns) {
  if (!durable_) return Status::success();
  Status ready = maybe_checkpoint_locked();
  if (!ready) return ready;

  DurableRecord record{};
  record.type = type;
  record.sequence = ++durable_sequence_;
  record.timestamp_ns = timestamp_ns;
  record.payload = std::move(payload);
  Status s = durable_->append(std::move(record));
  if (!s) return s;
  stats_.journal_appends += 1;
  return Status::success();
}

Status PacingFabric::maybe_checkpoint_locked() {
  if (!durable_) return Status::success();
  if (!durable_->needs_checkpoint()) return Status::success();

  // Compacted records continue the same monotonic sequence as journal records.
  // The snapshot's sequence barrier is therefore strictly greater than every
  // record the snapshot supersedes, so an interruption between the snapshot
  // rename and the journal truncation can never replay stale state over it.
  std::vector<DurableRecord> compacted;
  u64 sequence = durable_sequence_ + 1;
  auto push = [&compacted, &sequence](DurableRecordType type, std::vector<u8> payload, u64 ts) {
    DurableRecord rec{};
    rec.type = type;
    rec.sequence = sequence++;
    rec.timestamp_ns = ts;
    rec.payload = std::move(payload);
    compacted.push_back(std::move(rec));
  };

  const u64 now_ns = clock_->now().ns();

  for (const FlowBinding& binding : bindings_.live_bindings()) {
    ByteWriter w(kEncodeCapacityBytes);
    encode_binding(w, binding);
    if (!w.ok()) return w.status();
    push(DurableRecordType::FlowBinding, w.take(), now_ns);
  }
  for (const PacingPolicy& policy : policies_.live_policies()) {
    ByteWriter w(kEncodeCapacityBytes);
    encode_policy(w, policy);
    if (!w.ok()) return w.status();
    push(DurableRecordType::Policy, w.take(), now_ns);
  }
  for (const auto& entry : envelopes_) {
    if (!entry.live) continue;
    ByteWriter w(kEncodeCapacityBytes);
    encode_envelope(w, entry.envelope);
    if (!w.ok()) return w.status();
    push(DurableRecordType::Envelope, w.take(), now_ns);
    if (entry.revoked) {
      ByteWriter rw(kEncodeCapacityBytes);
      encode_revocation(rw, entry.envelope.ref, entry.revoke_reason);
      if (!rw.ok()) return rw.status();
      push(DurableRecordType::EnvelopeRevocation, rw.take(), now_ns);
    }
  }
  for (const auto& entry : attempts_) {
    if (!entry.live) continue;
    ByteWriter w(kEncodeCapacityBytes);
    encode_attempt(w, entry.record);
    if (!w.ok()) return w.status();
    push(DurableRecordType::Attempt, w.take(), now_ns);
  }
  for (const auto& entry : backends_) {
    if (!entry.second) continue;
    ByteWriter w(kEncodeCapacityBytes);
    encode_backend_descriptor(w, entry.second->describe());
    if (!w.ok()) return w.status();
    push(DurableRecordType::BackendRegistration, w.take(), now_ns);
  }
  {
    ByteWriter w(256);
    encode_epoch(w, epoch_, boot_);
    if (!w.ok()) return w.status();
    push(DurableRecordType::EpochAdvance, w.take(), now_ns);
  }

  Status s = durable_->checkpoint(compacted);
  if (!s) return s;
  durable_sequence_ = sequence - 1;
  stats_.checkpoints += 1;
  return Status::success();
}

// ---------------------------------------------------------------------------
// recovery
// ---------------------------------------------------------------------------

Status PacingFabric::recover_locked() {
  recovery_.performed = true;
  const Instant now = sample_clock();
  const std::vector<DurableRecord>& records = durable_->recovered();
  recovery_.records_replayed = records.size();

  u64 max_envelope_id = 0;
  u64 max_attempt_id = 0;
  u64 max_provenance_id = 0;

  for (const DurableRecord& record : records) {
    if (record.sequence > durable_sequence_) durable_sequence_ = record.sequence;
    ByteReader reader(std::span<const u8>(record.payload.data(), record.payload.size()));
    switch (record.type) {
      case DurableRecordType::FlowBinding: {
        FlowBinding binding{};
        Status s = decode_binding(reader, config_.limits, binding);
        if (!s || !reader.exhausted()) {
          recovery_.records_rejected += 1;
          break;
        }
        (void)bindings_.restore(binding);
        break;
      }
      case DurableRecordType::Policy: {
        PacingPolicy policy{};
        Status s = decode_policy(reader, config_.limits, policy);
        if (!s || !reader.exhausted()) {
          recovery_.records_rejected += 1;
          break;
        }
        (void)policies_.restore(policy);
        break;
      }
      case DurableRecordType::Envelope: {
        PacingEnvelope envelope{};
        Status s = decode_envelope(reader, config_.limits, envelope);
        if (!s || !reader.exhausted()) {
          recovery_.records_rejected += 1;
          break;
        }
        const std::size_t existing = find_envelope(envelope.ref.id);
        if (existing != kNoIndex) {
          envelopes_[existing].envelope = envelope;
          break;
        }
        EnvelopeEntry entry{};
        entry.envelope = envelope;
        entry.live = true;
        envelopes_.push_back(std::move(entry));
        const std::size_t index = envelopes_.size() - 1;
        envelope_index_[envelope.ref.id.value()] = index;
        latest_envelope_by_flow_[envelope.flow.value()] = index;
        if (envelope.ref.id.value() > max_envelope_id) max_envelope_id = envelope.ref.id.value();
        if (envelope.provenance.id.value() > max_provenance_id) {
          max_provenance_id = envelope.provenance.id.value();
        }
        break;
      }
      case DurableRecordType::Attempt: {
        ApplicationRecord attempt_record{};
        Status s = decode_attempt(reader, config_.limits, attempt_record);
        if (!s || !reader.exhausted()) {
          recovery_.records_rejected += 1;
          break;
        }
        const std::size_t existing = find_attempt(attempt_record.attempt);
        if (existing != kNoIndex) {
          attempts_[existing].record = attempt_record;
          break;
        }
        AttemptEntry entry{};
        entry.record = attempt_record;
        entry.live = true;
        attempts_.push_back(std::move(entry));
        const std::size_t index = attempts_.size() - 1;
        attempt_index_[attempt_record.attempt.value()] = index;
        attempts_by_envelope_[attempt_record.envelope.id.value()].push_back(attempt_record.attempt);
        if (attempt_record.attempt.value() > max_attempt_id) max_attempt_id = attempt_record.attempt.value();
        break;
      }
      case DurableRecordType::EnvelopeRevocation: {
        EnvelopeRef ref{};
        std::string reason;
        Status s = decode_revocation(reader, ref, reason);
        if (!s || !reader.exhausted()) {
          recovery_.records_rejected += 1;
          break;
        }
        const std::size_t idx = find_envelope(ref.id);
        if (idx != kNoIndex) {
          envelopes_[idx].revoked = true;
          envelopes_[idx].revoke_reason = std::move(reason);
        }
        break;
      }
      case DurableRecordType::EpochAdvance: {
        Epoch persisted{};
        BootId persisted_boot{};
        Status s = decode_epoch(reader, persisted, persisted_boot);
        if (!s || !reader.exhausted()) {
          recovery_.records_rejected += 1;
          break;
        }
        recovery_.epoch_before = persisted;
        recovery_.boot_before = persisted_boot;
        if (persisted.value() > epoch_.value()) epoch_ = Epoch::from(persisted.value());
        if (persisted_boot.counter >= boot_.counter) {
          boot_.counter = persisted_boot.counter;
          u64 attempt_nonce = make_nonce(boot_.counter * 0x100000001B3ull + config_.fabric_instance);
          for (int i = 0; i < 8 && attempt_nonce == persisted_boot.nonce; ++i) {
            attempt_nonce = make_nonce(attempt_nonce ^ static_cast<u64>(i + 1));
          }
          boot_.nonce = attempt_nonce;
        }
        break;
      }
      case DurableRecordType::BackendRegistration: {
        BackendDescriptor descriptor{};
        Status s = decode_backend_descriptor(reader, config_.limits, descriptor);
        if (!s || !reader.exhausted()) {
          recovery_.records_rejected += 1;
          break;
        }
        // Descriptors are restored as historical lineage only. No backend
        // handle, liveness or effect survives a restart; the operator must
        // register a live backend before any pacing can be re-established.
        recovery_.backends_requiring_registration += 1;
        break;
      }
      case DurableRecordType::FlowUnbound: {
        u64 flow_id = 0;
        Status s = decode_identity_u64(reader, flow_id);
        if (!s || !reader.exhausted()) {
          recovery_.records_rejected += 1;
          break;
        }
        (void)bindings_.unbind(FlowId::from(flow_id));
        break;
      }
      case DurableRecordType::PolicyRemoved: {
        u64 policy_id = 0;
        Status s = decode_identity_u64(reader, policy_id);
        if (!s || !reader.exhausted()) {
          recovery_.records_rejected += 1;
          break;
        }
        (void)policies_.remove(PolicyId::from(policy_id));
        break;
      }
      case DurableRecordType::Meta:
      default:
        recovery_.records_rejected += 1;
        break;
    }
  }

  // Restored counts describe live entities, not the number of replay records
  // that mentioned them: repeated journal entries for the same binding must not
  // inflate the report.
  recovery_.bindings_restored = bindings_.live_bindings().size();
  recovery_.policies_restored = policies_.live_policies().size();

  // --- coordinator epoch and boot advance across restarts ----------------
  bool exhausted = false;
  const u64 persisted_epoch = epoch_.value();
  epoch_ = epoch_.advanced(exhausted);
  if (exhausted) return Status::of(ErrorCode::ResourceExhausted, "coordinator epoch is exhausted");
  if (epoch_.value() <= persisted_epoch) return Status::of(ErrorCode::Internal, "epoch did not advance");
  u64 next_boot = boot_.counter;
  if (next_boot == UINT64_MAX) return Status::of(ErrorCode::ResourceExhausted, "boot counter is exhausted");
  boot_.counter = next_boot + 1;
  boot_.nonce = make_nonce(boot_.counter * 0x9E3779B97F4A7C15ull + config_.fabric_instance);
  recovery_.epoch_after = epoch_;
  recovery_.boot_after = boot_;

  // --- nothing that was in force is restored as in force ------------------
  for (auto& entry : envelopes_) {
    if (!entry.live) continue;
    entry.revoked = true;
    entry.revoke_reason = "coordinator restart: prior epoch authority fenced";
    recovery_.envelopes_invalidated += 1;
  }
  for (auto& entry : attempts_) {
    if (!entry.live) continue;
    entry.record.requires_revalidation = true;
    if (!is_terminal(entry.record.state)) {
      // An attempt that never reached a terminal state has a genuinely
      // unknown outcome: the backend may or may not have installed pacing.
      entry.record.last_known_state = entry.record.state;
      entry.record.state = ApplicationState::Ambiguous;
      entry.record.reason = ErrorCode::Ambiguous;
      entry.record.detail = "coordinator restart during an in-flight application attempt";
      recovery_.attempts_ambiguous += 1;
    } else if (asserts_effect(entry.record.state) ||
               entry.record.state == ApplicationState::AppliedUnverified ||
               entry.record.state == ApplicationState::Mismatched) {
      entry.record.last_known_state = entry.record.state;
      entry.record.state = ApplicationState::RequiresRevalidation;
      entry.record.reason = ErrorCode::RequiresRevalidation;
      entry.record.detail = "coordinator restart: effect not restored without revalidation";
    }
    entry.record.effect = EffectLabel::None;
    recovery_.attempts_requiring_revalidation += 1;
    auto payload = encode_attempt_payload(entry.record);
    if (payload) (void)journal_record(DurableRecordType::Attempt, payload.value(), now.ns());
  }

  // The pending counter is recomputed from recovered state rather than being
  // carried across the restart.
  pending_attempts_ = 0;
  for (const auto& entry : attempts_) {
    if (entry.live && !is_terminal(entry.record.state)) pending_attempts_ += 1;
  }

  envelope_ids_.set_floor(max_envelope_id);
  attempt_ids_.set_floor(max_attempt_id);
  provenance_ids_.set_floor(max_provenance_id);

  {
    std::vector<u8> payload;
    ByteWriter w(256);
    encode_epoch(w, epoch_, boot_);
    if (!w.ok()) return w.status();
    payload = w.take();
    Status s = journal_record(DurableRecordType::EpochAdvance, std::move(payload), now.ns());
    if (!s) return s;
  }

  return Status::success();
}

}  // namespace pacing
