// Pacing Fabric test doubles.
//
// Every backend defined here is explicitly SYNTHETIC: it performs control-plane
// bookkeeping in this process and makes no claim about a physical mechanism.
// The REAL backend path is exercised only for label propagation and is
// documented as UNSUPPORTED without physical pacing hardware.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_TESTS_SUPPORT_HPP
#define PACING_FABRIC_TESTS_SUPPORT_HPP

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pacing/pacing.hpp"

namespace pf_test {

// RAII temporary directory. Removed recursively on destruction so a test run
// leaves no debris behind.
class TempDir {
 public:
  explicit TempDir(const std::string& label) {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t n = counter.fetch_add(1);
    path_ = (std::filesystem::temp_directory_path() /
             ("pacing_fabric_test_" + label + "_" + std::to_string(n)))
                .string();
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
};

// Deterministic monotonic clock that also lets a test model a clock that moves
// backwards, which the fabric must reject rather than absorb.
using ManualClock = pacing::ManualClock;

// Deterministic, scriptable upstream rate authority. It never synthesizes a
// ceiling the test did not install: an unset resource is an error, exactly as
// an unreachable external arbiter would be.
class ScriptedRateAuthority final : public pacing::IRateAuthority {
 public:
  struct Entry {
    pacing::RateGrant grant{};
    bool present{false};
    bool fail{false};
    pacing::ErrorCode fail_code{pacing::ErrorCode::UnknownAuthority};
  };

  void install(pacing::ResourceId resource, pacing::RateGrant grant) {
    std::lock_guard<std::mutex> lock(mu_);
    Entry entry{};
    entry.grant = grant;
    entry.grant.resource = resource;
    entry.present = true;
    entries_[resource.value()] = entry;
  }

  void set_failure(pacing::ResourceId resource, pacing::ErrorCode code) {
    std::lock_guard<std::mutex> lock(mu_);
    Entry& entry = entries_[resource.value()];
    entry.fail = true;
    entry.fail_code = code;
  }

  void clear_failure(pacing::ResourceId resource) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_[resource.value()].fail = false;
  }

  void remove(pacing::ResourceId resource) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.erase(resource.value());
  }

  [[nodiscard]] std::uint64_t fetch_count() const noexcept { return fetch_count_.load(); }

  [[nodiscard]] pacing::StatusOr<pacing::RateGrant> fetch(pacing::ResourceId resource) const override {
    fetch_count_.fetch_add(1);
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = entries_.find(resource.value());
    if (it == entries_.end() || !it->second.present) {
      return pacing::Status::of(pacing::ErrorCode::UnknownAuthority, "no rate authority installed");
    }
    if (it->second.fail) {
      return pacing::Status::of(it->second.fail_code, "scripted upstream failure");
    }
    return it->second.grant;
  }

  [[nodiscard]] const char* name() const noexcept override { return "scripted"; }
  [[nodiscard]] bool synthetic() const noexcept override { return true; }

 private:
  mutable std::mutex mu_;
  std::map<std::uint64_t, Entry> entries_;
  mutable std::atomic<std::uint64_t> fetch_count_{0};
};

// Software pacing backend used across the test suite. It records the cadence it
// was asked to install and reads that record back, so the fabric's
// acknowledgement-is-not-effect separation is exercised end to end.
class LoopbackBackend final : public pacing::IPacingBackend {
 public:
  struct Options {
    pacing::BackendNature nature{pacing::BackendNature::Synthetic};
    bool supports_readback{true};
    bool accept{true};
    bool fail_apply{false};
    pacing::ErrorCode apply_failure{pacing::ErrorCode::BackendFailure};
    bool fail_readback{false};
    bool drop_readback{false};
    bool corrupt_readback{false};
    bool false_acknowledge{false};  // accept, but install nothing
    std::int64_t rate_bias_bps{0};
    std::int64_t burst_bias_bytes{0};
    bool stall_apply{false};
    bool echo_wrong_attempt{false};
    bool readback_wrong_attempt{false};
    std::string name{"loopback-synthetic"};
  };

  explicit LoopbackBackend(pacing::BackendRef ref, Options options)
      : ref_(ref), options_(std::move(options)) {}

  void set_options(Options options) {
    std::lock_guard<std::mutex> lock(mu_);
    options_ = std::move(options);
  }

  [[nodiscard]] pacing::BackendDescriptor describe() const override {
    std::lock_guard<std::mutex> lock(mu_);
    pacing::BackendDescriptor d{};
    d.ref = ref_;
    d.name = options_.name;
    d.nature = options_.nature;
    d.supports_readback = options_.supports_readback;
    d.max_rate_bps = 400'000'000'000ull;
    d.max_burst_bytes = 1ull << 30;
    d.capability_flags = 1;
    return d;
  }

  [[nodiscard]] pacing::Status apply(const pacing::ApplyRequest& request, pacing::ApplyAck& ack) override {
    ack = pacing::ApplyAck{};
    ack.attempt = request.attempt;
    apply_entries_.fetch_add(1);
    {
      std::unique_lock<std::mutex> lock(mu_);
      if (options_.stall_apply) {
        stalled_.store(true);
        stalled_count_.fetch_add(1);
        stall_cv_.notify_all();
        gate_.wait(lock, [this] { return released_.load(); });
      }
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (options_.fail_apply) {
      ++rejected_;
      return pacing::Status::of(options_.apply_failure, "scripted apply failure");
    }
    if (!options_.accept) {
      ++rejected_;
      ack.accepted = false;
      ack.detail = "scripted rejection";
      return pacing::Status::success();
    }
    if (options_.false_acknowledge) {
      ++accepted_;
      ack.accepted = true;
      ack.backend_seq = ++sequence_;
      ack.detail = "accepted (nothing installed)";
      return pacing::Status::success();
    }
    if (options_.echo_wrong_attempt) {
      ack.attempt = pacing::AttemptId::from(request.attempt.value() + 1);
      ack.accepted = true;
      ack.backend_seq = ++sequence_;
      return pacing::Status::success();
    }
    Installed installed{};
    installed.attempt = request.attempt;
    installed.rate_bps = request.cadence.rate_bps;
    installed.quantum_bytes = request.cadence.quantum_bytes;
    installed.burst_bytes = request.cadence.burst_bytes;
    installed.interval_ns = request.cadence.interval_ns;
    installed.epoch = request.authority.epoch;
    installed.present = true;
    installed_[request.attempt.value()] = installed;
    ++accepted_;
    ack.accepted = true;
    ack.backend_seq = ++sequence_;
    ack.detail = "installed";
    return pacing::Status::success();
  }

  [[nodiscard]] pacing::Status readback(const pacing::ApplyRequest& request,
                                        pacing::ReadbackEvidence& evidence) override {
    evidence = pacing::ReadbackEvidence{};
    evidence.backend = ref_;
    evidence.attempt = options_.readback_wrong_attempt
                           ? pacing::AttemptId::from(request.attempt.value() + 7)
                           : request.attempt;
    evidence.observed_at_ns = request.issued_at_ns;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++readbacks_;
      if (options_.fail_readback) {
        return pacing::Status::of(pacing::ErrorCode::BackendFailure, "scripted readback failure");
      }
      if (options_.drop_readback) {
        evidence.present = false;
        return pacing::Status::success();
      }
    }
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = installed_.find(request.attempt.value());
    if (it == installed_.end() || !it->second.present) {
      evidence.present = false;
      return pacing::Status::success();
    }
    evidence.present = true;
    evidence.kind = options_.nature == pacing::BackendNature::Real
                        ? pacing::EvidenceKind::BackendReadback
                        : pacing::EvidenceKind::SyntheticReadback;
    evidence.observed_rate_bps =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(it->second.rate_bps) + options_.rate_bias_bps);
    evidence.observed_quantum_bytes = it->second.quantum_bytes;
    evidence.observed_burst_bytes = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(it->second.burst_bytes) + options_.burst_bias_bytes);
    evidence.observed_interval_ns = it->second.interval_ns;
    evidence.backend_seq = it->second.seq;
    evidence.integrity_ok = !options_.corrupt_readback;
    evidence.digest = 0;
    return pacing::Status::success();
  }

  [[nodiscard]] pacing::Status revoke(const pacing::ApplyRequest& request,
                                      std::string_view reason) override {
    (void)reason;
    std::lock_guard<std::mutex> lock(mu_);
    ++revokes_;
    const auto it = installed_.find(request.attempt.value());
    if (it != installed_.end()) it->second.present = false;
    return pacing::Status::success();
  }

  [[nodiscard]] pacing::Status rebind_epoch(pacing::Epoch new_epoch, std::uint64_t& discarded) override {
    std::lock_guard<std::mutex> lock(mu_);
    discarded = 0;
    for (auto& entry : installed_) {
      if (entry.second.present && entry.second.epoch < new_epoch) {
        entry.second.present = false;
        ++discarded;
      }
    }
    ++epoch_rebinds_;
    return pacing::Status::success();
  }

  [[nodiscard]] pacing::BackendStats stats() const override {
    std::lock_guard<std::mutex> lock(mu_);
    pacing::BackendStats s{};
    s.applies_accepted = accepted_;
    s.applies_rejected = rejected_;
    s.readbacks_served = readbacks_;
    s.revokes = revokes_;
    s.epoch_discards = epoch_rebinds_;
    return s;
  }

  // --- test controls ------------------------------------------------------
  void release_stall() {
    released_.store(true);
    gate_.notify_all();
  }

  void reset_stall() { released_.store(false); }

  // Blocks until an apply has actually entered the stall. Deterministic: no
  // polling and no timeout, the condition itself is the synchronisation.
  void wait_until_stalled() {
    std::unique_lock<std::mutex> lock(mu_);
    stall_cv_.wait(lock, [this] { return stalled_.load(); });
  }

  // Blocks until at least count applies have entered the stall.
  void wait_until_stalled(std::uint64_t count) {
    std::unique_lock<std::mutex> lock(mu_);
    stall_cv_.wait(lock, [this, count] { return stalled_count_.load() >= count; });
  }

  [[nodiscard]] bool stalled() const noexcept { return stalled_.load(); }
  [[nodiscard]] std::uint64_t apply_entries() const noexcept { return apply_entries_.load(); }
  [[nodiscard]] std::uint64_t accepted() const {
    std::lock_guard<std::mutex> lock(mu_);
    return accepted_;
  }
  [[nodiscard]] std::uint64_t revokes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return revokes_;
  }
  [[nodiscard]] bool installed(pacing::AttemptId id) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = installed_.find(id.value());
    return it != installed_.end() && it->second.present;
  }

 private:
  struct Installed {
    pacing::AttemptId attempt{};
    std::uint64_t rate_bps{0};
    std::uint64_t quantum_bytes{0};
    std::uint64_t burst_bytes{0};
    std::uint64_t interval_ns{0};
    std::uint64_t seq{0};
    pacing::Epoch epoch{};
    bool present{false};
  };

  pacing::BackendRef ref_;
  mutable std::mutex mu_;
  Options options_;
  std::unordered_map<std::uint64_t, Installed> installed_;
  std::condition_variable gate_;
  std::condition_variable stall_cv_;
  std::atomic<bool> released_{false};
  std::atomic<bool> stalled_{false};
  std::atomic<std::uint64_t> stalled_count_{0};
  std::atomic<std::uint64_t> apply_entries_{0};
  std::uint64_t accepted_{0};
  std::uint64_t rejected_{0};
  std::uint64_t readbacks_{0};
  std::uint64_t revokes_{0};
  std::uint64_t epoch_rebinds_{0};
  std::uint64_t sequence_{0};
};

// Records events so a test can assert on ordering and on the invariant that
// acknowledgement is not reported as an effect.
class RecordingSink final : public pacing::IEventSink {
 public:
  void on_event(const pacing::FabricEvent& event) noexcept override {
    try {
      std::lock_guard<std::mutex> lock(mu_);
      events_.push_back(event.kind);
      states_.push_back(event.state);
    } catch (...) {
    }
  }

  [[nodiscard]] std::size_t count(pacing::EventKind kind) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t n = 0;
    for (const auto k : events_) {
      if (k == kind) ++n;
    }
    return n;
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return events_.size();
  }

 private:
  mutable std::mutex mu_;
  std::vector<pacing::EventKind> events_;
  std::vector<pacing::ApplicationState> states_;
};

}  // namespace pf_test

#endif  // PACING_FABRIC_TESTS_SUPPORT_HPP
