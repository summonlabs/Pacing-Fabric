// Pacing Fabric - the *only* ingress for authorized rate/grant information.
//
// Rate authority is not pacing authority. This interface is read-only from the
// fabric's perspective: the fabric can observe an upstream ceiling, and can
// never produce, extend or repair one. A missing or non-authoritative grant
// stays UNKNOWN and no envelope can be derived from it.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_UPSTREAM_HPP
#define PACING_FABRIC_UPSTREAM_HPP

#include <string>

#include "pacing/id.hpp"
#include "pacing/status.hpp"
#include "pacing/time.hpp"

namespace pacing {

// A snapshot of upstream rate authority for one resource.
//
// This structure is *evidence*, not entitlement: possessing one grants no
// right to transmit, and the fabric never writes one back upstream.
struct RateGrant {
  GrantRef ref{};
  ResourceId resource{};
  u64 ceiling_bps{0};   // authorized ceiling; the hard upper bound
  u64 floor_bps{0};     // authorized floor; may be zero
  u64 revision{0};      // upstream-internal revision, for change detection
  Instant observed_at{};
  Deadline valid_until{};  // unset means "no stated expiry", not "infinite"
  ProvenanceId provenance{};
  // When false the snapshot is present but not authoritative: it must be
  // treated exactly like an absent grant.
  bool authoritative{false};

  [[nodiscard]] bool usable() const noexcept {
    return authoritative && ref.known() && resource.valid() && ceiling_bps != 0;
  }

  [[nodiscard]] bool expired_at(Instant now) const noexcept { return valid_until.expired_at(now); }

  // Canonical digest of the evidence, used to detect a silent upstream change
  // that did not bump the grant generation.
  [[nodiscard]] u64 digest() const noexcept;
};

// Narrow, vendor-neutral upstream rate authority. Implementations are expected
// to source this from an external arbiter; the fabric contributes no fallback.
class IRateAuthority {
 public:
  virtual ~IRateAuthority() = default;

  // Fetch current rate authority for a resource. Returns a non-Ok status when
  // the authority is unreachable - never a synthesized ceiling.
  [[nodiscard]] virtual StatusOr<RateGrant> fetch(ResourceId resource) const = 0;

  [[nodiscard]] virtual const char* name() const noexcept = 0;

  // True when this implementation is a software stand-in rather than a real
  // integration with an external arbiter. Propagated into every envelope and
  // every explanation so synthetic proof is never presented as real.
  [[nodiscard]] virtual bool synthetic() const noexcept = 0;
};

}  // namespace pacing

#endif  // PACING_FABRIC_UPSTREAM_HPP
