// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/policy.hpp"

#include <algorithm>

namespace pacing {

std::string_view to_string(PacingLayer layer) noexcept {
  switch (layer) {
    case PacingLayer::Resource: return "resource";
    case PacingLayer::Flow: return "flow";
  }
  return "unknown";
}

bool parse_pacing_layer(std::string_view text, PacingLayer& out) noexcept {
  if (text == "resource") {
    out = PacingLayer::Resource;
    return true;
  }
  if (text == "flow") {
    out = PacingLayer::Flow;
    return true;
  }
  return false;
}

Status validate_policy(const PacingPolicy& policy, const Limits& limits) noexcept {
  if (!policy.ref.id.valid()) return Status::of(ErrorCode::InvalidArgument, "policy id is zero");
  if (!policy.ref.generation.known()) {
    return Status::of(ErrorCode::InvalidArgument, "policy generation is unknown");
  }
  if (policy.layer == PacingLayer::Resource && !policy.resource.valid()) {
    return Status::of(ErrorCode::InvalidArgument, "resource-layer policy has no resource");
  }
  if (policy.layer == PacingLayer::Flow && !policy.flow.valid()) {
    return Status::of(ErrorCode::InvalidArgument, "flow-layer policy has no flow");
  }
  if (!policy.path.known()) {
    return Status::of(ErrorCode::InvalidArgument, "policy path binding is not generation-bound");
  }
  if (policy.rate_share_ppm > 1'000'000ull) {
    return Status::of(ErrorCode::OutOfRange, "policy rate share exceeds 1000000 ppm");
  }
  if (policy.rate_bps > limits.max_rate_bps) {
    return Status::of(ErrorCode::OutOfRange, "policy rate above configured limit");
  }
  if (policy.min_rate_bps > limits.max_rate_bps) {
    return Status::of(ErrorCode::OutOfRange, "policy floor above configured limit");
  }
  if (policy.quantum_bytes == 0 || policy.quantum_bytes > limits.max_quantum_bytes) {
    return Status::of(ErrorCode::OutOfRange, "policy quantum out of range");
  }
  if (policy.window_ns < limits.min_window_ns || policy.window_ns > limits.max_window_ns) {
    return Status::of(ErrorCode::OutOfRange, "policy window out of range");
  }
  if (policy.burst_bytes > limits.max_burst_bytes) {
    return Status::of(ErrorCode::BurstExceedsBound, "policy burst bytes above limit");
  }
  if (policy.burst_packets > limits.max_burst_packets) {
    return Status::of(ErrorCode::BurstExceedsBound, "policy burst packets above limit");
  }
  if (policy.grants_per_window_hint > kNanosPerSecond) {
    return Status::of(ErrorCode::OutOfRange, "policy grants-per-window hint above bound");
  }
  if (policy.exceptions.size() > limits.max_service_class_exceptions) {
    return Status::of(ErrorCode::Oversized, "policy exception table above bound");
  }
  for (std::size_t i = 0; i < policy.exceptions.size(); ++i) {
    const auto& ex = policy.exceptions[i];
    if (!ex.service_class.valid()) {
      return Status::of(ErrorCode::InvalidArgument, "service class exception has zero id");
    }
    if (ex.rate_share_ppm > 1'000'000ull) {
      return Status::of(ErrorCode::OutOfRange, "exception rate share exceeds 1000000 ppm");
    }
    if (ex.min_rate_bps > limits.max_rate_bps) {
      return Status::of(ErrorCode::OutOfRange, "exception floor above configured limit");
    }
    if (ex.burst_bytes > limits.max_burst_bytes) {
      return Status::of(ErrorCode::BurstExceedsBound, "exception burst bytes above limit");
    }
    if (ex.burst_packets > limits.max_burst_packets) {
      return Status::of(ErrorCode::BurstExceedsBound, "exception burst packets above limit");
    }
    for (std::size_t j = i + 1; j < policy.exceptions.size(); ++j) {
      if (policy.exceptions[j].service_class == ex.service_class) {
        return Status::of(ErrorCode::InvalidArgument, "duplicate service class exception");
      }
    }
  }
  return Status::success();
}

const ServiceClassException* find_exception(const PacingPolicy& policy, ServiceClassId service_class) noexcept {
  if (!service_class.valid()) return nullptr;
  for (const auto& ex : policy.exceptions) {
    if (ex.service_class == service_class) return &ex;
  }
  return nullptr;
}

StatusOr<PolicyRef> PolicyStore::publish(PacingPolicy policy) {
  // A caller publishing a policy states its identity, not its revision: the
  // store assigns the first generation so a publication can never claim a
  // revision it did not receive.
  if (!policy.ref.generation.known()) policy.ref.generation = Generation::first();
  Status valid = validate_policy(policy, limits_);
  if (!valid) return valid;

  for (auto& entry : policies_) {
    if (entry.policy.ref.id == policy.ref.id) {
      if (!entry.live && policies_.size() >= limits_.max_policies) {
        return Status::of(ErrorCode::ResourceExhausted, "policy store is full");
      }
      // Republishing advances the generation; envelopes derived under the
      // previous generation become stale by construction.
      if (policy.ref.generation <= entry.policy.ref.generation) {
        bool exhausted = false;
        policy.ref.generation = entry.policy.ref.generation.next(exhausted);
        if (exhausted) return Status::of(ErrorCode::ResourceExhausted, "policy generation exhausted");
      }
      entry.policy = std::move(policy);
      entry.live = true;
      return entry.policy.ref;
    }
  }

  if (policies_.size() >= limits_.max_policies) {
    return Status::of(ErrorCode::ResourceExhausted, "policy store is full");
  }
  PolicyRef ref = policy.ref;
  policies_.push_back(Entry{std::move(policy), true});
  return ref;
}

Status PolicyStore::restore(PacingPolicy policy) {
  Status valid = validate_policy(policy, limits_);
  if (!valid) return valid;
  for (auto& entry : policies_) {
    if (entry.policy.ref.id == policy.ref.id) {
      entry.policy = std::move(policy);
      entry.live = true;
      return Status::success();
    }
  }
  if (policies_.size() >= limits_.max_policies) {
    return Status::of(ErrorCode::ResourceExhausted, "policy store is full");
  }
  policies_.push_back(Entry{std::move(policy), true});
  return Status::success();
}

std::vector<PacingPolicy> PolicyStore::live_policies() const {
  std::vector<PacingPolicy> out;
  out.reserve(policies_.size());
  for (const auto& entry : policies_) {
    if (entry.live) out.push_back(entry.policy);
  }
  return out;
}

StatusOr<PacingPolicy> PolicyStore::get(PolicyId id) const {
  for (const auto& entry : policies_) {
    if (entry.live && entry.policy.ref.id == id) return entry.policy;
  }
  return Status::of(ErrorCode::NotFound, "policy not found");
}

StatusOr<PacingPolicy> PolicyStore::resolve(FlowId flow, ResourceId resource) const {
  const PacingPolicy* best = nullptr;
  for (const auto& entry : policies_) {
    if (!entry.live) continue;
    if (!entry.policy.applies_to(flow, resource)) continue;
    if (entry.policy.layer == PacingLayer::Flow) {
      // A flow-layer policy always wins over a resource-layer policy.
      return entry.policy;
    }
    // Among resource policies for the same resource, the highest generation
    // is authoritative; equal generations cannot occur.
    if (best == nullptr || entry.policy.ref.generation > best->ref.generation) {
      best = &entry.policy;
    }
  }
  if (best != nullptr) return *best;
  return Status::of(ErrorCode::NotFound, "no policy governs this flow/resource binding");
}

Status PolicyStore::remove(PolicyId id) {
  for (auto& entry : policies_) {
    if (entry.live && entry.policy.ref.id == id) {
      entry.live = false;
      return Status::success();
    }
  }
  return Status::of(ErrorCode::NotFound, "policy not found");
}

}  // namespace pacing
