// Pacing Fabric - strongly typed identities, generations, epochs, boots and
// the exact authority vector that binds every decision to its evidence.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_ID_HPP
#define PACING_FABRIC_ID_HPP

#include <compare>
#include <cstdint>
#include <functional>
#include <string>

#include "pacing/checked.hpp"

namespace pacing {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

// ---------------------------------------------------------------------------
// Tag types. A tag exists only to make distinct identity domains
// type-incompatible: mixing a FlowId into a ResourceId slot cannot compile.
// ---------------------------------------------------------------------------
struct FlowTag {};
struct ResourceTag {};
struct PolicyTag {};
struct EnvelopeTag {};
struct BackendTag {};
struct AttemptTag {};
struct GrantTag {};
struct PathTag {};
struct ServiceClassTag {};
struct ProvenanceTag {};
struct TenantTag {};

// Zero is never a valid identity. Ids are minted from a monotonically
// increasing counter owned by the fabric instance that created them.
template <typename Tag>
class Id {
 public:
  using rep = u64;
  using tag = Tag;

  constexpr Id() noexcept = default;

  static constexpr Id from(rep value) noexcept {
    Id id;
    id.value_ = value;
    return id;
  }

  [[nodiscard]] constexpr rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(Id, Id) noexcept = default;
  friend constexpr auto operator<=>(Id, Id) noexcept = default;

 private:
  rep value_{0};
};

using FlowId = Id<FlowTag>;
using ResourceId = Id<ResourceTag>;
using PolicyId = Id<PolicyTag>;
using EnvelopeId = Id<EnvelopeTag>;
using BackendId = Id<BackendTag>;
using AttemptId = Id<AttemptTag>;
using GrantId = Id<GrantTag>;
using PathId = Id<PathTag>;
using ServiceClassId = Id<ServiceClassTag>;
using ProvenanceId = Id<ProvenanceTag>;
using TenantId = Id<TenantTag>;

// Monotonic revision counter attached to every identity that can be
// re-published. Generation 0 means UNKNOWN and never authorises anything.
class Generation {
 public:
  constexpr Generation() noexcept = default;

  static constexpr Generation unknown() noexcept { return Generation{}; }
  static constexpr Generation first() noexcept { return Generation::from(1); }
  static constexpr Generation from(u64 v) noexcept {
    Generation g;
    g.value_ = v;
    return g;
  }

  // Next generation, saturating at the maximum. Saturation is reported so a
  // caller can never mistake a wrapped counter for fresh authority.
  [[nodiscard]] constexpr Generation next(bool& exhausted) const noexcept {
    if (value_ == checked::kU64Max) {
      exhausted = true;
      return *this;
    }
    exhausted = false;
    return Generation::from(value_ + 1);
  }

  [[nodiscard]] constexpr u64 value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool known() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(Generation, Generation) noexcept = default;
  friend constexpr auto operator<=>(Generation, Generation) noexcept = default;

 private:
  u64 value_{0};
};

// Coordinator epoch. Advances on every coordinator incarnation that takes
// ownership of durable state. Work stamped with a lower epoch is fenced.
class Epoch {
 public:
  constexpr Epoch() noexcept = default;

  static constexpr Epoch none() noexcept { return Epoch{}; }
  static constexpr Epoch from(u64 v) noexcept {
    Epoch e;
    e.value_ = v;
    return e;
  }

  [[nodiscard]] constexpr u64 value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr Epoch advanced(bool& exhausted) const noexcept {
    if (value_ == checked::kU64Max) {
      exhausted = true;
      return *this;
    }
    exhausted = false;
    return Epoch::from(value_ + 1);
  }

  friend constexpr bool operator==(Epoch, Epoch) noexcept = default;
  friend constexpr auto operator<=>(Epoch, Epoch) noexcept = default;

 private:
  u64 value_{0};
};

// Process incarnation. The counter is durable and monotonic across restarts;
// the nonce distinguishes two processes that somehow observed the same
// counter. A boot identity is evidence of liveness only for the process that
// holds it - it is never restored from disk as current.
struct BootId {
  u64 counter{0};
  u64 nonce{0};

  [[nodiscard]] constexpr bool valid() const noexcept { return counter != 0 && nonce != 0; }

  friend constexpr bool operator==(const BootId&, const BootId&) noexcept = default;
};

// Worker/publisher incarnation. Distinguished from BootId so a coordinator
// boot can never be confused with a worker boot at the type level.
struct WorkerBoot {
  u64 incarnation{0};
  u64 nonce{0};

  [[nodiscard]] constexpr bool valid() const noexcept { return incarnation != 0 && nonce != 0; }

  friend constexpr bool operator==(const WorkerBoot&, const WorkerBoot&) noexcept = default;
};

// Identity plus generation. The generation is what makes a reference staleable;
// matching ids with mismatched generations is a staleness event, never a hit.
template <typename Tag>
struct GenRef {
  Id<Tag> id{};
  Generation generation{};

  [[nodiscard]] constexpr bool known() const noexcept { return id.valid() && generation.known(); }

  friend constexpr bool operator==(const GenRef&, const GenRef&) noexcept = default;
};

using FlowRef = GenRef<FlowTag>;
using ResourceRef = GenRef<ResourceTag>;
using PolicyRef = GenRef<PolicyTag>;
using GrantRef = GenRef<GrantTag>;
using PathRef = GenRef<PathTag>;
using BackendRef = GenRef<BackendTag>;
using EnvelopeRef = GenRef<EnvelopeTag>;

// The exact evidence set that justified an authorization decision.
//
// Every envelope, application record and durable journal entry carries one of
// these. Validation re-derives the vector from current authority and compares:
// any drift invalidates the decision rather than being silently tolerated.
struct AuthorityVector {
  FlowRef flow{};
  ResourceRef resource{};
  PolicyRef policy{};
  GrantRef rate_grant{};
  PathRef path{};
  Epoch epoch{};
  BootId coordinator_boot{};

  [[nodiscard]] constexpr bool complete() const noexcept {
    return flow.known() && resource.known() && policy.known() && rate_grant.known() && path.known() &&
           epoch.valid() && coordinator_boot.valid();
  }

  // Canonical digest over the full vector. Two vectors with equal digests are
  // bit-identical for every field the fabric observes.
  [[nodiscard]] u64 digest() const noexcept {
    Digest64 d;
    d.mix_u64(flow.id.value());
    d.mix_u64(flow.generation.value());
    d.mix_u64(resource.id.value());
    d.mix_u64(resource.generation.value());
    d.mix_u64(policy.id.value());
    d.mix_u64(policy.generation.value());
    d.mix_u64(rate_grant.id.value());
    d.mix_u64(rate_grant.generation.value());
    d.mix_u64(path.id.value());
    d.mix_u64(path.generation.value());
    d.mix_u64(epoch.value());
    d.mix_u64(coordinator_boot.counter);
    d.mix_u64(coordinator_boot.nonce);
    return d.value();
  }

  friend bool operator==(const AuthorityVector&, const AuthorityVector&) noexcept = default;
};

// Structured, machine-checkable description of why two authority vectors do
// not match. Deliberately exhaustive: callers must be able to explain a
// rejection without guessing.
enum class AuthorityDrift : u8 {
  None = 0,
  Flow,
  Resource,
  Policy,
  RateGrant,
  Path,
  Epoch,
  CoordinatorBoot,
  Incomplete,
};

std::string_view to_string(AuthorityDrift drift) noexcept;

AuthorityDrift compare_authority(const AuthorityVector& held, const AuthorityVector& current) noexcept;

// Human/JSON renderable identity helpers. Bounded output.
std::string to_string(const AuthorityVector& authority);

template <typename Tag>
std::string to_string(Id<Tag> id) {
  return std::to_string(id.value());
}

template <typename Tag>
std::string to_string(const GenRef<Tag>& ref) {
  return std::to_string(ref.id.value()) + "@" + std::to_string(ref.generation.value());
}

}  // namespace pacing

// std::hash specialisations so identities can key unordered containers without
// ever being flattened to a raw integer at a call site.
namespace std {

template <typename Tag>
struct hash<pacing::Id<Tag>> {
  size_t operator()(const pacing::Id<Tag>& id) const noexcept {
    return std::hash<pacing::Id<Tag>::rep>{}(id.value());
  }
};

template <typename Tag>
struct hash<pacing::GenRef<Tag>> {
  size_t operator()(const pacing::GenRef<Tag>& ref) const noexcept {
    size_t h = std::hash<pacing::Id<Tag>::rep>{}(ref.id.value());
    h ^= std::hash<pacing::Id<Tag>::rep>{}(ref.generation.value()) + 0x9E3779B97F4A7C15ull + (h << 6) +
         (h >> 2);
    return h;
  }
};

template <>
struct hash<pacing::AuthorityVector> {
  size_t operator()(const pacing::AuthorityVector& a) const noexcept {
    return static_cast<size_t>(a.digest());
  }
};

}  // namespace std

#endif  // PACING_FABRIC_ID_HPP
