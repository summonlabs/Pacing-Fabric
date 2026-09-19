// Pacing Fabric - versioned, integrity-checked, crash-safe durable state.
//
// Layout: one snapshot file plus one append-only journal.
//   snapshot.pfs : "PFSN" + version + record count + sequence barrier + body CRC-32C
//   journal.pfj  : "PFJ1" + version, followed by self-describing records
//
// A record is never acknowledged before it is on stable storage. Recovery
// replays a valid prefix and reports a torn tail explicitly; it never invents
// state for bytes it could not validate.
//
// The snapshot header carries the highest journal sequence it already
// includes. Replay discards any journal record at or below that barrier, so an
// interruption between the snapshot rename and the journal truncation cannot
// replay stale state over newer state.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_DURABLE_HPP
#define PACING_FABRIC_DURABLE_HPP

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "pacing/codec.hpp"
#include "pacing/limits.hpp"
#include "pacing/status.hpp"

namespace pacing {

enum class DurableRecordType : u16 {
  Meta = 1,
  FlowBinding = 2,
  Policy = 3,
  Envelope = 4,
  Attempt = 5,
  EnvelopeRevocation = 6,
  EpochAdvance = 7,
  BackendRegistration = 8,
  FlowUnbound = 9,
  PolicyRemoved = 10,
};

std::string_view to_string(DurableRecordType type) noexcept;

struct DurableRecord {
  DurableRecordType type{DurableRecordType::Meta};
  u64 sequence{0};
  u64 timestamp_ns{0};
  std::vector<u8> payload{};
};

struct DurabilityReport {
  bool snapshot_present{false};
  bool journal_present{false};
  u32 format_version{0};
  u64 snapshot_records{0};
  u64 journal_records{0};
  u64 corrupt_records{0};
  u64 torn_tail_bytes{0};
  u64 ignored_records{0};
  ErrorCode load_code{ErrorCode::Ok};
  std::string detail{};
};

// Crash-safe durable store. All mutation methods are synchronous and return
// only once the write has been flushed to stable storage.
class DurableStore {
 public:
  DurableStore(std::string directory, Limits limits);
  ~DurableStore();

  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;

  [[nodiscard]] Status open();
  Status close();
  [[nodiscard]] bool is_open() const noexcept;

  // Appends one record to the journal and flushes it. Returns IoFailure when
  // the bytes did not reach stable storage: a caller must not treat a failed
  // append as a committed mutation.
  [[nodiscard]] Status append(DurableRecord record);

  // Atomically replaces the snapshot with the supplied set and resets the
  // journal. The new snapshot is written to a temporary file, flushed,
  // renamed over the live path, and the journal is then truncated. An
  // interruption at any point leaves either the old or the new snapshot
  // complete and self-consistent.
  [[nodiscard]] Status checkpoint(const std::vector<DurableRecord>& compacted);

  [[nodiscard]] bool needs_checkpoint() const noexcept;

  [[nodiscard]] const std::vector<DurableRecord>& recovered() const noexcept { return recovered_; }
  [[nodiscard]] const DurabilityReport& report() const noexcept { return report_; }
  [[nodiscard]] u64 journal_bytes() const noexcept;
  [[nodiscard]] u64 journal_records() const noexcept;
  [[nodiscard]] const std::string& directory() const noexcept { return directory_; }

  [[nodiscard]] std::string snapshot_path() const;
  [[nodiscard]] std::string journal_path() const;

  static constexpr std::size_t kSnapshotHeaderBytes = 40;
  static constexpr std::size_t kJournalHeaderBytes = 16;
  static constexpr std::size_t kRecordHeaderBytes = 32;

 private:
  [[nodiscard]] Status load_snapshot();
  [[nodiscard]] Status load_journal();

  std::string directory_;
  Limits limits_;
  mutable std::mutex mu_;
  std::FILE* journal_{nullptr};
  u64 journal_bytes_{0};
  u64 journal_records_{0};
  u64 sequence_barrier_{0};
  bool open_{false};
  std::vector<DurableRecord> recovered_{};
  DurabilityReport report_{};
};

}  // namespace pacing

#endif  // PACING_FABRIC_DURABLE_HPP
