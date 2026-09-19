// Model-level tests: cadence derivation, authority binding, generation stores.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"
#include "harness.hpp"

using namespace pacing;
using pf_test::Harness;

namespace {

DerivationInput base_input() {
  DerivationInput in{};
  in.ceiling_bps = 10'000'000'000ull;  // 10 Gbps
  in.floor_bps = 0;
  in.quantum_bytes = 1500;
  in.window_ns = 1'000'000ull;
  in.shape = CadenceShape::Uniform;
  in.rate_share_ppm = 1'000'000ull;
  return in;
}

}  // namespace

PF_TEST(rate, interval_never_exceeds_requested_rate) {
  tf::Rng rng(0xA11CEull);
  for (int i = 0; i < 20000; ++i) {
    const u64 quantum = rng.range(1, 1u << 20);
    const u64 rate = rng.range(1, 400'000'000'000ull);
    u64 interval = 0;
    PF_CHECK(derive_interval_ns(quantum, rate, interval));
    PF_CHECK(interval > 0);
    // realized rate = quantum*8e9/interval must be <= rate
    u64 realized = 0;
    PF_CHECK(checked::mul_div_floor(quantum, 8ull * kNanosPerSecond, interval, realized));
    PF_CHECK(realized <= rate);
  }
}

PF_TEST(rate, interval_rejects_zero_inputs) {
  u64 interval = 0;
  PF_CHECK(!derive_interval_ns(0, 1000, interval));
  PF_CHECK(!derive_interval_ns(1000, 0, interval));
}

PF_TEST(rate, burst_capacity_is_rate_proportional) {
  u64 capacity = 0;
  PF_CHECK(burst_capacity_bytes(10'000'000'000ull, 1'000'000ull, capacity));
  // 10 Gbps for 1 ms == 1.25 MB
  PF_CHECK_EQ(capacity, 1'250'000ull);
  PF_CHECK(burst_capacity_bytes(0, 1'000'000ull, capacity));
  PF_CHECK_EQ(capacity, 0ull);
}

PF_TEST(rate, derivation_never_exceeds_ceiling) {
  tf::Rng rng(0xBEEFull);
  const Limits limits{};
  for (int i = 0; i < 20000; ++i) {
    DerivationInput in{};
    in.ceiling_bps = rng.range(1, 400'000'000'000ull);
    in.floor_bps = rng.range(0, in.ceiling_bps);
    in.min_rate_bps = rng.range(0, in.ceiling_bps);
    in.requested_rate_bps = rng.coin() ? 0 : rng.range(0, 400'000'000'000ull);
    in.rate_share_ppm = rng.range(1, 1'000'000ull);
    in.quantum_bytes = rng.range(1, 65'536);
    in.window_ns = rng.range(limits.min_window_ns, 10'000'000ull);
    in.burst_bytes = rng.coin() ? 0 : rng.range(0, 1u << 20);
    in.burst_packets = rng.coin() ? 0 : rng.range(0, 4096);
    switch (rng.below(3)) {
      case 0: in.shape = CadenceShape::Uniform; break;
      case 1: in.shape = CadenceShape::Windowed; in.grants_per_window_hint = rng.below(1000); break;
      default: in.shape = CadenceShape::TokenBucket; break;
    }
    auto outcome = derive_cadence(in, limits);
    if (!outcome) {
      // A refusal is always an acceptable answer; a bad cadence is not.
      PF_CHECK(outcome.code() != ErrorCode::Ok);
      continue;
    }
    const Cadence& cadence = outcome.value().cadence;
    PF_CHECK(cadence.rate_bps <= in.ceiling_bps);
    u64 effective = 0;
    PF_CHECK(cadence.effective_rate_bps(effective));
    PF_CHECK(effective <= in.ceiling_bps);
    PF_CHECK(cadence.interval_ns > 0);
    PF_CHECK(cadence.burst_bytes >= cadence.quantum_bytes);
    PF_CHECK(cadence.burst_bytes <= limits.max_burst_bytes);
    PF_CHECK(cadence.burst_packets <= limits.max_burst_packets);
    if (in.shape == CadenceShape::Windowed && in.grants_per_window_hint != 0) {
      PF_CHECK(cadence.grants_per_window <= in.grants_per_window_hint);
    }
  }
}

PF_TEST(rate, derivation_refuses_entitlement_mint) {
  DerivationInput in = base_input();
  in.min_rate_bps = in.ceiling_bps + 1;
  auto outcome = derive_cadence(in, Limits{});
  PF_CHECK_CODE(outcome, ErrorCode::EntitlementMint);
}

PF_TEST(rate, derivation_clamps_request_to_ceiling) {
  DerivationInput in = base_input();
  in.requested_rate_bps = in.ceiling_bps * 4;
  auto outcome = derive_cadence(in, Limits{});
  PF_CHECK_STATUS_OK(outcome);
  PF_CHECK(outcome.value().clamped_to_ceiling);
  PF_CHECK_EQ(outcome.value().cadence.rate_bps, in.ceiling_bps);
  PF_CHECK_EQ(outcome.value().requested_rate_bps, in.ceiling_bps * 4);
}

PF_TEST(rate, derivation_raises_to_authorized_floor) {
  DerivationInput in = base_input();
  in.floor_bps = 1'000'000'000ull;
  in.min_rate_bps = 2'000'000'000ull;
  in.requested_rate_bps = 1;  // deliberately tiny
  auto outcome = derive_cadence(in, Limits{});
  PF_CHECK_STATUS_OK(outcome);
  PF_CHECK(outcome.value().clamped_to_floor);
  PF_CHECK_EQ(outcome.value().cadence.rate_bps, 2'000'000'000ull);
  PF_CHECK(outcome.value().cadence.rate_bps <= in.ceiling_bps);
}

PF_TEST(rate, derivation_refuses_floor_above_ceiling) {
  DerivationInput in = base_input();
  in.floor_bps = in.ceiling_bps + 10;
  auto outcome = derive_cadence(in, Limits{});
  PF_CHECK_CODE(outcome, ErrorCode::FloorExceedsCeiling);
}

PF_TEST(rate, derivation_rejects_out_of_bound_inputs) {
  const Limits limits{};
  {
    DerivationInput in = base_input();
    in.ceiling_bps = 0;
    PF_CHECK_CODE(derive_cadence(in, limits), ErrorCode::UnknownAuthority);
  }
  {
    DerivationInput in = base_input();
    in.quantum_bytes = 0;
    PF_CHECK_CODE(derive_cadence(in, limits), ErrorCode::OutOfRange);
  }
  {
    DerivationInput in = base_input();
    in.quantum_bytes = limits.max_quantum_bytes + 1;
    PF_CHECK_CODE(derive_cadence(in, limits), ErrorCode::OutOfRange);
  }
  {
    DerivationInput in = base_input();
    in.window_ns = limits.min_window_ns - 1;
    PF_CHECK_CODE(derive_cadence(in, limits), ErrorCode::OutOfRange);
  }
  {
    DerivationInput in = base_input();
    in.window_ns = limits.max_window_ns + 1;
    PF_CHECK_CODE(derive_cadence(in, limits), ErrorCode::OutOfRange);
  }
  {
    DerivationInput in = base_input();
    in.rate_share_ppm = 1'000'001ull;
    PF_CHECK_CODE(derive_cadence(in, limits), ErrorCode::OutOfRange);
  }
  {
    DerivationInput in = base_input();
    in.ceiling_bps = limits.max_rate_bps + 1;
    PF_CHECK_CODE(derive_cadence(in, limits), ErrorCode::OutOfRange);
  }
  {
    DerivationInput in = base_input();
    in.requested_rate_bps = limits.max_rate_bps + 1;
    PF_CHECK_CODE(derive_cadence(in, limits), ErrorCode::OutOfRange);
  }
  {
    DerivationInput in = base_input();
    in.burst_packets = limits.max_burst_packets + 1;
    PF_CHECK_CODE(derive_cadence(in, limits), ErrorCode::BurstExceedsBound);
  }
  {
    DerivationInput in = base_input();
    in.burst_bytes = limits.max_burst_bytes + 1;
    PF_CHECK_CODE(derive_cadence(in, limits), ErrorCode::BurstExceedsBound);
  }
  {
    DerivationInput in = base_input();
    in.ceiling_bps = 1'000'000;  // 1 Mbps
    in.quantum_bytes = 1500;
    in.window_ns = 1'000'000;
    in.burst_bytes = limits.max_burst_bytes;  // far beyond what 1 Mbps can accumulate
    in.shape = CadenceShape::TokenBucket;
    PF_CHECK_CODE(derive_cadence(in, limits), ErrorCode::BurstExceedsBound);
  }
}

PF_TEST(rate, derivation_rejects_burst_below_one_quantum) {
  DerivationInput in = base_input();
  in.burst_bytes = 10;
  in.quantum_bytes = 1500;
  PF_CHECK_CODE(derive_cadence(in, Limits{}), ErrorCode::InvalidArgument);
}

PF_TEST(rate, flow_bound_only_tightens_burst) {
  DerivationInput in = base_input();
  in.burst_bytes = 100'000;
  in.flow_max_burst_bytes = 200'000;  // looser than the policy: ignored
  auto outcome = derive_cadence(in, Limits{});
  PF_CHECK_STATUS_OK(outcome);
  PF_CHECK_EQ(outcome.value().cadence.burst_bytes, 100'000ull);

  in.flow_max_burst_bytes = 50'000;  // tighter than the policy: applied
  outcome = derive_cadence(in, Limits{});
  PF_CHECK_STATUS_OK(outcome);
  PF_CHECK_EQ(outcome.value().cadence.burst_bytes, 50'000ull);
}

PF_TEST(rate, zero_burst_defaults_to_one_quantum) {
  DerivationInput in = base_input();
  in.burst_bytes = 0;
  in.burst_packets = 0;
  auto outcome = derive_cadence(in, Limits{});
  PF_CHECK_STATUS_OK(outcome);
  PF_CHECK_EQ(outcome.value().cadence.burst_bytes, in.quantum_bytes);
  PF_CHECK_EQ(outcome.value().cadence.burst_packets, 1ull);
}

PF_TEST(rate, windowed_hint_never_densifies_beyond_rate) {
  DerivationInput in = base_input();
  in.shape = CadenceShape::Windowed;
  in.grants_per_window_hint = 1000;  // far denser than the authorized rate allows
  auto outcome = derive_cadence(in, Limits{});
  PF_CHECK_STATUS_OK(outcome);
  u64 effective = 0;
  PF_CHECK(outcome.value().cadence.effective_rate_bps(effective));
  PF_CHECK(effective <= in.ceiling_bps);
}

PF_TEST(rate, validate_cadence_rejects_inconsistency) {
  Cadence cadence{};
  cadence.rate_bps = 1'000'000'000ull;
  cadence.quantum_bytes = 1500;
  cadence.interval_ns = 12'000;
  cadence.window_ns = 1'000'000;
  cadence.grants_per_window = 83;
  cadence.bytes_per_window = 83 * 1500;
  cadence.burst_bytes = 1500;
  cadence.burst_packets = 1;
  cadence.shape = CadenceShape::Uniform;
  PF_CHECK_OK(validate_cadence(cadence, 1'000'000'000ull, 1u << 30, 1u << 24));

  Cadence broken = cadence;
  broken.bytes_per_window += 1;
  PF_CHECK_STATUS_CODE(validate_cadence(broken, 1'000'000'000ull, 1u << 30, 1u << 24),
                       ErrorCode::InvalidState);

  Cadence over = cadence;
  over.rate_bps = 1;  // effective rate now exceeds the stated rate
  PF_CHECK_STATUS_CODE(validate_cadence(over, 1'000'000'000ull, 1u << 30, 1u << 24),
                       ErrorCode::CeilingExceeded);
}

PF_TEST(authority, vector_completeness_and_digest) {
  AuthorityVector v{};
  PF_CHECK(!v.complete());
  v.flow = pacing::FlowRef{FlowId::from(1), Generation::from(1)};
  v.resource = ResourceRef{ResourceId::from(2), Generation::from(1)};
  v.policy = PolicyRef{PolicyId::from(3), Generation::from(1)};
  v.rate_grant = pacing::GrantRef{pacing::GrantId::from(4), Generation::from(1)};
  v.path = pacing::PathRef{pacing::PathId::from(5), Generation::from(1)};
  v.epoch = Epoch::from(1);
  v.coordinator_boot = BootId{1, 99};
  PF_CHECK(v.complete());

  AuthorityVector same = v;
  PF_CHECK_EQ(v.digest(), same.digest());
  PF_CHECK_EQ(compare_authority(v, same), AuthorityDrift::None);

  same.path.generation = Generation::from(2);
  PF_CHECK_EQ(compare_authority(v, same), AuthorityDrift::Path);
  same = v;
  same.rate_grant.generation = Generation::from(2);
  PF_CHECK_EQ(compare_authority(v, same), AuthorityDrift::RateGrant);
  same = v;
  same.policy.generation = Generation::from(2);
  PF_CHECK_EQ(compare_authority(v, same), AuthorityDrift::Policy);
  same = v;
  same.resource.generation = Generation::from(2);
  PF_CHECK_EQ(compare_authority(v, same), AuthorityDrift::Resource);
  same = v;
  same.flow.generation = Generation::from(2);
  PF_CHECK_EQ(compare_authority(v, same), AuthorityDrift::Flow);
  same = v;
  same.epoch = Epoch::from(2);
  PF_CHECK_EQ(compare_authority(v, same), AuthorityDrift::Epoch);
  same = v;
  same.coordinator_boot = BootId{2, 99};
  PF_CHECK_EQ(compare_authority(v, same), AuthorityDrift::CoordinatorBoot);

  AuthorityVector incomplete{};
  PF_CHECK_EQ(compare_authority(incomplete, v), AuthorityDrift::Incomplete);
}

PF_TEST(authority, generation_and_epoch_saturate_not_wrap) {
  Generation g = Generation::from(1);
  bool exhausted = false;
  for (int i = 0; i < 5; ++i) g = g.next(exhausted);
  PF_CHECK_EQ(g.value(), 6ull);
  PF_CHECK(!exhausted);
  g = Generation::from(std::numeric_limits<u64>::max());
  g = g.next(exhausted);
  PF_CHECK(exhausted);
  PF_CHECK_EQ(g.value(), std::numeric_limits<u64>::max());

  Epoch e = Epoch::from(std::numeric_limits<u64>::max());
  e = e.advanced(exhausted);
  PF_CHECK(exhausted);
  PF_CHECK_EQ(e.value(), std::numeric_limits<u64>::max());
}

PF_TEST(policy_store, republish_advances_generation) {
  Limits limits{};
  PolicyStore store(limits);
  PacingPolicy policy{};
  policy.ref.id = PolicyId::from(11);
  policy.layer = PacingLayer::Resource;
  policy.resource = ResourceId::from(1);
  policy.path = pacing::PathRef{pacing::PathId::from(2), Generation::from(1)};
  policy.quantum_bytes = 1500;
  policy.window_ns = 1'000'000;
  policy.rate_share_ppm = 1'000'000;

  auto first = store.publish(policy);
  PF_CHECK_STATUS_OK(first);
  PF_CHECK_EQ(first.value().generation.value(), 1ull);

  policy.rate_share_ppm = 500'000;
  auto second = store.publish(policy);
  PF_CHECK_STATUS_OK(second);
  PF_CHECK_EQ(second.value().generation.value(), 2ull);

  auto resolved = store.resolve(FlowId::from(1), ResourceId::from(1));
  PF_CHECK_STATUS_OK(resolved);
  PF_CHECK_EQ(resolved.value().ref.generation.value(), 2ull);
  PF_CHECK_EQ(resolved.value().rate_share_ppm, 500'000ull);
}

PF_TEST(policy_store, flow_layer_wins_over_resource_layer) {
  Limits limits{};
  PolicyStore store(limits);
  PacingPolicy resource_policy{};
  resource_policy.ref.id = PolicyId::from(1);
  resource_policy.layer = PacingLayer::Resource;
  resource_policy.resource = ResourceId::from(1);
  resource_policy.path = pacing::PathRef{pacing::PathId::from(2), Generation::from(1)};
  resource_policy.quantum_bytes = 1500;
  resource_policy.window_ns = 1'000'000;
  PF_CHECK_STATUS_OK(store.publish(resource_policy));

  PacingPolicy flow_policy{};
  flow_policy.ref.id = PolicyId::from(2);
  flow_policy.layer = PacingLayer::Flow;
  flow_policy.flow = FlowId::from(42);
  flow_policy.resource = ResourceId::from(1);
  flow_policy.path = pacing::PathRef{pacing::PathId::from(2), Generation::from(1)};
  flow_policy.quantum_bytes = 9000;
  flow_policy.window_ns = 1'000'000;
  PF_CHECK_STATUS_OK(store.publish(flow_policy));

  auto resolved = store.resolve(FlowId::from(42), ResourceId::from(1));
  PF_CHECK_STATUS_OK(resolved);
  PF_CHECK_EQ(resolved.value().ref.id.value(), 2ull);

  auto other = store.resolve(FlowId::from(43), ResourceId::from(1));
  PF_CHECK_STATUS_OK(other);
  PF_CHECK_EQ(other.value().ref.id.value(), 1ull);

  auto none = store.resolve(FlowId::from(43), ResourceId::from(99));
  PF_CHECK_CODE(none, ErrorCode::NotFound);
}

PF_TEST(policy_store, validation_rejects_malformed_policies) {
  const Limits limits{};
  PacingPolicy policy{};
  policy.ref.id = PolicyId::from(1);
  policy.ref.generation = Generation::from(1);
  policy.layer = PacingLayer::Resource;
  policy.resource = ResourceId::from(1);
  policy.path = pacing::PathRef{pacing::PathId::from(2), Generation::from(1)};
  policy.quantum_bytes = 1500;
  policy.window_ns = 1'000'000;
  PF_CHECK_OK(validate_policy(policy, limits));

  PacingPolicy no_path = policy;
  no_path.path = pacing::PathRef{};
  PF_CHECK_STATUS_CODE(validate_policy(no_path, limits), ErrorCode::InvalidArgument);

  PacingPolicy no_resource = policy;
  no_resource.resource = ResourceId{};
  PF_CHECK_STATUS_CODE(validate_policy(no_resource, limits), ErrorCode::InvalidArgument);

  PacingPolicy bad_share = policy;
  bad_share.rate_share_ppm = 2'000'000;
  PF_CHECK_STATUS_CODE(validate_policy(bad_share, limits), ErrorCode::OutOfRange);

  PacingPolicy zero_quantum = policy;
  zero_quantum.quantum_bytes = 0;
  PF_CHECK_STATUS_CODE(validate_policy(zero_quantum, limits), ErrorCode::OutOfRange);

  PacingPolicy short_window = policy;
  short_window.window_ns = 1;
  PF_CHECK_STATUS_CODE(validate_policy(short_window, limits), ErrorCode::OutOfRange);

  PacingPolicy bad_burst = policy;
  bad_burst.burst_packets = limits.max_burst_packets + 1;
  PF_CHECK_STATUS_CODE(validate_policy(bad_burst, limits), ErrorCode::BurstExceedsBound);

  PacingPolicy dup = policy;
  ServiceClassException ex{};
  ex.service_class = ServiceClassId::from(5);
  dup.exceptions.push_back(ex);
  dup.exceptions.push_back(ex);
  PF_CHECK_STATUS_CODE(validate_policy(dup, limits), ErrorCode::InvalidArgument);

  PacingPolicy too_many = policy;
  for (u32 i = 0; i < limits.max_service_class_exceptions + 1; ++i) {
    ServiceClassException e{};
    e.service_class = ServiceClassId::from(1000 + i);
    too_many.exceptions.push_back(e);
  }
  PF_CHECK_STATUS_CODE(validate_policy(too_many, limits), ErrorCode::Oversized);
}

PF_TEST(binding_registry, rebind_advances_generation) {
  Limits limits{};
  BindingRegistry registry(limits);
  FlowBinding binding{};
  binding.flow = FlowId::from(7);
  binding.resource = pacing::ResourceRef{ResourceId::from(1), Generation::from(1)};
  binding.path = pacing::PathRef{pacing::PathId::from(2), Generation::from(1)};
  PF_CHECK_OK(registry.bind(binding));
  auto first = registry.get(FlowId::from(7));
  PF_CHECK_STATUS_OK(first);
  PF_CHECK_EQ(first.value().generation.value(), 1ull);

  PF_CHECK_OK(registry.bind(binding));
  auto second = registry.get(FlowId::from(7));
  PF_CHECK_STATUS_OK(second);
  PF_CHECK_EQ(second.value().generation.value(), 2ull);

  PF_CHECK_OK(registry.quiesce(FlowId::from(7), true));
  auto quiesced = registry.get(FlowId::from(7));
  PF_CHECK_STATUS_OK(quiesced);
  PF_CHECK(!quiesced.value().usable());
  PF_CHECK_EQ(quiesced.value().generation.value(), 3ull);

  PF_CHECK_OK(registry.unbind(FlowId::from(7)));
  PF_CHECK_CODE(registry.get(FlowId::from(7)), ErrorCode::NotFound);
}

PF_TEST(binding_registry, rejects_malformed_bindings) {
  Limits limits{};
  BindingRegistry registry(limits);
  FlowBinding binding{};
  PF_CHECK_STATUS_CODE(registry.bind(binding), ErrorCode::InvalidArgument);

  binding.flow = FlowId::from(1);
  PF_CHECK_STATUS_CODE(registry.bind(binding), ErrorCode::InvalidArgument);

  binding.resource = pacing::ResourceRef{ResourceId::from(1), Generation::from(1)};
  PF_CHECK_STATUS_CODE(registry.bind(binding), ErrorCode::InvalidArgument);

  binding.path = pacing::PathRef{pacing::PathId::from(2), Generation::from(1)};
  PF_CHECK_OK(registry.bind(binding));

  FlowBinding oversized = binding;
  oversized.flow = FlowId::from(2);
  oversized.declared_max_burst_bytes = limits.max_burst_bytes + 1;
  PF_CHECK_STATUS_CODE(registry.bind(oversized), ErrorCode::BurstExceedsBound);
}
