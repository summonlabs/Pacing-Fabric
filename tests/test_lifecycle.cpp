// End-to-end fabric lifecycle: binding, policy, derivation, application,
// verification, idempotency and authority-drift invalidation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"
#include "harness.hpp"

using namespace pacing;
using pf_test::Harness;
using pf_test::HarnessOptions;
using pf_test::LoopbackBackend;

namespace {

// Configures a harness in place. The harness owns non-copyable state (clock,
// scripted authority), so it is never returned by value.
void make_ready(Harness& h, std::uint64_t ceiling = 10'000'000'000ull,
                std::uint64_t rate = 1'000'000'000ull, LoopbackBackend::Options options = {}) {
  h.start();
  h.install_grant(ceiling);
  h.bind_flow(h.flow);
  h.publish_policy(rate);
  h.register_backend(std::move(options));
}

}  // namespace

PF_TEST(lifecycle, derive_binds_complete_authority) {
  Harness h; make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  PF_CHECK(envelope.authority.complete());
  PF_CHECK_EQ(envelope.authority.flow.id.value(), h.flow.value());
  PF_CHECK_EQ(envelope.authority.flow.generation.value(), 1ull);
  PF_CHECK_EQ(envelope.authority.resource.id.value(), h.resource.value());
  PF_CHECK_EQ(envelope.authority.policy.id.value(), 1ull);
  PF_CHECK_EQ(envelope.authority.rate_grant.id.value(), h.grant.value());
  PF_CHECK_EQ(envelope.authority.path.id.value(), h.path.value());
  PF_CHECK_EQ(envelope.authority.epoch.value(), h.fabric->epoch().value());
  PF_CHECK_EQ(envelope.authority.coordinator_boot.counter, h.fabric->boot().counter);
  PF_CHECK_EQ(envelope.ceiling_bps, 10'000'000'000ull);
  PF_CHECK_EQ(envelope.cadence.rate_bps, 1'000'000'000ull);
  PF_CHECK(envelope.provenance.valid());
  PF_CHECK(envelope.valid_until.is_set());
}

PF_TEST(lifecycle, apply_verifies_effect_on_synthetic_backend) {
  Harness h; make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  const AttemptId id = h.apply(envelope);
  const ApplicationRecord rec = h.record(id);
  PF_CHECK_EQ(rec.state, ApplicationState::Applied);
  PF_CHECK_EQ(rec.effect, EffectLabel::SyntheticVerified);
  PF_CHECK_EQ(rec.evidence_kind, EvidenceKind::SyntheticReadback);
  PF_CHECK(rec.backend_synthetic);
  PF_CHECK(!rec.requires_revalidation);
  PF_CHECK(rec.evidence_integrity_ok);
  PF_CHECK_EQ(rec.observed_rate_bps, envelope.cadence.rate_bps);
  PF_CHECK_EQ(rec.observed_quantum_bytes, envelope.cadence.quantum_bytes);
  PF_CHECK_EQ(rec.observed_burst_bytes, envelope.cadence.burst_bytes);
  PF_CHECK(h.backend->installed(id));
}

PF_TEST(lifecycle, acknowledgement_alone_is_not_an_effect) {
  LoopbackBackend::Options options{};
  options.false_acknowledge = true;
  Harness h; make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const PacingEnvelope envelope = h.derive(h.flow);
  const AttemptId id = h.apply(envelope);
  const ApplicationRecord rec = h.record(id);
  // The backend accepted the request but installed nothing, so the fabric must
  // not claim an effect.
  PF_CHECK_EQ(rec.state, ApplicationState::AppliedUnverified);
  PF_CHECK_EQ(rec.effect, EffectLabel::Unverified);
  PF_CHECK_EQ(rec.reason, ErrorCode::ReadbackUnavailable);
  PF_CHECK(!rec.claims_effect());
}

PF_TEST(lifecycle, backend_without_readback_stays_unverified) {
  LoopbackBackend::Options options{};
  options.supports_readback = false;
  Harness h; make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const PacingEnvelope envelope = h.derive(h.flow);
  const AttemptId id = h.apply(envelope);
  const ApplicationRecord rec = h.record(id);
  PF_CHECK_EQ(rec.state, ApplicationState::AppliedUnverified);
  PF_CHECK_EQ(rec.effect, EffectLabel::Unverified);
  PF_CHECK_EQ(rec.reason, ErrorCode::ReadbackUnavailable);
  PF_CHECK_EQ(rec.evidence_kind, EvidenceKind::None);
}

PF_TEST(lifecycle, corrupt_readback_is_never_trusted) {
  LoopbackBackend::Options options{};
  options.corrupt_readback = true;
  Harness h; make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const AttemptId id = h.apply(h.derive(h.flow));
  const ApplicationRecord rec = h.record(id);
  PF_CHECK_EQ(rec.state, ApplicationState::AppliedUnverified);
  PF_CHECK_EQ(rec.reason, ErrorCode::IntegrityFailure);
  PF_CHECK(!rec.claims_effect());
}

PF_TEST(lifecycle, readback_failure_is_never_trusted) {
  LoopbackBackend::Options options{};
  options.fail_readback = true;
  Harness h; make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const AttemptId id = h.apply(h.derive(h.flow));
  const ApplicationRecord rec = h.record(id);
  PF_CHECK_EQ(rec.state, ApplicationState::AppliedUnverified);
  PF_CHECK_EQ(rec.reason, ErrorCode::BackendFailure);
}

PF_TEST(lifecycle, under_rate_readback_is_a_mismatch_not_a_pass) {
  LoopbackBackend::Options options{};
  options.rate_bias_bps = -1'000'000;
  Harness h; make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const PacingEnvelope envelope = h.derive(h.flow);
  const AttemptId id = h.apply(envelope);
  const ApplicationRecord rec = h.record(id);
  PF_CHECK_EQ(rec.state, ApplicationState::Mismatched);
  PF_CHECK_EQ(rec.effect, EffectLabel::Mismatched);
  PF_CHECK_EQ(rec.reason, ErrorCode::ReadbackMismatch);
  PF_CHECK(!rec.claims_effect());
}

PF_TEST(lifecycle, over_rate_readback_is_withdrawn) {
  LoopbackBackend::Options options{};
  options.rate_bias_bps = 5'000'000'000;
  Harness h; make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const AttemptId id = h.apply(h.derive(h.flow));
  const ApplicationRecord rec = h.record(id);
  PF_CHECK_EQ(rec.state, ApplicationState::Mismatched);
  PF_CHECK_EQ(rec.reason, ErrorCode::CeilingExceeded);
  // The over-rate effect must have been withdrawn rather than left in place.
  PF_CHECK(h.backend->revokes() >= 1);
  PF_CHECK(!h.backend->installed(id));
}

PF_TEST(lifecycle, apply_failure_is_recorded_as_failed) {
  LoopbackBackend::Options options{};
  options.fail_apply = true;
  options.apply_failure = ErrorCode::BackendUnavailable;
  Harness h; make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  auto attempt = h.fabric->apply_envelope(h.derive(h.flow).ref, h.backend_id);
  PF_CHECK_STATUS_OK(attempt);
  const ApplicationRecord rec = h.record(attempt.value());
  PF_CHECK_EQ(rec.state, ApplicationState::Failed);
  PF_CHECK_EQ(rec.reason, ErrorCode::BackendUnavailable);
  PF_CHECK(!rec.claims_effect());
}

PF_TEST(lifecycle, explicit_rejection_is_recorded_as_failed) {
  LoopbackBackend::Options options{};
  options.accept = false;
  Harness h; make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  auto attempt = h.fabric->apply_envelope(h.derive(h.flow).ref, h.backend_id);
  PF_CHECK_STATUS_OK(attempt);
  const ApplicationRecord rec = h.record(attempt.value());
  PF_CHECK_EQ(rec.state, ApplicationState::Failed);
  PF_CHECK_EQ(rec.reason, ErrorCode::BackendRejected);
}

PF_TEST(lifecycle, duplicate_apply_is_idempotent) {
  Harness h; make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  const AttemptId first = h.apply(envelope);
  const AttemptId second = h.apply(envelope);
  const AttemptId third = h.apply(envelope);
  PF_CHECK_EQ(first.value(), second.value());
  PF_CHECK_EQ(first.value(), third.value());
  // The backend must have been asked exactly once.
  PF_CHECK_EQ(h.backend->apply_entries(), 1ull);
  PF_CHECK_EQ(h.backend->accepted(), 1ull);
  PF_CHECK_EQ(h.fabric->stats().duplicate_applies, 2ull);
  PF_CHECK_EQ(h.fabric->stats().applies_reserved, 1ull);
}

PF_TEST(lifecycle, explicit_attempt_id_is_idempotent) {
  Harness h; make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  const AttemptId requested = AttemptId::from(4242);
  auto first = h.fabric->apply_envelope(envelope.ref, h.backend_id, requested);
  PF_CHECK_STATUS_OK(first);
  PF_CHECK_EQ(first.value().value(), 4242ull);
  auto second = h.fabric->apply_envelope(envelope.ref, h.backend_id, requested);
  PF_CHECK_STATUS_OK(second);
  PF_CHECK_EQ(second.value().value(), 4242ull);
  PF_CHECK_EQ(h.backend->apply_entries(), 1ull);

  // Reusing the same attempt id for a different envelope is a conflict, not a
  // silent overwrite.
  const PacingEnvelope other = h.derive(h.flow);
  auto conflict = h.fabric->apply_envelope(other.ref, h.backend_id, requested);
  PF_CHECK_CODE(conflict, ErrorCode::Duplicate);
}

PF_TEST(lifecycle, envelope_expiry_invalidates_apply) {
  Harness h; make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  h.clock.advance(2'000'000'000ull);
  auto attempt = h.fabric->apply_envelope(envelope.ref, h.backend_id);
  PF_CHECK_CODE(attempt, ErrorCode::StaleGeneration);
  PF_CHECK_EQ(h.backend->apply_entries(), 0ull);
}

PF_TEST(lifecycle, rate_grant_generation_change_invalidates_envelope) {
  Harness h; make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  h.install_grant(10'000'000'000ull, 0, 2, 2);
  auto attempt = h.fabric->apply_envelope(envelope.ref, h.backend_id);
  PF_CHECK_CODE(attempt, ErrorCode::StaleRateGrant);
  PF_CHECK_EQ(h.backend->apply_entries(), 0ull);
  PF_CHECK(h.fabric->stats().stale_rejections >= 1);
}

PF_TEST(lifecycle, policy_republish_invalidates_envelope) {
  Harness h; make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  h.publish_policy(1'000'000'000ull);
  auto attempt = h.fabric->apply_envelope(envelope.ref, h.backend_id);
  PF_CHECK_CODE(attempt, ErrorCode::StalePolicy);
}

PF_TEST(lifecycle, binding_rebind_invalidates_envelope) {
  Harness h; make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  h.bind_flow(h.flow);
  auto attempt = h.fabric->apply_envelope(envelope.ref, h.backend_id);
  PF_CHECK_CODE(attempt, ErrorCode::StaleGeneration);
}

PF_TEST(lifecycle, path_generation_change_invalidates_envelope) {
  Harness h; make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  // Replace the binding with one on a new path generation.
  FlowBinding rebound{};
  rebound.flow = h.flow;
  rebound.resource = pacing::ResourceRef{h.resource, Generation::from(1)};
  rebound.path = pacing::PathRef{h.path, Generation::from(2)};
  PF_CHECK_OK(h.fabric->bind_flow(rebound));
  auto attempt = h.fabric->apply_envelope(envelope.ref, h.backend_id);
  // The governing policy was authored for the previous path generation, so the
  // fabric reports the precise drift rather than a generic staleness.
  PF_CHECK_CODE(attempt, ErrorCode::StalePath);
  // No attempt is created for a rejected authorization request.
  PF_CHECK(h.fabric->attempts_for_flow(h.flow).empty());
}

PF_TEST(lifecycle, missing_authority_refuses_derivation) {
  Harness h;
  h.start();
  h.bind_flow(h.flow);
  h.publish_policy(1'000'000'000ull);
  h.register_backend();
  // No rate grant installed: the fabric has no entitlement to pace from.
  auto envelope = h.fabric->derive_envelope(h.flow);
  PF_CHECK_CODE(envelope, ErrorCode::UnknownAuthority);
  PF_CHECK_EQ(h.fabric->stats().envelopes_derived, 0ull);
  PF_CHECK(h.fabric->stats().envelopes_refused >= 1);
}

PF_TEST(lifecycle, non_authoritative_grant_never_authorizes) {
  Harness h;
  h.start();
  h.bind_flow(h.flow);
  h.publish_policy(1'000'000'000ull);
  h.register_backend();
  RateGrant grant{};
  grant.ref = pacing::GrantRef{h.grant, Generation::from(1)};
  grant.resource = h.resource;
  grant.ceiling_bps = 10'000'000'000ull;
  grant.authoritative = false;  // present but not authoritative
  h.upstream.install(h.resource, grant);
  auto envelope = h.fabric->derive_envelope(h.flow);
  PF_CHECK_CODE(envelope, ErrorCode::UnknownAuthority);
}

PF_TEST(lifecycle, expired_grant_is_stale) {
  Harness h;
  h.start();
  h.bind_flow(h.flow);
  h.publish_policy(1'000'000'000ull);
  h.register_backend();
  RateGrant grant{};
  grant.ref = pacing::GrantRef{h.grant, Generation::from(1)};
  grant.resource = h.resource;
  grant.ceiling_bps = 10'000'000'000ull;
  grant.authoritative = true;
  grant.valid_until = Deadline::at(Instant::from_ns(h.clock.now().ns() + 1'000));
  h.upstream.install(h.resource, grant);
  h.clock.advance(2'000);
  auto envelope = h.fabric->derive_envelope(h.flow);
  PF_CHECK_CODE(envelope, ErrorCode::StaleRateGrant);
}

PF_TEST(lifecycle, quiesced_binding_authorizes_nothing) {
  Harness h; make_ready(h);
  PF_CHECK_STATUS_OK(h.fabric->binding(h.flow));
  PF_CHECK_OK(h.fabric->unbind_flow(h.flow));
  auto envelope = h.fabric->derive_envelope(h.flow);
  PF_CHECK_CODE(envelope, ErrorCode::NotFound);
}

PF_TEST(lifecycle, ceiling_is_never_exceeded_by_derivation) {
  Harness h;
  h.start();
  h.install_grant(1'000'000'000ull);  // 1 Gbps authorized
  h.bind_flow(h.flow);
  h.publish_policy(100'000'000'000ull);  // policy requests 100 Gbps
  h.register_backend();
  const PacingEnvelope envelope = h.derive(h.flow);
  PF_CHECK_EQ(envelope.cadence.rate_bps, 1'000'000'000ull);
  PF_CHECK(envelope.clamped_to_ceiling);
  PF_CHECK_EQ(envelope.requested_rate_bps, 100'000'000'000ull);
  u64 effective = 0;
  PF_CHECK(envelope.cadence.effective_rate_bps(effective));
  PF_CHECK(effective <= 1'000'000'000ull);
}

PF_TEST(lifecycle, policy_floor_above_ceiling_is_refused_not_clamped) {
  Harness h;
  h.start();
  h.install_grant(1'000'000'000ull);
  h.bind_flow(h.flow);
  h.publish_policy(0, 1'000'000ull, 5'000'000'000ull);  // floor of 5 Gbps over a 1 Gbps ceiling
  h.register_backend();
  auto envelope = h.fabric->derive_envelope(h.flow);
  PF_CHECK_CODE(envelope, ErrorCode::EntitlementMint);
}

PF_TEST(lifecycle, service_class_exception_raises_floor_within_ceiling) {
  Harness h;
  h.start();
  h.install_grant(10'000'000'000ull);
  h.bind_flow(h.flow, 0, 0, ServiceClassId::from(5));
  h.publish_policy(1'000'000'000ull, 1'000'000ull, 0, 0, 1500, 1'000'000ull, CadenceShape::Uniform, 0,
                   4'000'000'000ull);
  h.register_backend();
  const PacingEnvelope envelope = h.derive(h.flow);
  PF_CHECK_EQ(envelope.cadence.rate_bps, 4'000'000'000ull);
  PF_CHECK(envelope.clamped_to_floor);
  PF_CHECK(envelope.cadence.rate_bps <= envelope.ceiling_bps);
}

PF_TEST(lifecycle, verify_upgrades_unverified_to_applied) {
  LoopbackBackend::Options options{};
  options.drop_readback = true;
  Harness h; make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const AttemptId id = h.apply(h.derive(h.flow));
  PF_CHECK_EQ(h.record(id).state, ApplicationState::AppliedUnverified);

  LoopbackBackend::Options good{};
  h.backend->set_options(good);
  PF_CHECK_OK(h.fabric->verify_attempt(id));
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Applied);
  PF_CHECK_EQ(h.record(id).effect, EffectLabel::SyntheticVerified);
}

PF_TEST(lifecycle, verify_marks_stale_when_authority_moves) {
  Harness h; make_ready(h);
  const AttemptId id = h.apply(h.derive(h.flow));
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Applied);
  h.install_grant(10'000'000'000ull, 0, 9, 9);
  auto verify = h.fabric->verify_attempt(id);
  PF_CHECK_STATUS_CODE(verify, ErrorCode::StaleRateGrant);
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Stale);
  PF_CHECK(!h.record(id).claims_effect());
}

PF_TEST(lifecycle, real_backend_label_is_propagated_but_not_claimed_as_physical) {
  // Label propagation only. No physical pacing device exists in this test, so
  // the descriptor is a synthetic double that merely declares REAL; the point
  // of the test is that the label travels into the evidence unchanged.
  LoopbackBackend::Options options{};
  options.nature = BackendNature::Real;
  options.name = "declared-real-test-double";
  Harness h; make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const AttemptId id = h.apply(h.derive(h.flow));
  const ApplicationRecord rec = h.record(id);
  PF_CHECK_EQ(rec.state, ApplicationState::Applied);
  PF_CHECK_EQ(rec.effect, EffectLabel::Verified);
  PF_CHECK_EQ(rec.evidence_kind, EvidenceKind::BackendReadback);
  PF_CHECK(!rec.backend_synthetic);
  const auto descriptors = h.fabric->list_backends();
  PF_CHECK_EQ(descriptors.size(), std::size_t{1});
  PF_CHECK_EQ(descriptors[0].nature, BackendNature::Real);
}

PF_TEST(lifecycle, backend_registration_bounds_are_enforced) {
  Harness h; make_ready(h);
  auto unknown = h.fabric->apply_envelope(h.derive(h.flow).ref, pacing::BackendId::from(77));
  PF_CHECK_CODE(unknown, ErrorCode::NotFound);
  PF_CHECK_STATUS_CODE(h.fabric->unregister_backend(pacing::BackendId::from(77)), ErrorCode::NotFound);
  PF_CHECK_STATUS_CODE(h.fabric->register_backend(nullptr), ErrorCode::InvalidArgument);

  // A backend name longer than the configured bound is refused rather than
  // truncated, which requires the bound to be in place before construction.
  pacing::Limits tight{};
  tight.max_name_bytes = 4;
  Harness bounded{{}, 7, HarnessOptions{tight}};
  bounded.start();
  bounded.install_grant();
  bounded.bind_flow(bounded.flow);
  bounded.publish_policy(1'000'000'000ull);
  auto long_name = std::make_shared<LoopbackBackend>(
      pacing::BackendRef{pacing::BackendId::from(5), Generation::from(1)}, LoopbackBackend::Options{});
  PF_CHECK_STATUS_CODE(bounded.fabric->register_backend(long_name), ErrorCode::Oversized);
  PF_CHECK(bounded.fabric->list_backends().empty());
}

PF_TEST(lifecycle, statistics_track_work) {
  Harness h; make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  (void)h.apply(envelope);
  (void)h.apply(envelope);
  const FabricStats stats = h.fabric->stats();
  PF_CHECK_EQ(stats.envelopes_derived, 1ull);
  PF_CHECK_EQ(stats.applies_requested, 2ull);
  PF_CHECK_EQ(stats.applies_reserved, 1ull);
  PF_CHECK_EQ(stats.applies_committed, 1ull);
  PF_CHECK_EQ(stats.duplicate_applies, 1ull);
}

PF_TEST(lifecycle, shutdown_stops_accepting_work) {
  Harness h; make_ready(h);
  PF_CHECK(h.fabric->running());
  PF_CHECK_OK(h.fabric->shutdown());
  PF_CHECK(!h.fabric->running());
  auto envelope = h.fabric->derive_envelope(h.flow);
  PF_CHECK_CODE(envelope, ErrorCode::InvalidState);
  PF_CHECK_OK(h.fabric->shutdown());
}
