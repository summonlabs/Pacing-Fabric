// Shared fabric harness for tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_TESTS_HARNESS_HPP
#define PACING_FABRIC_TESTS_HARNESS_HPP

#include <memory>
#include <string>

#include "framework.hpp"
#include "support.hpp"

namespace pf_test {

using pacing::CadenceShape;
using pacing::FlowId;
using pacing::Generation;
using pacing::PolicyId;
using pacing::ResourceId;
using pacing::ServiceClassId;

// Construction-time options. The fabric copies its configuration, so anything
// that must be in force at initialize() has to be supplied here rather than
// mutated afterwards.
struct HarnessOptions {
  pacing::Limits limits{};
  std::uint32_t max_pending_attempts{4096};
  std::uint64_t envelope_lifetime_ns{1'000'000'000ull};
  std::uint64_t tick_period_ns{1'000'000ull};
};

struct Harness {
  pacing::Limits limits{};
  pacing::ManualClock clock{1'000'000'000ull};
  ScriptedRateAuthority upstream{};
  pacing::FabricConfig config{};
  std::unique_ptr<pacing::PacingFabric> fabric{};
  std::shared_ptr<LoopbackBackend> backend{};

  ResourceId resource{ResourceId::from(1)};
  pacing::PathId path{pacing::PathId::from(2)};
  pacing::GrantId grant{pacing::GrantId::from(3)};
  pacing::BackendId backend_id{pacing::BackendId::from(9)};
  FlowId flow{FlowId::from(100)};

  explicit Harness(const std::string& durable_directory = std::string{}, std::uint64_t instance = 7,
                   HarnessOptions options = {})
      : limits(options.limits) {
    config.limits = limits;
    config.durable_directory = durable_directory;
    config.enable_durability = !durable_directory.empty();
    config.fabric_instance = instance;
    config.tick_period_ns = options.tick_period_ns;
    config.envelope_lifetime_ns = options.envelope_lifetime_ns;
    config.max_pending_attempts = options.max_pending_attempts;
    fabric = std::make_unique<pacing::PacingFabric>(config, upstream, clock);
  }

  // Rebinds the flow, advancing its binding generation. Used by race tests
  // that need concurrent reconfiguration.
  void rebind() {
    pacing::FlowBinding b{};
    b.flow = flow;
    b.resource = pacing::ResourceRef{resource, Generation::from(1)};
    b.path = pacing::PathRef{path, Generation::from(1)};
    PF_CHECK_OK(fabric->bind_flow(b));
  }

  void start() { PF_CHECK_OK(fabric->initialize()); }

  void install_grant(std::uint64_t ceiling_bps = 10'000'000'000ull, std::uint64_t floor_bps = 0,
                     std::uint64_t generation = 1, std::uint64_t revision = 1) {
    pacing::RateGrant g{};
    g.ref = pacing::GrantRef{grant, Generation::from(generation)};
    g.resource = resource;
    g.ceiling_bps = ceiling_bps;
    g.floor_bps = floor_bps;
    g.revision = revision;
    g.observed_at = clock.now();
    g.authoritative = true;
    upstream.install(resource, g);
  }

  void bind_flow(FlowId target, std::uint64_t declared_burst_bytes = 0,
                 std::uint64_t declared_burst_packets = 0, ServiceClassId service_class = ServiceClassId{}) {
    pacing::FlowBinding b{};
    b.flow = target;
    b.resource = pacing::ResourceRef{resource, Generation::from(1)};
    b.path = pacing::PathRef{path, Generation::from(1)};
    b.service_class = service_class;
    b.declared_max_burst_bytes = declared_burst_bytes;
    b.declared_max_burst_packets = declared_burst_packets;
    PF_CHECK_OK(fabric->bind_flow(b));
  }

  pacing::PolicyRef publish_policy(std::uint64_t rate_bps = 0, std::uint64_t share_ppm = 1'000'000ull,
                                   std::uint64_t min_rate_bps = 0, std::uint64_t burst_bytes = 0,
                                   std::uint64_t quantum_bytes = 1500, std::uint64_t window_ns = 1'000'000ull,
                                   CadenceShape shape = CadenceShape::Uniform,
                                   std::uint64_t grants_hint = 0,
                                   std::uint64_t exception_min_rate = 0) {
    pacing::PacingPolicy policy{};
    policy.ref.id = PolicyId::from(1);
    policy.ref.generation = Generation::unknown();
    policy.layer = pacing::PacingLayer::Resource;
    policy.resource = resource;
    policy.rate_bps = rate_bps;
    policy.rate_share_ppm = share_ppm;
    policy.min_rate_bps = min_rate_bps;
    policy.burst_bytes = burst_bytes;
    policy.quantum_bytes = quantum_bytes;
    policy.window_ns = window_ns;
    policy.shape = shape;
    policy.grants_per_window_hint = grants_hint;
    policy.path = pacing::PathRef{path, Generation::from(1)};
    if (exception_min_rate != 0) {
      pacing::ServiceClassException ex{};
      ex.service_class = ServiceClassId::from(5);
      ex.min_rate_bps = exception_min_rate;
      policy.exceptions.push_back(ex);
    }
    auto ref = fabric->publish_policy(std::move(policy));
    PF_CHECK_STATUS_OK(ref);
    return ref.value();
  }

  void register_backend(LoopbackBackend::Options options = {}) {
    backend = std::make_shared<LoopbackBackend>(pacing::BackendRef{backend_id, Generation::from(1)},
                                                std::move(options));
    PF_CHECK_OK(fabric->register_backend(backend));
  }

  [[nodiscard]] pacing::PacingEnvelope derive(FlowId target) {
    auto envelope = fabric->derive_envelope(target);
    PF_CHECK_STATUS_OK(envelope);
    return envelope.value();
  }

  [[nodiscard]] pacing::AttemptId apply(const pacing::PacingEnvelope& envelope) {
    auto attempt = fabric->apply_envelope(envelope.ref, backend_id);
    PF_CHECK_STATUS_OK(attempt);
    return attempt.value();
  }

  [[nodiscard]] pacing::ApplicationRecord record(pacing::AttemptId id) {
    auto r = fabric->attempt(id);
    PF_CHECK_STATUS_OK(r);
    return r.value();
  }
};

}  // namespace pf_test

#endif  // PACING_FABRIC_TESTS_HARNESS_HPP
