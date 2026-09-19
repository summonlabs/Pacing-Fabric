// Real OS process control for the multiprocess coordinator tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_TESTS_MULTIPROC_PROCESS_HPP
#define PACING_FABRIC_TESTS_MULTIPROC_PROCESS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace pf_proc {

// Owns one child operating-system process. Waits are unbounded on purpose:
// a child that never exits is a defect to diagnose, not something to hide
// behind a watchdog.
class Child {
 public:
  Child() = default;
  ~Child();

  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  Child(Child&& other) noexcept;
  Child& operator=(Child&& other) noexcept;

  static bool spawn(const std::string& executable, const std::vector<std::string>& arguments,
                    Child& out);

  [[nodiscard]] bool valid() const noexcept { return process_ != nullptr; }
  [[nodiscard]] unsigned long pid() const noexcept { return pid_; }

  // True while the child process is still alive.
  [[nodiscard]] bool running() const;

  // Hard kill: no cooperative shutdown, no unwinding, no flush. Used to prove
  // that durable state and fencing survive an abrupt death.
  bool kill_hard();

  // Waits for the child and reports its exit code.
  bool wait(int& exit_code);

  void release();

 private:
  void close_handles();

#if defined(_WIN32)
  void* process_{nullptr};
#endif
  unsigned long pid_{0};
};

// Returns a loopback port that was free at the moment of the call.
std::uint16_t pick_free_loopback_port();

// Bounded readiness probe: waits for a file written by a starting child.
// Bounded by attempt count, never by wall-clock timeout semantics.
bool wait_for_file(const std::string& path, unsigned attempts, unsigned delay_ms);

// Reads a small text file into a string. Returns false when unreadable.
bool read_text_file(const std::string& path, std::string& out);

}  // namespace pf_proc

#endif  // PACING_FABRIC_TESTS_MULTIPROC_PROCESS_HPP
