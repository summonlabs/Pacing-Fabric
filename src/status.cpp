// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/status.hpp"

#include <string>

namespace pacing {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::MalformedInput: return "MalformedInput";
    case ErrorCode::Oversized: return "Oversized";
    case ErrorCode::OutOfRange: return "OutOfRange";
    case ErrorCode::ArithmeticOverflow: return "ArithmeticOverflow";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::AlreadyExists: return "AlreadyExists";
    case ErrorCode::UnknownAuthority: return "UnknownAuthority";
    case ErrorCode::StaleGeneration: return "StaleGeneration";
    case ErrorCode::StaleEpoch: return "StaleEpoch";
    case ErrorCode::StaleBoot: return "StaleBoot";
    case ErrorCode::StalePolicy: return "StalePolicy";
    case ErrorCode::StaleRateGrant: return "StaleRateGrant";
    case ErrorCode::StalePath: return "StalePath";
    case ErrorCode::StaleResource: return "StaleResource";
    case ErrorCode::RequiresRevalidation: return "RequiresRevalidation";
    case ErrorCode::CeilingExceeded: return "CeilingExceeded";
    case ErrorCode::EntitlementMint: return "EntitlementMint";
    case ErrorCode::FloorExceedsCeiling: return "FloorExceedsCeiling";
    case ErrorCode::BurstExceedsBound: return "BurstExceedsBound";
    case ErrorCode::InvalidState: return "InvalidState";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::Revoked: return "Revoked";
    case ErrorCode::Fenced: return "Fenced";
    case ErrorCode::Duplicate: return "Duplicate";
    case ErrorCode::Ambiguous: return "Ambiguous";
    case ErrorCode::BackendUnavailable: return "BackendUnavailable";
    case ErrorCode::BackendRejected: return "BackendRejected";
    case ErrorCode::BackendFailure: return "BackendFailure";
    case ErrorCode::ReadbackUnavailable: return "ReadbackUnavailable";
    case ErrorCode::ReadbackMismatch: return "ReadbackMismatch";
    case ErrorCode::IntegrityFailure: return "IntegrityFailure";
    case ErrorCode::ResourceExhausted: return "ResourceExhausted";
    case ErrorCode::IoFailure: return "IoFailure";
    case ErrorCode::Internal: return "Internal";
    case ErrorCode::ClockRegression: return "ClockRegression";
  }
  return "Unknown";
}

bool is_retryable(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::BackendUnavailable:
    case ErrorCode::BackendFailure:
    case ErrorCode::ReadbackUnavailable:
    case ErrorCode::IoFailure:
    case ErrorCode::ResourceExhausted:
      return true;
    default:
      return false;
  }
}

bool is_staleness(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::StaleGeneration:
    case ErrorCode::StaleEpoch:
    case ErrorCode::StaleBoot:
    case ErrorCode::StalePolicy:
    case ErrorCode::StaleRateGrant:
    case ErrorCode::StalePath:
    case ErrorCode::StaleResource:
    case ErrorCode::RequiresRevalidation:
    case ErrorCode::Fenced:
      return true;
    default:
      return false;
  }
}

std::string Status::to_string() const {
  std::string out(pacing::to_string(code_));
  if (!detail_.empty()) {
    out += ": ";
    out.append(detail_);
  }
  return out;
}

}  // namespace pacing
