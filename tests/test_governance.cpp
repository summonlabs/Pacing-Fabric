// Governance tests: cancellation, revocation, fencing, revalidation,
// explanation and event emission. Includes a deliberate lock-reentrancy probe.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <thread>

#include "framework.hpp"
#include "harness.hpp"

using namespace pacing;
using pf_test::Harness;
using pf_test::LoopbackBackend;
using pf_test::RecordingSink;
using pf_test::TempDir;

namespace {

void make_ready(Harness& h, std::uint64_t ceiling = 10'000'000'000ull,
                std::uint64_t rate = 1'000'000'000ull, LoopbackBackend::Options options = {}) {
  h.start();
  h.install_grant(ceiling);
  h.bind_flow(h.flow);
  h.publish_policy(rate);
  h.register_backend(std::move(options));
}

// A sink that calls back into the fabric. If the fabric emitted events while
// holding its state lock this would deadlock rather than fail, which is exactly
// the defect this probe exists to catch.
class ReentrantSink final : public IEventSink {
 public:
  explicit ReentrantSink(PacingFabric* fabric) : fabric_(fabric) {}

  void on_event(const FabricEvent& event) noexcept override {
    ++calls_;
    if (fabric_ != nullptr) {
      (void)fabric_->stats();
      (void)fabric_->epoch();
      (void)fabric_->running();
    }
    last_kind_ = event.kind;
  }

  [[nodiscard]] std::uint64_t calls() const noexcept { return calls_; }
  [[nodiscard]] EventKind last_kind() const noexcept { return last_kind_; }

 private:
  PacingFabric* fabric_;
  std::uint64_t calls_{0};
  EventKind last_kind_{EventKind::EnvelopeDerived};
};

}  // namespace

PF_TEST(governance, cancel_before_backend_completion_prevents_commit) {
  LoopbackBackend::Options options{};
  options.stall_apply = true;
  Harness h;
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const PacingEnvelope envelope = h.derive(h.flow);

  StatusOr<AttemptId> result = Status::of(ErrorCode::Internal, "not run");
  std::thread worker([&] { result = h.fabric->apply_envelope(envelope.ref, h.backend_id); });

  h.backend->wait_until_stalled();
  // The ledger must show the attempt as reserved-but-not-committed.
  const auto in_flight = h.fabric->attempts_for_flow(h.flow);
  PF_CHECK_EQ(in_flight.size(), std::size_t{1});
  const AttemptId id = in_flight[0].attempt;
  PF_CHECK(!in_flight[0].claims_effect());

  PF_CHECK_OK(h.fabric->cancel_attempt(id, "operator cancelled during apply"));
  h.backend->release_stall();
  worker.join();

  // The completion arrived after the attempt was closed, so it must have been
  // discarded rather than committed.
  PF_CHECK_CODE(result, ErrorCode::Cancelled);
  const ApplicationRecord rec = h.record(id);
  PF_CHECK_EQ(rec.state, ApplicationState::Cancelled);
  PF_CHECK(!rec.claims_effect());
  PF_CHECK(h.backend->revokes() >= 1);
  PF_CHECK(!h.backend->installed(id));
  PF_CHECK_EQ(h.fabric->stats().late_completions_discarded, 1ull);
  PF_CHECK_EQ(h.fabric->stats().applies_committed, 0ull);
}

PF_TEST(governance, cancelled_attempt_is_not_resurrected_by_reapply) {
  LoopbackBackend::Options options{};
  options.stall_apply = true;
  Harness h;
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const PacingEnvelope envelope = h.derive(h.flow);

  StatusOr<AttemptId> result = Status::of(ErrorCode::Internal, "not run");
  std::thread worker([&] { result = h.fabric->apply_envelope(envelope.ref, h.backend_id); });
  h.backend->wait_until_stalled();
  const AttemptId id = h.fabric->attempts_for_flow(h.flow)[0].attempt;
  PF_CHECK_OK(h.fabric->cancel_attempt(id, "cancel"));
  h.backend->release_stall();
  worker.join();

  LoopbackBackend::Options healthy{};
  h.backend->set_options(healthy);
  auto retry = h.fabric->apply_envelope(envelope.ref, h.backend_id);
  PF_CHECK_STATUS_OK(retry);
  PF_CHECK_EQ(retry.value().value(), id.value());
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Cancelled);
  PF_CHECK_EQ(h.backend->apply_entries(), 1ull);
}

PF_TEST(governance, cancel_after_apply_revokes_and_withdraws) {
  Harness h;
  make_ready(h);
  const AttemptId id = h.apply(h.derive(h.flow));
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Applied);
  PF_CHECK(h.backend->installed(id));

  PF_CHECK_OK(h.fabric->cancel_attempt(id, "operator revoked"));
  const ApplicationRecord rec = h.record(id);
  PF_CHECK_EQ(rec.state, ApplicationState::Revoked);
  PF_CHECK_EQ(rec.effect, EffectLabel::None);
  PF_CHECK(!h.backend->installed(id));
  PF_CHECK_EQ(h.fabric->stats().revocations, 1ull);
}

PF_TEST(governance, cancel_is_idempotent_on_closed_attempts) {
  Harness h;
  make_ready(h);
  const AttemptId id = h.apply(h.derive(h.flow));
  PF_CHECK_OK(h.fabric->cancel_attempt(id, "first"));
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Revoked);
  PF_CHECK_OK(h.fabric->cancel_attempt(id, "second"));
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Revoked);
  PF_CHECK_EQ(h.fabric->stats().revocations, 1ull);
  PF_CHECK_STATUS_CODE(h.fabric->cancel_attempt(AttemptId::from(9999), "x"), ErrorCode::NotFound);
}

PF_TEST(governance, cancel_rejects_oversized_reason) {
  Harness h;
  make_ready(h);
  const AttemptId id = h.apply(h.derive(h.flow));
  const std::string huge(h.limits.max_explanation_bytes + 1, 'x');
  PF_CHECK_STATUS_CODE(h.fabric->cancel_attempt(id, huge), ErrorCode::Oversized);
}

PF_TEST(governance, revoke_envelope_withdraws_every_applied_attempt) {
  Harness h;
  make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  const AttemptId id = h.apply(envelope);
  PF_CHECK(h.backend->installed(id));

  PF_CHECK_OK(h.fabric->revoke_envelope(envelope.ref, "policy withdrawn"));
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Revoked);
  PF_CHECK(!h.backend->installed(id));

  auto retry = h.fabric->apply_envelope(envelope.ref, h.backend_id);
  PF_CHECK_CODE(retry, ErrorCode::Revoked);
  PF_CHECK_EQ(h.backend->apply_entries(), 1ull);
}

PF_TEST(governance, revoke_envelope_checks_generation) {
  Harness h;
  make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  EnvelopeRef wrong = envelope.ref;
  wrong.generation = Generation::from(99);
  PF_CHECK_STATUS_CODE(h.fabric->revoke_envelope(wrong, "wrong generation"), ErrorCode::StaleGeneration);
  PF_CHECK_STATUS_CODE(h.fabric->revoke_envelope(EnvelopeRef{}, "no id"), ErrorCode::InvalidArgument);
  PF_CHECK_STATUS_CODE(h.fabric->revoke_envelope(EnvelopeRef{EnvelopeId::from(1234), Generation::from(1)},
                                                 "missing"),
                       ErrorCode::NotFound);
}

PF_TEST(governance, revoke_twice_is_idempotent) {
  Harness h;
  make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  (void)h.apply(envelope);
  PF_CHECK_OK(h.fabric->revoke_envelope(envelope.ref, "first"));
  PF_CHECK_OK(h.fabric->revoke_envelope(envelope.ref, "second"));
  PF_CHECK_EQ(h.record(h.fabric->attempts_for_flow(h.flow)[0].attempt).state, ApplicationState::Revoked);
}

PF_TEST(governance, fence_below_epoch_invalidates_older_records) {
  Harness h;
  make_ready(h);
  const AttemptId id = h.apply(h.derive(h.flow));
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Applied);
  PF_CHECK_OK(h.fabric->fence_below(Epoch::from(h.fabric->epoch().value() + 1)));
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Fenced);
  PF_CHECK(!h.record(id).claims_effect());
  PF_CHECK_STATUS_CODE(h.fabric->fence_below(Epoch::none()), ErrorCode::InvalidArgument);
}

PF_TEST(governance, advance_epoch_revokes_envelopes_and_fences_effects) {
  Harness h;
  make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  const AttemptId id = h.apply(envelope);
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Applied);
  PF_CHECK(h.backend->installed(id));

  const Epoch before = h.fabric->epoch();
  PF_CHECK_OK(h.fabric->advance_epoch());
  PF_CHECK_EQ(h.fabric->epoch().value(), before.value() + 1);

  // Everything authorised under the previous epoch is invalid, and the backend
  // has been told to discard that epoch's state.
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Fenced);
  PF_CHECK(!h.backend->installed(id));
  auto retry = h.fabric->apply_envelope(envelope.ref, h.backend_id);
  PF_CHECK_CODE(retry, ErrorCode::Revoked);

  // A fresh derivation under the new epoch works normally.
  const PacingEnvelope renewed = h.derive(h.flow);
  PF_CHECK_EQ(renewed.authority.epoch.value(), before.value() + 1);
  const AttemptId renewed_id = h.apply(renewed);
  PF_CHECK_EQ(h.record(renewed_id).state, ApplicationState::Applied);
}

PF_TEST(governance, revalidate_requires_positive_readback) {
  LoopbackBackend::Options options{};
  options.drop_readback = true;
  Harness h;
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const AttemptId id = h.apply(h.derive(h.flow));
  PF_CHECK_EQ(h.record(id).state, ApplicationState::AppliedUnverified);

  PF_CHECK_STATUS_CODE(h.fabric->revalidate(id), ErrorCode::RequiresRevalidation);
  PF_CHECK(h.record(id).requires_revalidation);
  PF_CHECK(!h.record(id).claims_effect());

  LoopbackBackend::Options healthy{};
  h.backend->set_options(healthy);
  PF_CHECK_OK(h.fabric->revalidate(id));
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Applied);
  PF_CHECK(!h.record(id).requires_revalidation);
}

PF_TEST(governance, revalidate_rejects_authority_drift) {
  Harness h;
  make_ready(h);
  const AttemptId id = h.apply(h.derive(h.flow));
  h.install_grant(10'000'000'000ull, 0, 5, 5);
  PF_CHECK_STATUS_CODE(h.fabric->revalidate(id), ErrorCode::StaleRateGrant);
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Stale);
  PF_CHECK(!h.record(id).claims_effect());
  PF_CHECK_STATUS_CODE(h.fabric->revalidate(AttemptId::from(123456)), ErrorCode::NotFound);
}

PF_TEST(governance, revalidate_rejects_closed_attempts) {
  Harness h;
  make_ready(h);
  const AttemptId id = h.apply(h.derive(h.flow));
  PF_CHECK_OK(h.fabric->cancel_attempt(id, "cancel"));
  PF_CHECK_STATUS_CODE(h.fabric->revalidate(id), ErrorCode::InvalidState);
}

PF_TEST(governance, explain_reports_cadence_authority_and_effect) {
  Harness h;
  make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  const AttemptId id = h.apply(envelope);

  auto explanation = h.fabric->explain(h.flow);
  PF_CHECK_STATUS_OK(explanation);
  const Explanation& ex = explanation.value();
  PF_CHECK(ex.has_binding);
  PF_CHECK(ex.has_envelope);
  PF_CHECK(ex.has_attempt);
  PF_CHECK(ex.has_grant);
  PF_CHECK_EQ(ex.drift, AuthorityDrift::None);
  PF_CHECK_EQ(ex.effect, EffectLabel::SyntheticVerified);
  PF_CHECK_EQ(ex.refusal, ErrorCode::Ok);
  PF_CHECK_EQ(ex.envelope.cadence.rate_bps, envelope.cadence.rate_bps);
  PF_CHECK_EQ(ex.attempt.attempt.value(), id.value());
  PF_CHECK_EQ(ex.burst_budget_bytes, envelope.cadence.burst_bytes);
  PF_CHECK_EQ(ex.grant.ceiling_bps, envelope.ceiling_bps);

  const std::string text = ex.to_text(h.limits.max_explanation_bytes);
  PF_CHECK(text.find("pacing explanation") != std::string::npos);
  PF_CHECK(text.find("cadence") != std::string::npos ||
           text.find("interval_ns") != std::string::npos);
  const std::string json = ex.to_json(h.limits.max_explanation_bytes);
  PF_CHECK(json.front() == '{');
  PF_CHECK(json.back() == '}');
  PF_CHECK(json.size() <= h.limits.max_explanation_bytes);
}

PF_TEST(governance, explain_surfaces_drift_and_refusal) {
  Harness h;
  make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  (void)h.apply(envelope);
  h.install_grant(10'000'000'000ull, 0, 3, 3);
  auto explanation = h.fabric->explain(h.flow);
  PF_CHECK_STATUS_OK(explanation);
  PF_CHECK_EQ(explanation.value().drift, AuthorityDrift::RateGrant);
  PF_CHECK_EQ(explanation.value().refusal, ErrorCode::StaleRateGrant);
  PF_CHECK_EQ(explanation.value().effect, EffectLabel::None);
  PF_CHECK(!explanation.value().stale_or_revoke_reason.empty());
}

PF_TEST(governance, explain_reports_unknown_for_unbound_flow) {
  Harness h;
  make_ready(h);
  auto explanation = h.fabric->explain(FlowId::from(999));
  PF_CHECK_STATUS_OK(explanation);
  PF_CHECK(!explanation.value().has_binding);
  PF_CHECK(!explanation.value().has_envelope);
  PF_CHECK_EQ(explanation.value().effect, EffectLabel::None);
  PF_CHECK_EQ(explanation.value().refusal, ErrorCode::NotFound);
  PF_CHECK_CODE(h.fabric->explain(FlowId{}), ErrorCode::InvalidArgument);
}

PF_TEST(governance, explanation_rendering_is_bounded) {
  Harness h;
  make_ready(h);
  (void)h.apply(h.derive(h.flow));
  auto explanation = h.fabric->explain(h.flow);
  PF_CHECK_STATUS_OK(explanation);
  for (std::size_t limit : {std::size_t{16}, std::size_t{64}, std::size_t{256}, std::size_t{4096}}) {
    const std::string text = explanation.value().to_text(limit);
    PF_CHECK(text.size() <= limit);
    const std::string json = explanation.value().to_json(limit);
    PF_CHECK(json.size() <= limit);
  }
  PF_CHECK_EQ(bound_explanation("abcdef", 0).size(), std::size_t{0});
  PF_CHECK(bound_explanation("abcdef", 3).size() <= 3);
}

PF_TEST(governance, events_are_emitted_outside_the_state_lock) {
  Harness h;
  make_ready(h);
  ReentrantSink sink(h.fabric.get());
  h.fabric->set_event_sink(&sink);
  const AttemptId id = h.apply(h.derive(h.flow));
  PF_CHECK(sink.calls() >= 3);  // reserved, acknowledged/verified
  PF_CHECK_EQ(id.value() > 0, true);
  h.fabric->set_event_sink(nullptr);
}

PF_TEST(governance, event_kinds_track_the_lifecycle) {
  Harness h;
  make_ready(h);
  RecordingSink sink;
  h.fabric->set_event_sink(&sink);
  const PacingEnvelope envelope = h.derive(h.flow);
  const AttemptId id = h.apply(envelope);
  PF_CHECK_OK(h.fabric->cancel_attempt(id, "done"));
  h.fabric->set_event_sink(nullptr);
  PF_CHECK_EQ(sink.count(EventKind::EnvelopeDerived), std::size_t{1});
  PF_CHECK_EQ(sink.count(EventKind::ApplyReserved), std::size_t{1});
  PF_CHECK_EQ(sink.count(EventKind::ApplyVerified), std::size_t{1});
  PF_CHECK_EQ(sink.count(EventKind::AttemptCancelled), std::size_t{1});
  PF_CHECK(h.fabric->stats().events_emitted >= 4);
}

PF_TEST(governance, shutdown_waits_for_in_flight_apply) {
  LoopbackBackend::Options options{};
  options.stall_apply = true;
  Harness h;
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const PacingEnvelope envelope = h.derive(h.flow);

  StatusOr<AttemptId> result = Status::of(ErrorCode::Internal, "not run");
  Status shutdown_status = Status::of(ErrorCode::Internal, "not run");
  // jthread joins on scope exit, so a failing assertion below cannot turn into
  // a std::terminate from a joinable thread.
  {
    std::jthread worker([&] { result = h.fabric->apply_envelope(envelope.ref, h.backend_id); });
    h.backend->wait_until_stalled();
    std::jthread stopper([&] { shutdown_status = h.fabric->shutdown(); });
    // Release only after shutdown has been requested: the apply that is already
    // in flight must still be allowed to cross its completion boundary, and
    // shutdown must not close the ledger underneath it.
    h.backend->release_stall();
  }

  PF_CHECK_STATUS_OK(result);
  PF_CHECK_OK(shutdown_status);
  PF_CHECK(!h.fabric->running());
  const ApplicationRecord rec = h.record(result.value());
  PF_CHECK_EQ(rec.state, ApplicationState::Applied);
  PF_CHECK_EQ(rec.effect, EffectLabel::SyntheticVerified);
}

PF_TEST(governance, work_after_shutdown_is_refused) {
  Harness h;
  make_ready(h);
  const AttemptId id = h.apply(h.derive(h.flow));
  PF_CHECK_OK(h.fabric->shutdown());
  PF_CHECK_STATUS_CODE(h.fabric->bind_flow(pacing::FlowBinding{}), ErrorCode::InvalidState);
  PF_CHECK_CODE(h.fabric->publish_policy(PacingPolicy{}), ErrorCode::InvalidState);
  PF_CHECK_STATUS_CODE(h.fabric->cancel_attempt(id, "x"), ErrorCode::InvalidState);
  PF_CHECK_STATUS_CODE(h.fabric->verify_attempt(id), ErrorCode::InvalidState);
  PF_CHECK_STATUS_CODE(h.fabric->advance_epoch(), ErrorCode::InvalidState);
  PF_CHECK_STATUS_CODE(h.fabric->fence_below(Epoch::from(1)), ErrorCode::InvalidState);
  PF_CHECK_STATUS_CODE(h.fabric->register_backend(nullptr), ErrorCode::InvalidArgument);
}
