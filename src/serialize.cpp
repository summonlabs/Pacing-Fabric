// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/serialize.hpp"

namespace pacing {
namespace {

void encode_genref(ByteWriter& w, u64 id, u64 generation) {
  w.u64v(id);
  w.u64v(generation);
}

void encode_burst(ByteWriter& w, u64 bytes, u64 packets) {
  w.u64v(bytes);
  w.u64v(packets);
}

}  // namespace

void encode_cadence(ByteWriter& w, const Cadence& cadence) {
  w.u64v(cadence.rate_bps);
  w.u64v(cadence.quantum_bytes);
  w.u64v(cadence.interval_ns);
  w.u64v(cadence.window_ns);
  w.u64v(cadence.grants_per_window);
  w.u64v(cadence.bytes_per_window);
  w.u64v(cadence.burst_bytes);
  w.u64v(cadence.burst_packets);
  w.u8v(static_cast<u8>(cadence.shape));
}

Status decode_cadence(ByteReader& r, Cadence& out) {
  out = Cadence{};
  out.rate_bps = r.u64v();
  out.quantum_bytes = r.u64v();
  out.interval_ns = r.u64v();
  out.window_ns = r.u64v();
  out.grants_per_window = r.u64v();
  out.bytes_per_window = r.u64v();
  out.burst_bytes = r.u64v();
  out.burst_packets = r.u64v();
  const u8 shape = r.u8v();
  if (!r.ok()) return r.status();
  if (shape > static_cast<u8>(CadenceShape::TokenBucket)) {
    return Status::of(ErrorCode::MalformedInput, "unknown cadence shape discriminator");
  }
  out.shape = static_cast<CadenceShape>(shape);
  return Status::success();
}

void encode_authority(ByteWriter& w, const AuthorityVector& authority) {
  encode_genref(w, authority.flow.id.value(), authority.flow.generation.value());
  encode_genref(w, authority.resource.id.value(), authority.resource.generation.value());
  encode_genref(w, authority.policy.id.value(), authority.policy.generation.value());
  encode_genref(w, authority.rate_grant.id.value(), authority.rate_grant.generation.value());
  encode_genref(w, authority.path.id.value(), authority.path.generation.value());
  w.u64v(authority.epoch.value());
  w.u64v(authority.coordinator_boot.counter);
  w.u64v(authority.coordinator_boot.nonce);
}

Status decode_authority(ByteReader& r, AuthorityVector& out) {
  out = AuthorityVector{};
  out.flow = FlowRef{FlowId::from(r.u64v()), Generation::from(r.u64v())};
  out.resource = ResourceRef{ResourceId::from(r.u64v()), Generation::from(r.u64v())};
  out.policy = PolicyRef{PolicyId::from(r.u64v()), Generation::from(r.u64v())};
  out.rate_grant = GrantRef{GrantId::from(r.u64v()), Generation::from(r.u64v())};
  out.path = PathRef{PathId::from(r.u64v()), Generation::from(r.u64v())};
  out.epoch = Epoch::from(r.u64v());
  out.coordinator_boot.counter = r.u64v();
  out.coordinator_boot.nonce = r.u64v();
  if (!r.ok()) return r.status();
  if (!out.complete()) {
    return Status::of(ErrorCode::MalformedInput, "decoded authority vector is incomplete");
  }
  return Status::success();
}

void encode_binding(ByteWriter& w, const FlowBinding& binding) {
  w.u64v(binding.flow.value());
  w.u64v(binding.generation.value());
  encode_genref(w, binding.resource.id.value(), binding.resource.generation.value());
  encode_genref(w, binding.path.id.value(), binding.path.generation.value());
  w.u64v(binding.service_class.value());
  w.u64v(binding.tenant.value());
  w.u64v(binding.declared_max_burst_bytes);
  w.u64v(binding.declared_max_burst_packets);
  w.boolean(binding.quiesced);
  w.u64v(binding.provenance.value());
}

Status decode_binding(ByteReader& r, const Limits& limits, FlowBinding& out) {
  out = FlowBinding{};
  out.flow = FlowId::from(r.u64v());
  out.generation = Generation::from(r.u64v());
  out.resource = ResourceRef{ResourceId::from(r.u64v()), Generation::from(r.u64v())};
  out.path = PathRef{PathId::from(r.u64v()), Generation::from(r.u64v())};
  out.service_class = ServiceClassId::from(r.u64v());
  out.tenant = TenantId::from(r.u64v());
  out.declared_max_burst_bytes = r.u64v();
  out.declared_max_burst_packets = r.u64v();
  out.quiesced = r.boolean();
  out.provenance = ProvenanceId::from(r.u64v());
  if (!r.ok()) return r.status();
  if (!out.flow.valid() || !out.generation.known()) {
    return Status::of(ErrorCode::MalformedInput, "decoded binding identity is invalid");
  }
  if (out.declared_max_burst_bytes > limits.max_burst_bytes) {
    return Status::of(ErrorCode::BurstExceedsBound, "decoded binding burst bytes above limit");
  }
  if (out.declared_max_burst_packets > limits.max_burst_packets) {
    return Status::of(ErrorCode::BurstExceedsBound, "decoded binding burst packets above limit");
  }
  return Status::success();
}

void encode_policy(ByteWriter& w, const PacingPolicy& policy) {
  w.u64v(policy.ref.id.value());
  w.u64v(policy.ref.generation.value());
  w.u8v(static_cast<u8>(policy.layer));
  w.u64v(policy.resource.value());
  w.u64v(policy.flow.value());
  w.u64v(policy.rate_share_ppm);
  w.u64v(policy.rate_bps);
  w.u64v(policy.min_rate_bps);
  encode_burst(w, policy.burst_bytes, policy.burst_packets);
  w.u64v(policy.quantum_bytes);
  w.u64v(policy.window_ns);
  w.u8v(static_cast<u8>(policy.shape));
  w.u64v(policy.grants_per_window_hint);
  encode_genref(w, policy.path.id.value(), policy.path.generation.value());
  w.u64v(policy.default_service_class.value());
  w.u64v(policy.tenant.value());
  w.u64v(policy.provenance.value());
  w.u64v(policy.published_at.ns());
  w.u32v(static_cast<u32>(policy.exceptions.size()));
  for (const auto& ex : policy.exceptions) {
    w.u64v(ex.service_class.value());
    w.u64v(ex.min_rate_bps);
    w.u64v(ex.rate_share_ppm);
    encode_burst(w, ex.burst_bytes, ex.burst_packets);
    w.u32v(ex.priority);
  }
}

Status decode_policy(ByteReader& r, const Limits& limits, PacingPolicy& out) {
  out = PacingPolicy{};
  out.ref.id = PolicyId::from(r.u64v());
  out.ref.generation = Generation::from(r.u64v());
  const u8 layer = r.u8v();
  out.resource = ResourceId::from(r.u64v());
  out.flow = FlowId::from(r.u64v());
  out.rate_share_ppm = r.u64v();
  out.rate_bps = r.u64v();
  out.min_rate_bps = r.u64v();
  out.burst_bytes = r.u64v();
  out.burst_packets = r.u64v();
  out.quantum_bytes = r.u64v();
  out.window_ns = r.u64v();
  const u8 shape = r.u8v();
  out.grants_per_window_hint = r.u64v();
  out.path = PathRef{PathId::from(r.u64v()), Generation::from(r.u64v())};
  out.default_service_class = ServiceClassId::from(r.u64v());
  out.tenant = TenantId::from(r.u64v());
  out.provenance = ProvenanceId::from(r.u64v());
  out.published_at = Instant::from_ns(r.u64v());
  const u32 exception_count = r.u32v();
  if (!r.ok()) return r.status();
  if (layer > static_cast<u8>(PacingLayer::Flow)) {
    return Status::of(ErrorCode::MalformedInput, "unknown pacing layer discriminator");
  }
  if (shape > static_cast<u8>(CadenceShape::TokenBucket)) {
    return Status::of(ErrorCode::MalformedInput, "unknown cadence shape discriminator");
  }
  if (exception_count > limits.max_service_class_exceptions) {
    return Status::of(ErrorCode::Oversized, "decoded policy exception count above bound");
  }
  out.layer = static_cast<PacingLayer>(layer);
  out.shape = static_cast<CadenceShape>(shape);
  out.exceptions.reserve(exception_count);
  for (u32 i = 0; i < exception_count; ++i) {
    ServiceClassException ex{};
    ex.service_class = ServiceClassId::from(r.u64v());
    ex.min_rate_bps = r.u64v();
    ex.rate_share_ppm = r.u64v();
    ex.burst_bytes = r.u64v();
    ex.burst_packets = r.u64v();
    ex.priority = r.u32v();
    if (!r.ok()) return r.status();
    out.exceptions.push_back(ex);
  }
  if (!out.ref.generation.known()) {
    // A publication request states an identity, not a revision: the store
    // assigns the first generation. Shape validation still applies, so a
    // request cannot smuggle an invalid policy past the decoder.
    PacingPolicy probe = out;
    probe.ref.generation = Generation::first();
    return validate_policy(probe, limits);
  }
  return validate_policy(out, limits);
}

void encode_envelope(ByteWriter& w, const PacingEnvelope& envelope) {
  w.u64v(envelope.ref.id.value());
  w.u64v(envelope.ref.generation.value());
  w.u64v(envelope.flow.value());
  w.u64v(envelope.resource.value());
  encode_authority(w, envelope.authority);
  encode_cadence(w, envelope.cadence);
  w.u64v(envelope.requested_rate_bps);
  w.u64v(envelope.ceiling_bps);
  w.u64v(envelope.floor_bps);
  w.boolean(envelope.clamped_to_ceiling);
  w.boolean(envelope.clamped_to_floor);
  w.u64v(envelope.service_class.value());
  w.u64v(envelope.provenance.id.value());
  w.u64v(envelope.provenance.sequence);
  w.u64v(envelope.provenance.fabric_instance);
  w.u64v(envelope.provenance.binding_digest);
  w.u64v(envelope.provenance.policy_digest);
  w.u64v(envelope.provenance.grant_digest);
  w.u64v(envelope.provenance.cadence_digest);
  w.u64v(envelope.derived_at.ns());
  w.boolean(envelope.valid_until.is_set());
  w.u64v(envelope.valid_until.is_set() ? envelope.valid_until.instant().ns() : 0);
  w.boolean(envelope.upstream_synthetic);
  w.boolean(envelope.policy_synthetic);
}

Status decode_envelope(ByteReader& r, const Limits& limits, PacingEnvelope& out) {
  (void)limits;
  out = PacingEnvelope{};
  out.ref.id = EnvelopeId::from(r.u64v());
  out.ref.generation = Generation::from(r.u64v());
  out.flow = FlowId::from(r.u64v());
  out.resource = ResourceId::from(r.u64v());
  Status auth = decode_authority(r, out.authority);
  if (!auth) return auth;
  Status cad = decode_cadence(r, out.cadence);
  if (!cad) return cad;
  out.requested_rate_bps = r.u64v();
  out.ceiling_bps = r.u64v();
  out.floor_bps = r.u64v();
  out.clamped_to_ceiling = r.boolean();
  out.clamped_to_floor = r.boolean();
  out.service_class = ServiceClassId::from(r.u64v());
  out.provenance.id = ProvenanceId::from(r.u64v());
  out.provenance.sequence = r.u64v();
  out.provenance.fabric_instance = r.u64v();
  out.provenance.binding_digest = r.u64v();
  out.provenance.policy_digest = r.u64v();
  out.provenance.grant_digest = r.u64v();
  out.provenance.cadence_digest = r.u64v();
  out.derived_at = Instant::from_ns(r.u64v());
  if (r.boolean()) {
    out.valid_until = Deadline::at(Instant::from_ns(r.u64v()));
  } else {
    (void)r.u64v();
  }
  out.upstream_synthetic = r.boolean();
  out.policy_synthetic = r.boolean();
  if (!r.ok()) return r.status();
  if (!out.ref.id.valid() || !out.ref.generation.known() || !out.flow.valid()) {
    return Status::of(ErrorCode::MalformedInput, "decoded envelope identity is invalid");
  }
  if (out.cadence.rate_bps > out.ceiling_bps) {
    return Status::of(ErrorCode::CeilingExceeded, "decoded envelope exceeds its recorded ceiling");
  }
  return Status::success();
}

void encode_attempt(ByteWriter& w, const ApplicationRecord& record) {
  w.u64v(record.attempt.value());
  w.u64v(record.envelope.id.value());
  w.u64v(record.envelope.generation.value());
  w.u64v(record.flow.value());
  w.u64v(record.resource.value());
  encode_authority(w, record.authority);
  encode_cadence(w, record.desired);
  w.u8v(static_cast<u8>(record.state));
  w.u8v(static_cast<u8>(record.last_known_state));
  w.u8v(static_cast<u8>(record.effect));
  w.u8v(static_cast<u8>(record.evidence_kind));
  w.u64v(record.observed_rate_bps);
  w.u64v(record.observed_quantum_bytes);
  w.u64v(record.observed_burst_bytes);
  w.u64v(record.observed_interval_ns);
  w.u64v(record.backend_seq);
  w.u64v(record.evidence_digest);
  w.boolean(record.evidence_integrity_ok);
  encode_genref(w, record.backend.id.value(), record.backend.generation.value());
  w.u64v(record.worker.incarnation);
  w.u64v(record.worker.nonce);
  w.boolean(record.backend_synthetic);
  w.u16v(static_cast<u16>(record.reason));
  w.u8v(static_cast<u8>(record.drift));
  w.str(record.detail);
  w.u64v(record.revision);
  w.u64v(record.created_ns);
  w.u64v(record.updated_ns);
  w.boolean(record.requires_revalidation);
}

Status decode_attempt(ByteReader& r, const Limits& limits, ApplicationRecord& out) {
  (void)limits;
  out = ApplicationRecord{};
  out.attempt = AttemptId::from(r.u64v());
  out.envelope.id = EnvelopeId::from(r.u64v());
  out.envelope.generation = Generation::from(r.u64v());
  out.flow = FlowId::from(r.u64v());
  out.resource = ResourceId::from(r.u64v());
  Status auth = decode_authority(r, out.authority);
  if (!auth) return auth;
  Status cad = decode_cadence(r, out.desired);
  if (!cad) return cad;
  const u8 state = r.u8v();
  const u8 last_state = r.u8v();
  const u8 effect = r.u8v();
  const u8 evidence = r.u8v();
  out.observed_rate_bps = r.u64v();
  out.observed_quantum_bytes = r.u64v();
  out.observed_burst_bytes = r.u64v();
  out.observed_interval_ns = r.u64v();
  out.backend_seq = r.u64v();
  out.evidence_digest = r.u64v();
  out.evidence_integrity_ok = r.boolean();
  out.backend = BackendRef{BackendId::from(r.u64v()), Generation::from(r.u64v())};
  out.worker.incarnation = r.u64v();
  out.worker.nonce = r.u64v();
  out.backend_synthetic = r.boolean();
  out.reason = static_cast<ErrorCode>(r.u16v());
  const u8 drift = r.u8v();
  out.detail = r.str(limits.max_explanation_bytes);
  out.revision = r.u64v();
  out.created_ns = r.u64v();
  out.updated_ns = r.u64v();
  out.requires_revalidation = r.boolean();
  if (!r.ok()) return r.status();
  if (!out.attempt.valid()) {
    return Status::of(ErrorCode::MalformedInput, "decoded attempt identity is invalid");
  }
  if (state > static_cast<u8>(ApplicationState::Ambiguous) ||
      last_state > static_cast<u8>(ApplicationState::Ambiguous) ||
      effect > static_cast<u8>(EffectLabel::Mismatched) ||
      evidence > static_cast<u8>(EvidenceKind::Injected) ||
      drift > static_cast<u8>(AuthorityDrift::Incomplete)) {
    return Status::of(ErrorCode::MalformedInput, "decoded attempt enum discriminator out of range");
  }
  out.state = static_cast<ApplicationState>(state);
  out.last_known_state = static_cast<ApplicationState>(last_state);
  out.effect = static_cast<EffectLabel>(effect);
  out.evidence_kind = static_cast<EvidenceKind>(evidence);
  out.drift = static_cast<AuthorityDrift>(drift);
  return Status::success();
}

void encode_grant(ByteWriter& w, const RateGrant& grant) {
  encode_genref(w, grant.ref.id.value(), grant.ref.generation.value());
  w.u64v(grant.resource.value());
  w.u64v(grant.ceiling_bps);
  w.u64v(grant.floor_bps);
  w.u64v(grant.revision);
  w.u64v(grant.observed_at.ns());
  w.boolean(grant.valid_until.is_set());
  w.u64v(grant.valid_until.is_set() ? grant.valid_until.instant().ns() : 0);
  w.u64v(grant.provenance.value());
  w.boolean(grant.authoritative);
}

Status decode_grant(ByteReader& r, const Limits& limits, RateGrant& out) {
  out = RateGrant{};
  out.ref.id = GrantId::from(r.u64v());
  out.ref.generation = Generation::from(r.u64v());
  out.resource = ResourceId::from(r.u64v());
  out.ceiling_bps = r.u64v();
  out.floor_bps = r.u64v();
  out.revision = r.u64v();
  out.observed_at = Instant::from_ns(r.u64v());
  if (r.boolean()) {
    out.valid_until = Deadline::at(Instant::from_ns(r.u64v()));
  } else {
    (void)r.u64v();
  }
  out.provenance = ProvenanceId::from(r.u64v());
  out.authoritative = r.boolean();
  if (!r.ok()) return r.status();
  if (out.ceiling_bps > limits.max_rate_bps) {
    return Status::of(ErrorCode::OutOfRange, "decoded grant ceiling above configured limit");
  }
  if (out.floor_bps > out.ceiling_bps) {
    return Status::of(ErrorCode::FloorExceedsCeiling, "decoded grant floor exceeds its ceiling");
  }
  return Status::success();
}

void encode_revocation(ByteWriter& w, EnvelopeRef envelope, std::string_view reason) {
  w.u64v(envelope.id.value());
  w.u64v(envelope.generation.value());
  w.str(reason);
}

Status decode_revocation(ByteReader& r, EnvelopeRef& envelope, std::string& reason) {
  envelope.id = EnvelopeId::from(r.u64v());
  envelope.generation = Generation::from(r.u64v());
  reason = r.str(256);
  if (!r.ok()) return r.status();
  if (!envelope.id.valid()) {
    return Status::of(ErrorCode::MalformedInput, "decoded revocation has no envelope identity");
  }
  return Status::success();
}

void encode_epoch(ByteWriter& w, Epoch epoch, BootId boot) {
  w.u64v(epoch.value());
  w.u64v(boot.counter);
  w.u64v(boot.nonce);
}

Status decode_epoch(ByteReader& r, Epoch& epoch, BootId& boot) {
  epoch = Epoch::from(r.u64v());
  boot.counter = r.u64v();
  boot.nonce = r.u64v();
  if (!r.ok()) return r.status();
  if (!epoch.valid()) {
    return Status::of(ErrorCode::MalformedInput, "decoded epoch is not valid");
  }
  return Status::success();
}

void encode_identity_u64(ByteWriter& w, u64 value) { w.u64v(value); }

Status decode_identity_u64(ByteReader& r, u64& value) {
  value = r.u64v();
  if (!r.ok()) return r.status();
  return Status::success();
}

void encode_backend_descriptor(ByteWriter& w, const BackendDescriptor& descriptor) {
  encode_genref(w, descriptor.ref.id.value(), descriptor.ref.generation.value());
  w.str(descriptor.name);
  w.u8v(static_cast<u8>(descriptor.nature));
  w.boolean(descriptor.supports_readback);
  w.u64v(descriptor.max_rate_bps);
  w.u64v(descriptor.max_burst_bytes);
  w.u64v(descriptor.capability_flags);
}

Status decode_backend_descriptor(ByteReader& r, const Limits& limits, BackendDescriptor& out) {
  out = BackendDescriptor{};
  out.ref.id = BackendId::from(r.u64v());
  out.ref.generation = Generation::from(r.u64v());
  out.name = r.str(limits.max_name_bytes);
  const u8 nature = r.u8v();
  out.supports_readback = r.boolean();
  out.max_rate_bps = r.u64v();
  out.max_burst_bytes = r.u64v();
  out.capability_flags = r.u64v();
  if (!r.ok()) return r.status();
  if (nature > static_cast<u8>(BackendNature::Synthetic)) {
    return Status::of(ErrorCode::MalformedInput, "unknown backend nature discriminator");
  }
  out.nature = static_cast<BackendNature>(nature);
  if (!out.ref.id.valid() || !out.ref.generation.known() || out.name.empty()) {
    return Status::of(ErrorCode::MalformedInput, "decoded backend descriptor is incomplete");
  }
  return Status::success();
}

}  // namespace pacing
