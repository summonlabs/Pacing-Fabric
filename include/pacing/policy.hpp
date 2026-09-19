// Pacing Fabric - pacing policy: the authored intent that envelopes derive
// from. Policy is generation-bound; publishing a new generation invalidates
// every envelope and every applied record derived from the previous one.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_POLICY_HPP
#define PACING_FABRIC_POLICY_HPP

#include <string>
#include <vector>

#include "pacing/limits.hpp"
#include "pacing/rate.hpp"
#include "pacing/status.hpp"

namespace pacing {

// Scope at which a policy applies. A resource policy is the default for every
// flow bound to that resource; a flow policy overrides it for exactly one flow.
enum class PacingLayer : u8 {
  Resource = 0,
  Flow = 1,
};

std::string_view to_string(PacingLayer layer) noexcept;
bool parse_pacing_layer(std::string_view text, PacingLayer& out) noexcept;

// A per-service-class refinement. It can raise the floor and raise the burst
// allowance for one class - it can never lift the upstream ceiling.
struct ServiceClassException {
  ServiceClassId service_class{};
  u64 min_rate_bps{0};
  u64 rate_share_ppm{0};
  u64 burst_bytes{0};
  u64 burst_packets{0};
  u32 priority{0};

  friend constexpr bool operator==(const ServiceClassException&, const ServiceClassException&) noexcept = default;
};

// Authored pacing policy. All rate fields are requests; the ceiling comes from
// upstream rate authority and is applied during derivation.
struct PacingPolicy {
  PolicyRef ref{};
  PacingLayer layer{PacingLayer::Resource};
  ResourceId resource{};   // required for the Resource layer
  FlowId flow{};           // required for the Flow layer
  u64 rate_share_ppm{1'000'000ull};  // share of upstream ceiling (1e6 == 100%)
  u64 rate_bps{0};                   // absolute request; 0 means "use the share"
  u64 min_rate_bps{0};               // floor this policy insists on
  u64 burst_bytes{0};
  u64 burst_packets{0};
  u64 quantum_bytes{0};
  u64 window_ns{0};
  CadenceShape shape{CadenceShape::Uniform};
  u64 grants_per_window_hint{0};  // Windowed shape only
  PathRef path{};                 // path binding the policy assumes
  ServiceClassId default_service_class{};
  std::vector<ServiceClassException> exceptions{};
  TenantId tenant{};
  ProvenanceId provenance{};
  Instant published_at{};

  [[nodiscard]] bool applies_to(FlowId candidate, ResourceId candidate_resource) const noexcept {
    if (layer == PacingLayer::Flow) return flow == candidate;
    return resource == candidate_resource;
  }
};

// Validates shape and bounds of an authored policy. Rejects empty identities,
// zero quantum/window, an unset path, out-of-bound burst values, an oversized
// exception table, and a floor above the absolute request.
[[nodiscard]] Status validate_policy(const PacingPolicy& policy, const Limits& limits) noexcept;

// Selects the exception matching a service class, if one exists. Returns
// nullptr when the class is not covered, which means "no refinement" and never
// "no limit".
[[nodiscard]] const ServiceClassException* find_exception(const PacingPolicy& policy,
                                                          ServiceClassId service_class) noexcept;

// Generation-tracked policy registry. Lookups never return a policy whose
// generation has been superseded without saying so.
class PolicyStore {
 public:
  explicit PolicyStore(const Limits& limits) : limits_(limits) {}

  // Publishes a policy. A repeat publish of the same policy id advances the
  // generation; the returned ref is the newly authoritative one.
  [[nodiscard]] StatusOr<PolicyRef> publish(PacingPolicy policy);

  // Recovery path: installs a policy exactly as recorded, without advancing
  // its generation.
  [[nodiscard]] Status restore(PacingPolicy policy);
  [[nodiscard]] std::vector<PacingPolicy> live_policies() const;

  // Retrieves the current policy for a policy id.
  [[nodiscard]] StatusOr<PacingPolicy> get(PolicyId id) const;

  // Retrieves the policy that governs a flow/resource pair, preferring the
  // flow-layer policy. Returns NotFound when nothing governs the binding.
  [[nodiscard]] StatusOr<PacingPolicy> resolve(FlowId flow, ResourceId resource) const;

  // Invalidates a policy id entirely: subsequent resolves treat it as absent.
  [[nodiscard]] Status remove(PolicyId id);

  [[nodiscard]] std::size_t size() const noexcept { return policies_.size(); }
  [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

 private:
  struct Entry {
    PacingPolicy policy{};
    bool live{true};
  };

  const Limits& limits_;
  std::vector<Entry> policies_{};
};

}  // namespace pacing

#endif  // PACING_FABRIC_POLICY_HPP
