// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/backends.hpp"

#include <utility>

namespace pacing {

SyntheticPacingBackend::SyntheticPacingBackend(BackendRef ref, std::string name)
    : ref_(ref), name_(std::move(name)) {}

BackendDescriptor SyntheticPacingBackend::describe() const {
  BackendDescriptor descriptor{};
  std::lock_guard<std::mutex> lock(mu_);
  descriptor.ref = ref_;
  descriptor.name = name_;
  descriptor.nature = BackendNature::Synthetic;
  descriptor.supports_readback = true;
  descriptor.max_rate_bps = 400'000'000'000ull;
  descriptor.max_burst_bytes = 1ull << 30;
  descriptor.capability_flags = 1;
  return descriptor;
}

Status SyntheticPacingBackend::apply(const ApplyRequest& request, ApplyAck& ack) {
  ack = ApplyAck{};
  ack.attempt = request.attempt;
  if (request.cadence.rate_bps == 0 || request.cadence.interval_ns == 0) {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.applies_rejected += 1;
    ack.accepted = false;
    ack.detail = "cadence is not installable";
    return Status::success();
  }
  std::lock_guard<std::mutex> lock(mu_);
  Installed installed{};
  installed.rate_bps = request.cadence.rate_bps;
  installed.quantum_bytes = request.cadence.quantum_bytes;
  installed.burst_bytes = request.cadence.burst_bytes;
  installed.interval_ns = request.cadence.interval_ns;
  installed.epoch = request.authority.epoch;
  installed.backend_seq = ++sequence_;
  installed_[request.attempt.value()] = installed;
  stats_.applies_accepted += 1;
  ack.accepted = true;
  ack.backend_seq = installed.backend_seq;
  ack.detail = "installed (synthetic shaper model)";
  return Status::success();
}

Status SyntheticPacingBackend::readback(const ApplyRequest& request, ReadbackEvidence& evidence) {
  evidence = ReadbackEvidence{};
  evidence.backend = ref_;
  evidence.attempt = request.attempt;
  evidence.observed_at_ns = request.issued_at_ns;
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = installed_.find(request.attempt.value());
  if (it == installed_.end()) {
    stats_.readbacks_unavailable += 1;
    evidence.present = false;
    return Status::success();
  }
  evidence.present = true;
  evidence.kind = EvidenceKind::SyntheticReadback;
  evidence.observed_rate_bps = it->second.rate_bps;
  evidence.observed_quantum_bytes = it->second.quantum_bytes;
  evidence.observed_burst_bytes = it->second.burst_bytes;
  evidence.observed_interval_ns = it->second.interval_ns;
  evidence.backend_seq = it->second.backend_seq;
  evidence.integrity_ok = true;
  evidence.digest = 0;
  stats_.readbacks_served += 1;
  return Status::success();
}

Status SyntheticPacingBackend::revoke(const ApplyRequest& request, std::string_view reason) {
  (void)reason;
  std::lock_guard<std::mutex> lock(mu_);
  // Revocation is idempotent: withdrawing something that was never installed is
  // a success, and the attempt is recorded as withdrawn either way.
  installed_.erase(request.attempt.value());
  ++stats_.revokes;
  return Status::success();
}

Status SyntheticPacingBackend::rebind_epoch(Epoch new_epoch, u64& discarded) {
  std::lock_guard<std::mutex> lock(mu_);
  discarded = 0;
  for (auto it = installed_.begin(); it != installed_.end();) {
    if (it->second.epoch < new_epoch) {
      it = installed_.erase(it);
      ++discarded;
    } else {
      ++it;
    }
  }
  stats_.epoch_discards += discarded;
  return Status::success();
}

BackendStats SyntheticPacingBackend::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

std::size_t SyntheticPacingBackend::installed_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return installed_.size();
}

bool SyntheticPacingBackend::installed(AttemptId attempt) const {
  std::lock_guard<std::mutex> lock(mu_);
  return installed_.find(attempt.value()) != installed_.end();
}

void StaticRateAuthority::install(RateGrant grant) {
  std::lock_guard<std::mutex> lock(mu_);
  Entry entry{};
  entry.grant = std::move(grant);
  entry.present = true;
  entry.unavailable = false;
  entries_[entry.grant.resource.value()] = entry;
}

void StaticRateAuthority::remove(ResourceId resource) {
  std::lock_guard<std::mutex> lock(mu_);
  entries_.erase(resource.value());
}

void StaticRateAuthority::set_unavailable(ResourceId resource, bool unavailable) {
  std::lock_guard<std::mutex> lock(mu_);
  entries_[resource.value()].unavailable = unavailable;
}

StatusOr<RateGrant> StaticRateAuthority::fetch(ResourceId resource) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = entries_.find(resource.value());
  if (it == entries_.end() || !it->second.present) {
    return Status::of(ErrorCode::UnknownAuthority, "no rate grant configured for this resource");
  }
  if (it->second.unavailable) {
    return Status::of(ErrorCode::BackendUnavailable, "configured rate authority is marked unavailable");
  }
  return it->second.grant;
}

}  // namespace pacing
