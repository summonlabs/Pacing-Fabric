// Hardening pass: scenarios that try to break the fabric after first green.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <limits>
#include <thread>
#include <vector>

#include "framework.hpp"
#include "harness.hpp"

using namespace pacing;
using pf_test::Harness;
using pf_test::HarnessOptions;
using pf_test::LoopbackBackend;
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

}  // namespace

PF_TEST(hardening, backend_acknowledging_a_different_attempt_is_a_failure) {
  LoopbackBackend::Options options{};
  options.echo_wrong_attempt = true;
  Harness h;
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const AttemptId id = h.apply(h.derive(h.flow));
  const ApplicationRecord record = h.record(id);
  PF_CHECK_EQ(record.state, ApplicationState::Failed);
  PF_CHECK_EQ(record.reason, ErrorCode::BackendFailure);
  PF_CHECK(!record.claims_effect());
}

PF_TEST(hardening, readback_for_a_different_attempt_is_a_mismatch) {
  LoopbackBackend::Options options{};
  options.readback_wrong_attempt = true;
  Harness h;
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const AttemptId id = h.apply(h.derive(h.flow));
  const ApplicationRecord record = h.record(id);
  PF_CHECK_EQ(record.state, ApplicationState::Mismatched);
  PF_CHECK_EQ(record.reason, ErrorCode::ReadbackMismatch);
  PF_CHECK(!record.claims_effect());
}

PF_TEST(hardening, grant_change_during_apply_prevents_the_effect_from_committing) {
  LoopbackBackend::Options options{};
  options.stall_apply = true;
  Harness h;
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const PacingEnvelope envelope = h.derive(h.flow);

  StatusOr<AttemptId> result = Status::of(ErrorCode::Internal, "not run");
  {
    std::jthread worker([&] { result = h.fabric->apply_envelope(envelope.ref, h.backend_id); });
    h.backend->wait_until_stalled();
    // The upstream grant moves while the backend is still working.
    h.install_grant(10'000'000'000ull, 0, 9, 9);
    h.backend->release_stall();
  }

  PF_CHECK_CODE(result, ErrorCode::StaleRateGrant);
  const auto records = h.fabric->attempts_for_flow(h.flow);
  PF_CHECK_EQ(records.size(), std::size_t{1});
  PF_CHECK_EQ(records[0].state, ApplicationState::Stale);
  PF_CHECK(!records[0].claims_effect());
  // The pacing the backend may have installed must have been withdrawn.
  PF_CHECK(!h.backend->installed(records[0].attempt));
  PF_CHECK(h.backend->revokes() >= 1);
  PF_CHECK_EQ(h.fabric->stats().applies_committed, 0ull);
}

PF_TEST(hardening, policy_change_during_apply_prevents_the_effect_from_committing) {
  LoopbackBackend::Options options{};
  options.supports_readback = false;  // exercise the unverified path too
  options.stall_apply = true;
  Harness h;
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const PacingEnvelope envelope = h.derive(h.flow);

  StatusOr<AttemptId> result = Status::of(ErrorCode::Internal, "not run");
  {
    std::jthread worker([&] { result = h.fabric->apply_envelope(envelope.ref, h.backend_id); });
    h.backend->wait_until_stalled();
    h.publish_policy(2'000'000'000ull);
    h.backend->release_stall();
  }

  const auto records = h.fabric->attempts_for_flow(h.flow);
  PF_CHECK_EQ(records.size(), std::size_t{1});
  PF_CHECK_EQ(records[0].state, ApplicationState::Stale);
  PF_CHECK_EQ(records[0].reason, ErrorCode::StalePolicy);
  PF_CHECK(!records[0].claims_effect());
  PF_CHECK(!h.backend->installed(records[0].attempt));
}

PF_TEST(hardening, grant_withdrawing_during_apply_prevents_the_effect_from_committing) {
  LoopbackBackend::Options options{};
  options.stall_apply = true;
  Harness h;
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const PacingEnvelope envelope = h.derive(h.flow);

  StatusOr<AttemptId> result = Status::of(ErrorCode::Internal, "not run");
  {
    std::jthread worker([&] { result = h.fabric->apply_envelope(envelope.ref, h.backend_id); });
    h.backend->wait_until_stalled();
    h.upstream.set_failure(h.resource, ErrorCode::BackendUnavailable);
    h.backend->release_stall();
  }

  PF_CHECK_CODE(result, ErrorCode::BackendUnavailable);
  const auto records = h.fabric->attempts_for_flow(h.flow);
  PF_CHECK_EQ(records[0].state, ApplicationState::Stale);
  PF_CHECK(!h.backend->installed(records[0].attempt));
}

PF_TEST(hardening, unregistered_backend_mid_apply_does_not_commit_an_effect) {
  LoopbackBackend::Options options{};
  options.stall_apply = true;
  Harness h;
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const PacingEnvelope envelope = h.derive(h.flow);

  StatusOr<AttemptId> result = Status::of(ErrorCode::Internal, "not run");
  {
    std::jthread worker([&] { result = h.fabric->apply_envelope(envelope.ref, h.backend_id); });
    h.backend->wait_until_stalled();
    PF_CHECK_OK(h.fabric->unregister_backend(h.backend_id));
    h.backend->release_stall();
  }
  // The coordinator no longer governs that backend, so an effect installed on
  // it could never be revoked or rebound. The completion is discarded and the
  // withdrawal reaches the handle the attempt already held.
  PF_CHECK_CODE(result, ErrorCode::BackendUnavailable);
  const auto records = h.fabric->attempts_for_flow(h.flow);
  PF_CHECK_EQ(records.size(), std::size_t{1});
  PF_CHECK_EQ(records[0].state, ApplicationState::Failed);
  PF_CHECK(!records[0].claims_effect());
  PF_CHECK(!h.backend->installed(records[0].attempt));
  PF_CHECK_EQ(h.fabric->list_backends().size(), std::size_t{0});
}

PF_TEST(hardening, attempt_ledger_exhaustion_is_reported_not_silently_ignored) {
  Limits limits{};
  limits.max_attempts = 4;
  HarnessOptions options{};
  options.limits = limits;
  Harness h{{}, 7, options};
  make_ready(h);

  std::size_t refused = 0;
  for (int i = 0; i < 12; ++i) {
    auto envelope = h.fabric->derive_envelope(h.flow);
    PF_CHECK_STATUS_OK(envelope);
    auto attempt = h.fabric->apply_envelope(envelope.value().ref, h.backend_id);
    if (!attempt) {
      PF_CHECK_EQ(attempt.code(), ErrorCode::ResourceExhausted);
      ++refused;
    }
  }
  PF_CHECK(refused > 0);
  PF_CHECK(h.fabric->attempts_for_flow(h.flow).size() <= limits.max_attempts);
  PF_CHECK_EQ(h.fabric->stats().applies_committed, 12ull - refused);
}

PF_TEST(hardening, envelope_table_capacity_evicts_only_closed_envelopes) {
  Limits limits{};
  limits.max_envelopes = 8;
  LoopbackBackend::Options backend_options{};
  backend_options.stall_apply = true;
  HarnessOptions options{};
  options.limits = limits;
  Harness h{{}, 7, options};
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, backend_options);

  // One envelope is stuck in flight; the table must still be able to serve
  // closed envelopes and must never evict the in-flight one.
  const PacingEnvelope in_flight = h.derive(h.flow);
  StatusOr<AttemptId> pending = Status::of(ErrorCode::Internal, "not run");
  std::jthread worker([&] { pending = h.fabric->apply_envelope(in_flight.ref, h.backend_id); });
  h.backend->wait_until_stalled();

  std::size_t derived = 1;
  for (int i = 0; i < 40; ++i) {
    auto envelope = h.fabric->derive_envelope(h.flow);
    if (!envelope) {
      PF_CHECK_EQ(envelope.code(), ErrorCode::ResourceExhausted);
      break;
    }
    ++derived;
  }
  // The in-flight envelope is still resolvable while it matters.
  PF_CHECK_STATUS_OK(h.fabric->envelope(in_flight.ref.id));
  h.backend->release_stall();
  worker.join();
  PF_CHECK_STATUS_OK(pending);
  PF_CHECK(derived > 1);
}

PF_TEST(hardening, journal_growth_stays_bounded_under_pressure) {
  TempDir dir("journal_bound");
  Limits limits{};
  limits.max_journal_records = 24;
  limits.max_journal_bytes = 64u << 10;
  limits.max_durable_record_bytes = 1u << 10;
  HarnessOptions options{};
  options.limits = limits;
  Harness h{dir.path(), 7, options};
  make_ready(h);

  std::size_t derived = 0;
  std::size_t applied = 0;
  for (int i = 0; i < 200; ++i) {
    auto envelope = h.fabric->derive_envelope(h.flow);
    if (!envelope) {
      PF_CHECK(envelope.code() == ErrorCode::ResourceExhausted ||
               envelope.code() == ErrorCode::IoFailure);
      break;
    }
    ++derived;
    auto attempt = h.fabric->apply_envelope(envelope.value().ref, h.backend_id);
    if (!attempt) {
      PF_CHECK(attempt.code() == ErrorCode::ResourceExhausted ||
               attempt.code() == ErrorCode::IoFailure);
      break;
    }
    ++applied;
  }
  PF_CHECK(derived > 0);
  PF_CHECK(applied <= derived);
  // Growth is bounded by compaction: the journal reaches its record bound and
  // is then compacted, so the counter never runs away.
  PF_CHECK(h.fabric->stats().checkpoints > 0);
  PF_CHECK(h.fabric->durability_report().journal_records <= limits.max_journal_records);
  // Every applied attempt is still evidenced.
  PF_CHECK_EQ(h.fabric->stats().applies_committed, applied);
}

PF_TEST(hardening, a_near_maximum_instant_is_refused_rather_than_wrapped) {
  // The tick index is a wrapping counter, but it is derived from an absolute
  // nanosecond instant. Near the top of the representable range an envelope
  // expiry could wrap, so the fabric refuses instead of issuing an envelope
  // whose lifetime is nonsense.
  HarnessOptions options{};
  options.tick_period_ns = 1;  // tick index equals the nanosecond instant
  options.envelope_lifetime_ns = 1'000'000'000ull;
  Harness h{{}, 7, options};
  make_ready(h);

  h.clock.set(std::numeric_limits<u64>::max() - 4);
  auto overflowed = h.fabric->derive_envelope(h.flow);
  PF_CHECK_CODE(overflowed, ErrorCode::ArithmeticOverflow);
  // The refusal is not a clock regression: time really is that large.
  PF_CHECK(!h.fabric->observed_clock_regression());

  // A separate coordinator starts at an instant that still has room for the
  // lifetime, and paces normally with the tick counter as a plain function of
  // the instant.
  Harness high{{}, 8, options};
  make_ready(high);
  high.clock.set(std::numeric_limits<u64>::max() - 10'000'000'000ull);
  const Tick tick = high.fabric->current_tick();
  PF_CHECK_EQ(tick.index(), std::numeric_limits<u64>::max() - 10'000'000'000ull);
  const PacingEnvelope envelope = high.derive(high.flow);
  PF_CHECK(envelope.valid_until.is_set());
  const AttemptId id = high.apply(envelope);
  PF_CHECK_EQ(high.record(id).state, ApplicationState::Applied);
}

PF_TEST(hardening, clock_regression_refuses_further_authorization) {
  Harness h;
  make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  const AttemptId id = h.apply(envelope);
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Applied);

  // A clock that jumps backwards makes time untrustworthy, so no further
  // authorization is derived: an expired envelope must not be resurrected by a
  // clock that moved the wrong way.
  h.clock.force_regression(1);
  auto envelope_after = h.fabric->derive_envelope(h.flow);
  PF_CHECK_CODE(envelope_after, ErrorCode::ClockRegression);
  auto attempt_after = h.fabric->apply_envelope(envelope.ref, h.backend_id);
  PF_CHECK_CODE(attempt_after, ErrorCode::ClockRegression);
  PF_CHECK_STATUS_CODE(h.fabric->verify_attempt(id), ErrorCode::ClockRegression);
  PF_CHECK_STATUS_CODE(h.fabric->revalidate(id), ErrorCode::ClockRegression);

  // The ledger is unaffected: the earlier, honestly observed effect stands.
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Applied);
}

PF_TEST(hardening, huge_but_valid_burst_is_bounded_and_applied) {
  Harness h;
  h.start();
  // 100 Gbps keeps rate times burst horizon inside 64 bits while still allowing
  // the maximum configured burst depth.
  h.install_grant(100'000'000'000ull);
  h.bind_flow(h.flow);
  const Limits limits{};
  h.publish_policy(0, 1'000'000ull, 0, limits.max_burst_bytes, 65'536, 10'000'000ull,
                   CadenceShape::TokenBucket);
  h.register_backend();
  const PacingEnvelope envelope = h.derive(h.flow);
  PF_CHECK_EQ(envelope.cadence.burst_bytes, limits.max_burst_bytes);
  const AttemptId id = h.apply(envelope);
  PF_CHECK_EQ(h.record(id).state, ApplicationState::Applied);
}

PF_TEST(hardening, huge_burst_beyond_capacity_is_refused) {
  Harness h;
  h.start();
  h.install_grant(1'000'000ull);  // 1 Mbps
  h.bind_flow(h.flow);
  h.publish_policy(0, 1'000'000ull, 0, 1ull << 29, 1500, 1'000'000ull, CadenceShape::TokenBucket);
  h.register_backend();
  auto envelope = h.fabric->derive_envelope(h.flow);
  PF_CHECK_CODE(envelope, ErrorCode::BurstExceedsBound);
}

PF_TEST(hardening, many_flows_share_one_policy_without_cross_talk) {
  Harness h;
  h.start();
  h.install_grant();
  h.publish_policy(1'000'000'000ull);
  h.register_backend();
  std::vector<FlowId> flows;
  for (u64 i = 0; i < 64; ++i) {
    const FlowId flow = FlowId::from(1000 + i);
    h.bind_flow(flow);
    flows.push_back(flow);
  }
  for (const FlowId flow : flows) {
    const PacingEnvelope envelope = h.derive(flow);
    const AttemptId id = h.apply(envelope);
    PF_CHECK_EQ(h.record(id).state, ApplicationState::Applied);
    PF_CHECK_EQ(h.record(id).flow.value(), flow.value());
  }
  for (const FlowId flow : flows) {
    PF_CHECK_EQ(h.fabric->attempts_for_flow(flow).size(), std::size_t{1});
  }
}

PF_TEST(hardening, simultaneous_shutdown_and_cancellation_leave_consistent_state) {
  LoopbackBackend::Options options{};
  options.stall_apply = true;
  Harness h;
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
  const PacingEnvelope envelope = h.derive(h.flow);

  StatusOr<AttemptId> result = Status::of(ErrorCode::Internal, "not run");
  Status shutdown_status = Status::of(ErrorCode::Internal, "not run");
  AttemptId cancelled{};
  {
    std::jthread worker([&] { result = h.fabric->apply_envelope(envelope.ref, h.backend_id); });
    h.backend->wait_until_stalled();
    cancelled = h.fabric->attempts_for_flow(h.flow)[0].attempt;
    PF_CHECK_OK(h.fabric->cancel_attempt(cancelled, "cancel before shutdown"));
    std::jthread stopper([&] { shutdown_status = h.fabric->shutdown(); });
    h.backend->release_stall();
  }
  PF_CHECK_OK(shutdown_status);
  // The completion arrived after the attempt was closed, so it must have been
  // discarded rather than committed.
  PF_CHECK_CODE(result, ErrorCode::Cancelled);
  const ApplicationRecord record = h.record(cancelled);
  PF_CHECK_EQ(record.state, ApplicationState::Cancelled);
  PF_CHECK(!record.claims_effect());
  PF_CHECK(!h.backend->installed(cancelled));
}
