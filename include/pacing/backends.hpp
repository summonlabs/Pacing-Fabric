// Pacing Fabric - bundled backend and rate-authority implementations.
//
// Everything in this header is SYNTHETIC. These types perform control-plane
// bookkeeping inside this process; they do not drive a network device and make
// no packet-rate claim. A REAL backend is a vendor integration that satisfies
// IPacingBackend and reports BackendNature::Real together with readback
// evidence from the actual mechanism. No such integration ships here, so the
// REAL path is UNSUPPORTED in this repository.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_BACKENDS_HPP
#define PACING_FABRIC_BACKENDS_HPP

#include <mutex>
#include <string>
#include <unordered_map>

#include "pacing/backend.hpp"
#include "pacing/upstream.hpp"

namespace pacing {

// Software pacing shaper model. It stores the cadence it was asked to install
// and reads exactly that back, which makes the acknowledgement/effect
// separation observable end to end. Labelled SYNTHETIC everywhere it appears.
class SyntheticPacingBackend final : public IPacingBackend {
 public:
  SyntheticPacingBackend(BackendRef ref, std::string name);
  ~SyntheticPacingBackend() override = default;

  SyntheticPacingBackend(const SyntheticPacingBackend&) = delete;
  SyntheticPacingBackend& operator=(const SyntheticPacingBackend&) = delete;

  [[nodiscard]] BackendDescriptor describe() const override;
  [[nodiscard]] Status apply(const ApplyRequest& request, ApplyAck& ack) override;
  [[nodiscard]] Status readback(const ApplyRequest& request, ReadbackEvidence& evidence) override;
  [[nodiscard]] Status revoke(const ApplyRequest& request, std::string_view reason) override;
  [[nodiscard]] Status rebind_epoch(Epoch new_epoch, u64& discarded) override;
  [[nodiscard]] BackendStats stats() const override;

  // Number of cadences currently installed. Used by operators and tests to see
  // whether a withdrawal actually took effect.
  [[nodiscard]] std::size_t installed_count() const;
  [[nodiscard]] bool installed(AttemptId attempt) const;

 private:
  struct Installed {
    u64 rate_bps{0};
    u64 quantum_bytes{0};
    u64 burst_bytes{0};
    u64 interval_ns{0};
    u64 backend_seq{0};
    Epoch epoch{};
  };

  BackendRef ref_;
  std::string name_;
  mutable std::mutex mu_;
  std::unordered_map<u64, Installed> installed_;
  BackendStats stats_{};
  u64 sequence_{0};
};

// Locally configured upstream rate authority. This is a stand-in for an
// external arbiter: it is explicitly synthetic, it never invents a ceiling for
// a resource it was not told about, and the fabric can only read from it.
class StaticRateAuthority final : public IRateAuthority {
 public:
  StaticRateAuthority() = default;

  void install(RateGrant grant);
  void remove(ResourceId resource);
  void set_unavailable(ResourceId resource, bool unavailable);

  [[nodiscard]] StatusOr<RateGrant> fetch(ResourceId resource) const override;
  [[nodiscard]] const char* name() const noexcept override { return "static-configured"; }
  [[nodiscard]] bool synthetic() const noexcept override { return true; }

 private:
  struct Entry {
    RateGrant grant{};
    bool present{false};
    bool unavailable{false};
  };
  mutable std::mutex mu_;
  std::unordered_map<u64, Entry> entries_;
};

}  // namespace pacing

#endif  // PACING_FABRIC_BACKENDS_HPP
