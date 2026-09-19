// Pacing Fabric test framework - minimal, dependency-free, no timeouts.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_TESTS_FRAMEWORK_HPP
#define PACING_FABRIC_TESTS_FRAMEWORK_HPP

#include <cstdint>
#include <exception>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "pacing/id.hpp"

namespace tf {

struct TestCase {
  const char* suite;
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

struct Registrar {
  Registrar(const char* suite, const char* name, void (*fn)()) { registry().push_back({suite, name, fn}); }
};

class Failure : public std::exception {
 public:
  explicit Failure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

inline void fail(const char* file, int line, const std::string& message) {
  std::ostringstream os;
  os << file << ":" << line << ": " << message;
  throw Failure(os.str());
}

template <typename Tag>
std::string describe(pacing::Id<Tag> value) {
  return std::to_string(value.value());
}

template <typename Tag>
std::string describe(pacing::GenRef<Tag> value) {
  return std::to_string(value.id.value()) + "@" + std::to_string(value.generation.value());
}

// Renders a value for failure messages. Enums print numerically, streamable
// types print through operator<<, and anything else degrades to a placeholder
// instead of failing to compile.
template <typename T>
std::string describe(const T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_same_v<T, std::string>) {
    return "\"" + value + "\"";
  } else if constexpr (std::is_same_v<T, std::string_view>) {
    return "\"" + std::string(value) + "\"";
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (requires(std::ostream& os, const T& v) { os << v; }) {
    std::ostringstream os;
    os << value;
    return os.str();
  } else {
    return "<unprintable>";
  }
}

// Command-line options supplied as --key value pairs. Used by test suites that
// need an external artifact, such as the path to the coordinator daemon.
void set_option(const std::string& key, const std::string& value);
std::string option(const std::string& key, const std::string& fallback = std::string{});

int run_all(const char* filter);

// Deterministic xorshift64* generator. Every randomized test seeds it with an
// explicit constant so a failure is exactly reproducible.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() {
    std::uint64_t x = state_;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state_ = x;
    return x * 0x2545F4914F6CDD1Dull;
  }

  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

  std::uint64_t range(std::uint64_t lo, std::uint64_t hi) {
    if (hi <= lo) return lo;
    return lo + below(hi - lo + 1);
  }

  bool coin() { return (next() & 1u) != 0; }

 private:
  std::uint64_t state_;
};

}  // namespace tf

#define PF_TEST(suite, name)                                                                     \
  static void pf_test_##suite##_##name();                                                        \
  [[maybe_unused]] static const ::tf::Registrar pf_reg_##suite##_##name(#suite, #name,           \
                                                                       &pf_test_##suite##_##name); \
  static void pf_test_##suite##_##name()

#define PF_CHECK(cond)                                                             \
  do {                                                                             \
    if (!(cond)) ::tf::fail(__FILE__, __LINE__, std::string("CHECK failed: ") + #cond); \
  } while (false)

#define PF_CHECK_EQ(actual, expected)                                                          \
  do {                                                                                         \
    const auto& pf_a = (actual);                                                               \
    const auto& pf_b = (expected);                                                             \
    if (!(pf_a == pf_b)) {                                                                     \
      ::tf::fail(__FILE__, __LINE__, std::string("CHECK_EQ failed: " #actual " == " #expected \
                                                 " (actual=") +                              \
                                         ::tf::describe(pf_a) + " expected=" +               \
                                         ::tf::describe(pf_b) + ")");                        \
    }                                                                                          \
  } while (false)

#define PF_CHECK_NE(actual, unexpected)                                                     \
  do {                                                                                      \
    if ((actual) == (unexpected)) {                                                         \
      ::tf::fail(__FILE__, __LINE__, std::string("CHECK_NE failed: " #actual " != " #unexpected)); \
    }                                                                                       \
  } while (false)

#define PF_CHECK_OK(expr)                                                              \
  do {                                                                                 \
    const auto pf_s = (expr);                                                          \
    if (!pf_s.ok()) {                                                                  \
      ::tf::fail(__FILE__, __LINE__, std::string("expected success from " #expr        \
                                                 " but got ") +                        \
                                         std::string(::pacing::to_string(pf_s.code())) + \
                                         ": " + std::string(pf_s.detail()));           \
    }                                                                                  \
  } while (false)

#define PF_CHECK_STATUS_OK(expr)                                                       \
  do {                                                                                 \
    const auto& pf_r = (expr);                                                         \
    if (!pf_r.status().ok()) {                                                         \
      ::tf::fail(__FILE__, __LINE__, std::string("expected success from " #expr        \
                                                 " but got ") +                        \
                                         std::string(::pacing::to_string(pf_r.status().code())) + \
                                         ": " + std::string(pf_r.status().detail()));  \
    }                                                                                  \
  } while (false)

#define PF_CHECK_CODE(expr, expected_code)                                                      \
  do {                                                                                          \
    const auto& pf_r = (expr);                                                                  \
    if (pf_r.status().code() != (expected_code)) {                                              \
      ::tf::fail(__FILE__, __LINE__,                                                            \
                 std::string("expected " #expr " to fail with " #expected_code " but got ") +  \
                     std::string(::pacing::to_string(pf_r.status().code())) + ": " +            \
                     std::string(pf_r.status().detail()));                                      \
    }                                                                                           \
  } while (false)

// For control-plane responses, whose outcome is a data member rather than a
// Status object.
#define PF_CHECK_RESPONSE(expr, expected_code)                                                 \
  do {                                                                                         \
    const auto& pf_r = (expr);                                                                 \
    if (pf_r.code != (expected_code)) {                                                        \
      ::tf::fail(__FILE__, __LINE__,                                                           \
                 std::string("expected " #expr " == " #expected_code " but got ") +           \
                     std::string(::pacing::to_string(pf_r.code)) + ": " + pf_r.detail);        \
    }                                                                                          \
  } while (false)

#define PF_CHECK_STATUS_CODE(expr, expected_code)                                              \
  do {                                                                                         \
    const auto pf_s = (expr);                                                                  \
    if (pf_s.code() != (expected_code)) {                                                      \
      ::tf::fail(__FILE__, __LINE__,                                                           \
                 std::string("expected " #expr " == " #expected_code " but got ") +           \
                     std::string(::pacing::to_string(pf_s.code())) + ": " +                    \
                     std::string(pf_s.detail()));                                              \
    }                                                                                          \
  } while (false)

#endif  // PACING_FABRIC_TESTS_FRAMEWORK_HPP
