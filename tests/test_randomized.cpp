// Seeded randomized and fuzz tests. Every seed is an explicit constant so a
// failure reproduces exactly.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <vector>

#include "framework.hpp"
#include "harness.hpp"
#include "pacing/serialize.hpp"

using namespace pacing;
using pf_test::Harness;
using pf_test::HarnessOptions;
using pf_test::LoopbackBackend;
using pf_test::TempDir;

namespace {

void check_invariants(Harness& h) {
  const Limits& limits = h.limits;
  const auto records = h.fabric->attempts_for_flow(h.flow);
  for (const auto& record : records) {
    PF_CHECK(record.attempt.valid());
    PF_CHECK(record.authority.complete());
    if (record.claims_effect()) {
      PF_CHECK_EQ(record.state, ApplicationState::Applied);
      PF_CHECK(record.evidence_integrity_ok);
      PF_CHECK(!record.requires_revalidation);
      PF_CHECK(record.desired.rate_bps <= record.desired.rate_bps);
    }
    if (record.requires_revalidation) PF_CHECK(!record.claims_effect());
    // The effect label is a function of the state: exactly three states may
    // claim anything, and every other state must claim nothing at all.
    switch (record.state) {
      case ApplicationState::Applied:
        PF_CHECK(record.effect == EffectLabel::Verified ||
                 record.effect == EffectLabel::SyntheticVerified);
        break;
      case ApplicationState::AppliedUnverified:
        PF_CHECK_EQ(record.effect, EffectLabel::Unverified);
        break;
      case ApplicationState::Mismatched:
        PF_CHECK_EQ(record.effect, EffectLabel::Mismatched);
        break;
      default:
        PF_CHECK_EQ(record.effect, EffectLabel::None);
        break;
    }
  }
  PF_CHECK(records.size() <= limits.max_attempts);

  const auto envelopes = h.fabric->envelopes_for_flow(h.flow);
  for (const auto& envelope : envelopes) {
    PF_CHECK(envelope.authority.complete());
    PF_CHECK(envelope.cadence.rate_bps <= envelope.ceiling_bps);
    PF_CHECK(envelope.cadence.burst_bytes <= limits.max_burst_bytes);
    PF_CHECK(envelope.cadence.burst_packets <= limits.max_burst_packets);
    u64 effective = 0;
    PF_CHECK(envelope.cadence.effective_rate_bps(effective));
    PF_CHECK(effective <= envelope.ceiling_bps);
    PF_CHECK_OK(validate_cadence(envelope.cadence, limits.max_rate_bps, limits.max_burst_bytes,
                                 limits.max_burst_packets));
  }
  PF_CHECK(envelopes.size() <= limits.max_envelopes);

  auto explanation = h.fabric->explain(h.flow);
  PF_CHECK_STATUS_OK(explanation);
  PF_CHECK(explanation.value().to_text(limits.max_explanation_bytes).size() <=
           limits.max_explanation_bytes);
}

void run_walk(std::uint64_t seed, std::uint64_t steps) {
  TempDir dir("walk");
  Harness h{dir.path(), seed % 1000 + 1};
  h.start();
  h.install_grant();
  h.bind_flow(h.flow);
  h.publish_policy(1'000'000'000ull);
  h.register_backend();

  tf::Rng rng(seed);
  std::vector<AttemptId> attempts;
  std::vector<EnvelopeRef> envelopes;

  for (std::uint64_t step = 0; step < steps; ++step) {
    switch (rng.below(14)) {
      case 0: {
        auto envelope = h.fabric->derive_envelope(h.flow);
        if (envelope) envelopes.push_back(envelope.value().ref);
        break;
      }
      case 1: {
        if (envelopes.empty()) break;
        const EnvelopeRef ref = envelopes[rng.below(envelopes.size())];
        auto attempt = h.fabric->apply_envelope(ref, h.backend_id);
        if (attempt) attempts.push_back(attempt.value());
        break;
      }
      case 2: {
        if (attempts.empty()) break;
        (void)h.fabric->verify_attempt(attempts[rng.below(attempts.size())]);
        break;
      }
      case 3: {
        if (attempts.empty()) break;
        (void)h.fabric->cancel_attempt(attempts[rng.below(attempts.size())], "walk cancellation");
        break;
      }
      case 4: {
        if (envelopes.empty()) break;
        (void)h.fabric->revoke_envelope(envelopes[rng.below(envelopes.size())], "walk revocation");
        break;
      }
      case 5: {
        (void)h.fabric->advance_epoch();
        break;
      }
      case 6: {
        h.rebind();
        break;
      }
      case 7: {
        h.publish_policy(rng.range(1'000'000ull, 20'000'000'000ull));
        break;
      }
      case 8: {
        h.install_grant(rng.range(1'000'000ull, 40'000'000'000ull), 0, rng.range(1, 40), step);
        break;
      }
      case 9: {
        h.clock.advance(rng.range(1, 400'000'000ull));
        break;
      }
      case 10: {
        if (attempts.empty()) break;
        (void)h.fabric->revalidate(attempts[rng.below(attempts.size())]);
        break;
      }
      case 11: {
        (void)h.fabric->fence_below(Epoch::from(rng.range(1, 6)));
        break;
      }
      case 12: {
        LoopbackBackend::Options options{};
        options.supports_readback = rng.coin();
        options.drop_readback = rng.coin();
        options.corrupt_readback = !options.supports_readback ? false : rng.coin();
        options.rate_bias_bps = rng.coin() ? 0 : static_cast<std::int64_t>(rng.range(1, 1000));
        h.backend->set_options(options);
        break;
      }
      default: {
        (void)h.fabric->list_backends();
        (void)h.fabric->stats();
        (void)h.fabric->epoch();
        break;
      }
    }
    check_invariants(h);
  }
  PF_CHECK_OK(h.fabric->shutdown());
}

}  // namespace

PF_TEST(randomized, seeded_operation_walks_preserve_invariants) {
  for (const std::uint64_t seed : {1ull, 7ull, 42ull, 0xDEADBEEFull, 0xC0FFEEull, 0x5EEDull}) {
    run_walk(seed, 220);
  }
}

// A decoder may legitimately succeed on the leading bytes of a longer buffer;
// what it must never do is return a decoded value that violates its own
// validation rules. Trailing bytes are the caller's contract to enforce, and
// that contract is exercised by the durable-replay test in test_persistence.
PF_TEST(randomized, random_bytes_never_decode_into_invalid_records) {
  tf::Rng rng(0xF00DFACEull);
  Limits limits{};
  std::uint64_t successes = 0;
  for (int i = 0; i < 20000; ++i) {
    std::vector<u8> bytes(rng.below(320));
    for (auto& byte : bytes) byte = static_cast<u8>(rng.next() & 0xFF);
    const std::span<const u8> view(bytes.data(), bytes.size());
    {
      ByteReader reader(view);
      FlowBinding decoded{};
      if (decode_binding(reader, limits, decoded).ok()) {
        ++successes;
        PF_CHECK(decoded.flow.valid());
        PF_CHECK(decoded.generation.known());
        PF_CHECK(decoded.resource.known());
        PF_CHECK(decoded.declared_max_burst_bytes <= limits.max_burst_bytes);
      }
    }
    {
      ByteReader reader(view);
      PacingPolicy decoded{};
      if (decode_policy(reader, limits, decoded).ok()) {
        ++successes;
        PF_CHECK_OK(validate_policy(decoded, limits));
      }
    }
    {
      ByteReader reader(view);
      PacingEnvelope decoded{};
      if (decode_envelope(reader, limits, decoded).ok()) {
        ++successes;
        PF_CHECK(decoded.ref.id.valid());
        PF_CHECK(decoded.authority.complete());
        PF_CHECK(decoded.cadence.rate_bps <= decoded.ceiling_bps);
      }
    }
    {
      ByteReader reader(view);
      ApplicationRecord decoded{};
      if (decode_attempt(reader, limits, decoded).ok()) {
        ++successes;
        PF_CHECK(decoded.attempt.valid());
        PF_CHECK(decoded.authority.complete());
        PF_CHECK(decoded.state <= ApplicationState::Ambiguous);
        PF_CHECK(decoded.effect <= EffectLabel::Mismatched);
      }
    }
    {
      ByteReader reader(view);
      RateGrant decoded{};
      if (decode_grant(reader, limits, decoded).ok()) {
        ++successes;
        PF_CHECK(decoded.ceiling_bps <= limits.max_rate_bps);
        PF_CHECK(decoded.floor_bps <= decoded.ceiling_bps);
      }
    }
    {
      ByteReader reader(view);
      BackendDescriptor decoded{};
      if (decode_backend_descriptor(reader, limits, decoded).ok()) {
        ++successes;
        PF_CHECK(decoded.ref.id.valid());
        PF_CHECK(!decoded.name.empty());
        PF_CHECK(decoded.name.size() <= limits.max_name_bytes);
      }
    }
    {
      ByteReader reader(view);
      Cadence decoded{};
      (void)decode_cadence(reader, decoded);
    }
  }
  // Random bytes are overwhelmingly rejected; a decoder that accepted most of
  // them would indicate a missing validation rather than luck.
  PF_CHECK(successes < 200);
}

PF_TEST(randomized, derivation_is_total_over_hostile_input) {
  tf::Rng rng(0xBADF00Dull);
  const Limits limits{};
  for (int i = 0; i < 40000; ++i) {
    DerivationInput input{};
    input.ceiling_bps = rng.next() % 1'000'000'000'000ull;
    input.floor_bps = rng.next() % 1'000'000'000'000ull;
    input.min_rate_bps = rng.next() % 1'000'000'000'000ull;
    input.requested_rate_bps = rng.next() % 1'000'000'000'000ull;
    input.rate_share_ppm = rng.next() % 3'000'000ull;
    input.quantum_bytes = rng.next() % (1ull << 30);
    input.window_ns = rng.next() % (100ull * kNanosPerSecond);
    input.burst_bytes = rng.next() % (1ull << 33);
    input.burst_packets = rng.next() % (1ull << 26);
    input.flow_max_burst_bytes = rng.next() % (1ull << 33);
    input.flow_max_burst_packets = rng.next() % (1ull << 26);
    input.grants_per_window_hint = rng.next() % (1ull << 34);
    switch (rng.below(3)) {
      case 0: input.shape = CadenceShape::Uniform; break;
      case 1: input.shape = CadenceShape::Windowed; break;
      default: input.shape = CadenceShape::TokenBucket; break;
    }
    auto outcome = derive_cadence(input, limits);
    if (!outcome) continue;
    const Cadence& cadence = outcome.value().cadence;
    PF_CHECK(cadence.rate_bps <= input.ceiling_bps);
    u64 effective = 0;
    PF_CHECK(cadence.effective_rate_bps(effective));
    PF_CHECK(effective <= input.ceiling_bps);
    PF_CHECK_OK(validate_cadence(cadence, limits.max_rate_bps, limits.max_burst_bytes,
                                 limits.max_burst_packets));
  }
}
