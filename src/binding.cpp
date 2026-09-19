// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/binding.hpp"

#include "pacing/checked.hpp"

namespace pacing {

u64 FlowBinding::digest() const noexcept {
  Digest64 d;
  d.mix_u64(flow.value());
  d.mix_u64(generation.value());
  d.mix_u64(resource.id.value());
  d.mix_u64(resource.generation.value());
  d.mix_u64(path.id.value());
  d.mix_u64(path.generation.value());
  d.mix_u64(service_class.value());
  d.mix_u64(tenant.value());
  d.mix_u64(declared_max_burst_bytes);
  d.mix_u64(declared_max_burst_packets);
  d.mix_bool(quiesced);
  d.mix_u64(provenance.value());
  return d.value();
}

Status BindingRegistry::bind(FlowBinding binding) {
  if (!binding.flow.valid()) return Status::of(ErrorCode::InvalidArgument, "binding flow id is zero");
  if (!binding.resource.known()) {
    return Status::of(ErrorCode::InvalidArgument, "binding resource reference is not generation-bound");
  }
  if (!binding.path.known()) {
    return Status::of(ErrorCode::InvalidArgument, "binding path reference is not generation-bound");
  }
  if (binding.declared_max_burst_bytes > limits_.max_burst_bytes) {
    return Status::of(ErrorCode::BurstExceedsBound, "binding burst bytes above limit");
  }
  if (binding.declared_max_burst_packets > limits_.max_burst_packets) {
    return Status::of(ErrorCode::BurstExceedsBound, "binding burst packets above limit");
  }
  if (!binding.generation.known()) binding.generation = Generation::first();

  for (auto& entry : bindings_) {
    if (entry.live && entry.binding.flow == binding.flow) {
      // Rebinding always advances the binding generation, which structurally
      // invalidates every envelope derived from the previous binding.
      if (binding.generation <= entry.binding.generation) {
        bool exhausted = false;
        binding.generation = entry.binding.generation.next(exhausted);
        if (exhausted) return Status::of(ErrorCode::ResourceExhausted, "binding generation exhausted");
      }
      entry.binding = binding;
      return Status::success();
    }
  }
  if (bindings_.size() >= limits_.max_flows) {
    return Status::of(ErrorCode::ResourceExhausted, "binding registry is full");
  }
  bindings_.push_back(Entry{binding, true});
  return Status::success();
}

Status BindingRegistry::restore(FlowBinding binding) {
  if (!binding.flow.valid() || !binding.generation.known()) {
    return Status::of(ErrorCode::MalformedInput, "restored binding identity is invalid");
  }
  for (auto& entry : bindings_) {
    if (entry.binding.flow == binding.flow) {
      entry.binding = binding;
      entry.live = true;
      return Status::success();
    }
  }
  if (bindings_.size() >= limits_.max_flows) {
    return Status::of(ErrorCode::ResourceExhausted, "binding registry is full");
  }
  bindings_.push_back(Entry{binding, true});
  return Status::success();
}

StatusOr<FlowBinding> BindingRegistry::get(FlowId flow) const {
  for (const auto& entry : bindings_) {
    if (entry.live && entry.binding.flow == flow) return entry.binding;
  }
  return Status::of(ErrorCode::NotFound, "flow binding not found");
}

Status BindingRegistry::unbind(FlowId flow) {
  for (auto& entry : bindings_) {
    if (entry.live && entry.binding.flow == flow) {
      entry.live = false;
      return Status::success();
    }
  }
  return Status::of(ErrorCode::NotFound, "flow binding not found");
}

Status BindingRegistry::quiesce(FlowId flow, bool quiesced) {
  for (auto& entry : bindings_) {
    if (entry.live && entry.binding.flow == flow) {
      entry.binding.quiesced = quiesced;
      bool exhausted = false;
      entry.binding.generation = entry.binding.generation.next(exhausted);
      if (exhausted) return Status::of(ErrorCode::ResourceExhausted, "binding generation exhausted");
      return Status::success();
    }
  }
  return Status::of(ErrorCode::NotFound, "flow binding not found");
}

std::vector<FlowBinding> BindingRegistry::live_bindings() const {
  std::vector<FlowBinding> out;
  out.reserve(bindings_.size());
  for (const auto& entry : bindings_) {
    if (entry.live) out.push_back(entry.binding);
  }
  return out;
}

}  // namespace pacing
