// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/backend.hpp"

namespace pacing {

std::string_view to_string(BackendNature nature) noexcept {
  switch (nature) {
    case BackendNature::Real: return "REAL";
    case BackendNature::Synthetic: return "SYNTHETIC";
  }
  return "UNKNOWN";
}

std::string_view to_string(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::None: return "NONE";
    case EvidenceKind::BackendReadback: return "BACKEND_READBACK";
    case EvidenceKind::SyntheticReadback: return "SYNTHETIC_READBACK";
    case EvidenceKind::Injected: return "INJECTED";
  }
  return "NONE";
}

}  // namespace pacing
