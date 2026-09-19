// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/id.hpp"

#include <string>

namespace pacing {

std::string_view to_string(AuthorityDrift drift) noexcept {
  switch (drift) {
    case AuthorityDrift::None: return "none";
    case AuthorityDrift::Flow: return "flow-generation";
    case AuthorityDrift::Resource: return "resource-generation";
    case AuthorityDrift::Policy: return "policy-generation";
    case AuthorityDrift::RateGrant: return "rate-grant-generation";
    case AuthorityDrift::Path: return "path-generation";
    case AuthorityDrift::Epoch: return "coordinator-epoch";
    case AuthorityDrift::CoordinatorBoot: return "coordinator-boot";
    case AuthorityDrift::Incomplete: return "incomplete-authority";
  }
  return "unknown";
}

AuthorityDrift compare_authority(const AuthorityVector& held, const AuthorityVector& current) noexcept {
  if (!held.complete()) return AuthorityDrift::Incomplete;
  if (held.flow != current.flow) return AuthorityDrift::Flow;
  if (held.resource != current.resource) return AuthorityDrift::Resource;
  if (held.policy != current.policy) return AuthorityDrift::Policy;
  if (held.rate_grant != current.rate_grant) return AuthorityDrift::RateGrant;
  if (held.path != current.path) return AuthorityDrift::Path;
  if (held.epoch != current.epoch) return AuthorityDrift::Epoch;
  if (!(held.coordinator_boot == current.coordinator_boot)) return AuthorityDrift::CoordinatorBoot;
  return AuthorityDrift::None;
}

std::string to_string(const AuthorityVector& authority) {
  std::string out;
  out.reserve(160);
  out += "flow=";
  out += pacing::to_string(authority.flow);
  out += " resource=";
  out += pacing::to_string(authority.resource);
  out += " policy=";
  out += pacing::to_string(authority.policy);
  out += " rate_grant=";
  out += pacing::to_string(authority.rate_grant);
  out += " path=";
  out += pacing::to_string(authority.path);
  out += " epoch=";
  out += std::to_string(authority.epoch.value());
  out += " boot=";
  out += std::to_string(authority.coordinator_boot.counter);
  out += ":";
  out += std::to_string(authority.coordinator_boot.nonce);
  return out;
}

}  // namespace pacing
