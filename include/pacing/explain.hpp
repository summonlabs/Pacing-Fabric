// Pacing Fabric - bounded, deterministic explanation of a pacing decision.
//
// Every refusal carries enough structure to answer "why" without re-running
// the fabric, and every rendering is bounded so a hostile input cannot turn
// an explanation into an unbounded allocation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_EXPLAIN_HPP
#define PACING_FABRIC_EXPLAIN_HPP

#include <cstddef>
#include <string>

#include "pacing/application.hpp"
#include "pacing/binding.hpp"
#include "pacing/upstream.hpp"

namespace pacing {

struct Explanation {
  FlowId flow{};
  Generation flow_generation{};
  ResourceId resource{};

  bool has_binding{false};
  bool has_envelope{false};
  bool has_attempt{false};
  bool has_grant{false};

  FlowBinding binding{};
  PacingEnvelope envelope{};
  ApplicationRecord attempt{};
  RateGrant grant{};

  AuthorityVector current_authority{};
  AuthorityDrift drift{AuthorityDrift::None};

  // The effect the fabric is willing to assert right now. None means UNKNOWN.
  EffectLabel effect{EffectLabel::None};
  ErrorCode refusal{ErrorCode::Ok};
  std::string refusal_detail{};
  std::string stale_or_revoke_reason{};

  u64 burst_budget_bytes{0};
  u64 burst_budget_packets{0};

  [[nodiscard]] std::string to_text(std::size_t max_bytes) const;
  [[nodiscard]] std::string to_json(std::size_t max_bytes) const;
};

// Truncates a rendered explanation at max_bytes, appending an explicit marker
// so a truncated explanation is never mistaken for a complete one.
std::string bound_explanation(std::string text, std::size_t max_bytes);

}  // namespace pacing

#endif  // PACING_FABRIC_EXPLAIN_HPP
