// Downstream consumer proof.
//
// Exercises the installed public surface end to end without reaching into the
// Pacing Fabric source tree: it configures a synthetic rate authority, binds a
// flow, publishes a policy, derives an envelope, applies it to a synthetic
// backend and asserts the resulting verified state.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <memory>
#include <string>

#include <pacing/pacing.hpp>

namespace {

int fail(const char* message) {
  std::fprintf(stderr, "downstream_consumer: %s\n", message);
  return 1;
}

class LocalRateAuthority final : public pacing::IRateAuthority {
 public:
  explicit LocalRateAuthority(pacing::u64 ceiling_bps) : ceiling_bps_(ceiling_bps) {}

  [[nodiscard]] pacing::StatusOr<pacing::RateGrant> fetch(pacing::ResourceId resource) const override {
    pacing::RateGrant grant{};
    grant.ref = pacing::GrantRef{pacing::GrantId::from(1), pacing::Generation::from(1)};
    grant.resource = resource;
    grant.ceiling_bps = ceiling_bps_;
    grant.authoritative = true;
    return grant;
  }

  [[nodiscard]] const char* name() const noexcept override { return "downstream-local"; }
  [[nodiscard]] bool synthetic() const noexcept override { return true; }

 private:
  pacing::u64 ceiling_bps_;
};

}  // namespace

int main() {
  std::printf("Pacing Fabric version %s\n", std::string(pacing::kVersionString).c_str());

  LocalRateAuthority authority(10'000'000'000ull);
  pacing::SteadyClock clock;
  pacing::FabricConfig config{};
  config.fabric_instance = 1;

  pacing::PacingFabric fabric(config, authority, clock);
  if (!fabric.initialize().ok()) return fail("initialize failed");

  auto backend = std::make_shared<pacing::SyntheticPacingBackend>(
      pacing::BackendRef{pacing::BackendId::from(9), pacing::Generation::from(1)}, "downstream-synthetic");
  if (!fabric.register_backend(backend).ok()) return fail("register_backend failed");

  pacing::FlowBinding binding{};
  binding.flow = pacing::FlowId::from(100);
  binding.resource = pacing::ResourceRef{pacing::ResourceId::from(1), pacing::Generation::from(1)};
  binding.path = pacing::PathRef{pacing::PathId::from(1), pacing::Generation::from(1)};
  if (!fabric.bind_flow(binding).ok()) return fail("bind_flow failed");

  pacing::PacingPolicy policy{};
  policy.ref.id = pacing::PolicyId::from(1);
  policy.ref.generation = pacing::Generation::unknown();
  policy.layer = pacing::PacingLayer::Resource;
  policy.resource = pacing::ResourceId::from(1);
  policy.path = pacing::PathRef{pacing::PathId::from(1), pacing::Generation::from(1)};
  policy.rate_bps = 2'000'000'000ull;
  policy.quantum_bytes = 1500;
  policy.window_ns = 1'000'000;
  if (!fabric.publish_policy(policy).ok()) return fail("publish_policy failed");

  auto envelope = fabric.derive_envelope(pacing::FlowId::from(100));
  if (!envelope) return fail("derive_envelope failed");
  if (!envelope.value().authority.complete()) return fail("authority incomplete");
  if (envelope.value().cadence.rate_bps > envelope.value().ceiling_bps) {
    return fail("cadence exceeds the authorized ceiling");
  }

  auto attempt = fabric.apply_envelope(envelope.value().ref, pacing::BackendId::from(9));
  if (!attempt) return fail("apply_envelope failed");
  auto record = fabric.attempt(attempt.value());
  if (!record) return fail("attempt lookup failed");
  if (record.value().state != pacing::ApplicationState::Applied) {
    return fail("pacing did not reach a verified applied state");
  }
  if (record.value().effect != pacing::EffectLabel::SyntheticVerified) {
    return fail("effect label is not the expected synthetic verification");
  }

  auto explanation = fabric.explain(pacing::FlowId::from(100));
  if (!explanation) return fail("explain failed");
  std::printf("%s\n", explanation.value().to_text(1024).c_str());

  if (!fabric.shutdown().ok()) return fail("shutdown failed");
  std::printf("downstream consumer verified: derive, apply and explain all succeeded\n");
  return 0;
}
