// Pacing Fabric - status, error classification and result plumbing.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_STATUS_HPP
#define PACING_FABRIC_STATUS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace pacing {

// Bounded, closed error taxonomy. Every rejection the fabric can produce maps
// to exactly one code so that callers never have to parse prose.
enum class ErrorCode : std::uint16_t {
  Ok = 0,
  // --- input shape -------------------------------------------------------
  InvalidArgument,
  MalformedInput,
  Oversized,
  OutOfRange,
  ArithmeticOverflow,
  Unsupported,
  // --- identity / lookup ------------------------------------------------
  NotFound,
  AlreadyExists,
  UnknownAuthority,
  // --- generation and authority binding ---------------------------------
  StaleGeneration,
  StaleEpoch,
  StaleBoot,
  StalePolicy,
  StaleRateGrant,
  StalePath,
  StaleResource,
  RequiresRevalidation,
  // --- pacing policy authority ------------------------------------------
  CeilingExceeded,
  EntitlementMint,
  FloorExceedsCeiling,
  BurstExceedsBound,
  // --- lifecycle --------------------------------------------------------
  InvalidState,
  Cancelled,
  Revoked,
  Fenced,
  Duplicate,
  Ambiguous,
  // --- backend / effect -------------------------------------------------
  BackendUnavailable,
  BackendRejected,
  BackendFailure,
  ReadbackUnavailable,
  ReadbackMismatch,
  IntegrityFailure,
  // --- resources / environment ------------------------------------------
  ResourceExhausted,
  IoFailure,
  Internal,
  // The injected clock moved backwards. Time is not trustworthy, so no pacing
  // decision may be derived from it until it recovers.
  ClockRegression,
};

std::string_view to_string(ErrorCode code) noexcept;

// True when the code denotes a condition the caller could resolve by
// re-obtaining authority and retrying from scratch.
bool is_retryable(ErrorCode code) noexcept;

// True when the code denotes that previously authoritative evidence was
// invalidated (generation/epoch/policy/rate/path drift).
bool is_staleness(ErrorCode code) noexcept;

// Canonical, bounded status object. The message is advisory; the code is
// authoritative and is what the fabric branches on.
class Status {
 public:
  Status() noexcept = default;
  Status(ErrorCode code, std::string_view detail) : code_(code), detail_(detail) {}

  // Factory for a successful status. Deliberately not named ok(): ok() is the
  // query, and overloading the two would be ambiguous at every call site.
  static Status success() noexcept { return Status{}; }
  static Status of(ErrorCode code, std::string_view detail = {}) { return Status(code, detail); }

  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] std::string_view detail() const noexcept { return detail_; }
  [[nodiscard]] std::string to_string() const;

  explicit operator bool() const noexcept { return ok(); }

  friend bool operator==(const Status& a, const Status& b) noexcept { return a.code_ == b.code_; }

 private:
  ErrorCode code_{ErrorCode::Ok};
  std::string detail_{};
};

// Result monad. Holds either a value or a non-Ok Status. There is no
// "value with error" state and no default-constructed success.
template <typename T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}         // NOLINT(google-explicit-constructor)
  Result(Status status) : storage_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept {
    static const Status kOk{};
    return ok() ? kOk : std::get<Status>(storage_);
  }
  [[nodiscard]] ErrorCode code() const noexcept { return status().code(); }

  [[nodiscard]] T& value() & { return std::get<T>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }

  [[nodiscard]] T value_or(T fallback) const {
    return ok() ? std::get<T>(storage_) : std::move(fallback);
  }

 private:
  std::variant<T, Status> storage_;
};

// Result<void> specialisation used for fallible operations with no payload.
template <>
class Result<void> {
 public:
  Result() = default;
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] ErrorCode code() const noexcept { return status_.code(); }

 private:
  Status status_{};
};

using VoidResult = Result<void>;

template <typename T>
using StatusOr = Result<T>;

}  // namespace pacing

#endif  // PACING_FABRIC_STATUS_HPP
