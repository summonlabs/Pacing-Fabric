// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/upstream.hpp"

#include "pacing/checked.hpp"

namespace pacing {

u64 RateGrant::digest() const noexcept {
  Digest64 d;
  d.mix_u64(ref.id.value());
  d.mix_u64(ref.generation.value());
  d.mix_u64(resource.value());
  d.mix_u64(ceiling_bps);
  d.mix_u64(floor_bps);
  d.mix_u64(revision);
  d.mix_u64(observed_at.ns());
  d.mix_bool(valid_until.is_set());
  d.mix_u64(valid_until.is_set() ? valid_until.instant().ns() : 0);
  d.mix_u64(provenance.value());
  d.mix_bool(authoritative);
  return d.value();
}

}  // namespace pacing
