// Pacing Fabric - authoritative flow/resource binding.
//
// The binding is the statement "this flow is carried on this resource over
// this path, at this generation". The fabric consumes bindings; it does not
// place flows, choose paths or admit traffic.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_BINDING_HPP
#define PACING_FABRIC_BINDING_HPP

#include <vector>

#include "pacing/id.hpp"
#include "pacing/limits.hpp"
#include "pacing/status.hpp"

namespace pacing {

struct FlowBinding {
  FlowId flow{};
  Generation generation{};  // binding generation; any bump invalidates envelopes
  ResourceRef resource{};
  PathRef path{};
  ServiceClassId service_class{};
  TenantId tenant{};
  // Flow-declared upper bounds. These can only tighten the policy allowance;
  // they can never raise it above the policy or the upstream ceiling.
  u64 declared_max_burst_bytes{0};
  u64 declared_max_burst_packets{0};
  // A quiesced binding still exists but authorizes no new pacing envelope.
  bool quiesced{false};
  ProvenanceId provenance{};

  [[nodiscard]] bool usable() const noexcept {
    return flow.valid() && generation.known() && resource.known() && path.known() && !quiesced;
  }

  [[nodiscard]] u64 digest() const noexcept;
};

// Generation-bound binding registry. Rebinding a flow bumps its generation and
// therefore invalidates every envelope derived from the previous binding.
class BindingRegistry {
 public:
  explicit BindingRegistry(const Limits& limits) : limits_(limits) {}

  [[nodiscard]] Status bind(FlowBinding binding);
  // Recovery path: installs a binding exactly as recorded, without advancing
  // its generation. Used only when replaying durable state, where the recorded
  // generation is authoritative history rather than a new publication.
  [[nodiscard]] Status restore(FlowBinding binding);
  [[nodiscard]] StatusOr<FlowBinding> get(FlowId flow) const;
  [[nodiscard]] Status unbind(FlowId flow);
  [[nodiscard]] Status quiesce(FlowId flow, bool quiesced);
  [[nodiscard]] std::vector<FlowBinding> live_bindings() const;
  [[nodiscard]] std::size_t size() const noexcept { return bindings_.size(); }

 private:
  struct Entry {
    FlowBinding binding{};
    bool live{true};
  };
  const Limits& limits_;
  std::vector<Entry> bindings_{};
};

}  // namespace pacing

#endif  // PACING_FABRIC_BINDING_HPP
