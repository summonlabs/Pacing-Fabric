// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/durable.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "pacing/checked.hpp"
#include "pacing/crc32c.hpp"
#include "pacing/version.hpp"

namespace pacing {
namespace {

constexpr u8 kSnapshotMagic[4] = {'P', 'F', 'S', 'N'};
constexpr u8 kJournalMagic[4] = {'P', 'F', 'J', '1'};

void put_u32(std::vector<u8>& out, std::size_t offset, u32 v) {
  out[offset + 0] = static_cast<u8>(v & 0xFF);
  out[offset + 1] = static_cast<u8>((v >> 8) & 0xFF);
  out[offset + 2] = static_cast<u8>((v >> 16) & 0xFF);
  out[offset + 3] = static_cast<u8>((v >> 24) & 0xFF);
}

void put_u64(std::vector<u8>& out, std::size_t offset, u64 v) {
  for (int i = 0; i < 8; ++i) out[offset + static_cast<std::size_t>(i)] = static_cast<u8>((v >> (8 * i)) & 0xFF);
}

u32 get_u32(const u8* p) {
  return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
         (static_cast<u32>(p[3]) << 24);
}

u64 get_u64(const u8* p) {
  u64 v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
  return v;
}

// Replaces a file atomically, retrying a bounded number of times. Windows can
// transiently refuse a replace while another process holds the destination
// open for reading (an indexer or a scanner, for example); retrying is a
// robustness measure, never a correctness shortcut, and the loop is bounded.
bool replace_atomically(const std::string& source, const std::string& target, std::string& error) {
  constexpr int kMaxAttempts = 8;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
#if defined(_WIN32)
    if (::MoveFileExA(source.c_str(), target.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
      return true;
    }
#else
    if (std::rename(source.c_str(), target.c_str()) == 0) return true;
#endif
    if (attempt + 1 < kMaxAttempts) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  error = "atomic snapshot replacement failed";
  return false;
}

bool flush_to_stable(std::FILE* f) {
  if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
  return ::_commit(::_fileno(f)) == 0;
#else
  return ::fsync(::fileno(f)) == 0;
#endif
}

void encode_record_header(u8* out, DurableRecordType type, u32 payload_len, u64 sequence, u64 timestamp,
                          u32 payload_crc) {
  out[0] = static_cast<u8>(static_cast<u16>(type) & 0xFF);
  out[1] = static_cast<u8>((static_cast<u16>(type) >> 8) & 0xFF);
  out[2] = 0;
  out[3] = 0;
  for (int i = 0; i < 4; ++i) out[4 + i] = static_cast<u8>((payload_len >> (8 * i)) & 0xFF);
  for (int i = 0; i < 8; ++i) out[8 + i] = static_cast<u8>((sequence >> (8 * i)) & 0xFF);
  for (int i = 0; i < 8; ++i) out[16 + i] = static_cast<u8>((timestamp >> (8 * i)) & 0xFF);
  for (int i = 0; i < 4; ++i) out[24 + i] = static_cast<u8>((payload_crc >> (8 * i)) & 0xFF);
  for (int i = 0; i < 4; ++i) out[28 + i] = 0;
}

}  // namespace

std::string_view to_string(DurableRecordType type) noexcept {
  switch (type) {
    case DurableRecordType::Meta: return "meta";
    case DurableRecordType::FlowBinding: return "flow-binding";
    case DurableRecordType::Policy: return "policy";
    case DurableRecordType::Envelope: return "envelope";
    case DurableRecordType::Attempt: return "attempt";
    case DurableRecordType::EnvelopeRevocation: return "envelope-revocation";
    case DurableRecordType::EpochAdvance: return "epoch-advance";
    case DurableRecordType::BackendRegistration: return "backend-registration";
    case DurableRecordType::FlowUnbound: return "flow-unbound";
    case DurableRecordType::PolicyRemoved: return "policy-removed";
  }
  return "unknown";
}

DurableStore::DurableStore(std::string directory, Limits limits)
    : directory_(std::move(directory)), limits_(limits) {}

DurableStore::~DurableStore() { (void)close(); }

std::string DurableStore::snapshot_path() const { return (std::filesystem::path(directory_) / "snapshot.pfs").string(); }
std::string DurableStore::journal_path() const { return (std::filesystem::path(directory_) / "journal.pfj").string(); }

bool DurableStore::is_open() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  return open_;
}

u64 DurableStore::journal_bytes() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  return journal_bytes_;
}

u64 DurableStore::journal_records() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  return journal_records_;
}

bool DurableStore::needs_checkpoint() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  return journal_records_ >= limits_.max_journal_records ||
         journal_bytes_ >= (limits_.max_journal_bytes * 3) / 4;
}

Status DurableStore::open() {
  std::lock_guard<std::mutex> lock(mu_);
  if (open_) return Status::of(ErrorCode::InvalidState, "durable store already open");
  if (directory_.empty()) return Status::of(ErrorCode::InvalidArgument, "durable directory is empty");

  std::error_code dir_ec;
  std::filesystem::create_directories(directory_, dir_ec);
  if (dir_ec && !std::filesystem::is_directory(directory_)) {
    return Status::of(ErrorCode::IoFailure, "cannot create durable directory");
  }

  recovered_.clear();
  report_ = DurabilityReport{};
  sequence_barrier_ = 0;

  Status snap = load_snapshot();
  if (!snap) {
    report_.load_code = snap.code();
    report_.detail = std::string(snap.detail());
    return snap;
  }
  Status jrn = load_journal();
  if (!jrn) {
    report_.load_code = jrn.code();
    report_.detail = std::string(jrn.detail());
    return jrn;
  }

  // Create the journal with a valid header when it does not yet exist.
  const std::string journal = journal_path();
  std::error_code exist_ec;
  const bool exists = std::filesystem::exists(journal, exist_ec) && !exist_ec;
  if (!exists) {
    std::FILE* created = std::fopen(journal.c_str(), "wb");
    if (created == nullptr) return Status::of(ErrorCode::IoFailure, "cannot create journal");
    u8 header[kJournalHeaderBytes] = {0};
    std::memcpy(header, kJournalMagic, 4);
    for (int i = 0; i < 4; ++i) {
      header[4 + static_cast<std::size_t>(i)] = static_cast<u8>((kStateFormatVersion >> (8 * i)) & 0xFF);
    }
    const bool wrote = std::fwrite(header, 1, kJournalHeaderBytes, created) == kJournalHeaderBytes;
    const bool flushed = wrote && flush_to_stable(created);
    if (std::fclose(created) != 0) {
      return Status::of(ErrorCode::IoFailure, "journal header was not made durable");
    }
    if (!flushed) return Status::of(ErrorCode::IoFailure, "journal header was not made durable");
  }

  std::FILE* f = std::fopen(journal.c_str(), "ab");
  if (f == nullptr) return Status::of(ErrorCode::IoFailure, "cannot open journal for append");
  journal_ = f;
  open_ = true;

  std::error_code size_ec;
  const auto size = std::filesystem::file_size(journal, size_ec);
  journal_bytes_ = size_ec ? static_cast<u64>(kJournalHeaderBytes) : static_cast<u64>(size);
  report_.load_code = ErrorCode::Ok;
  return Status::success();
}

Status DurableStore::load_snapshot() {
  const std::string path = snapshot_path();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    report_.snapshot_present = false;
    return Status::success();
  }
  report_.snapshot_present = true;

  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return Status::of(ErrorCode::IoFailure, "cannot open snapshot");

  std::vector<u8> bytes;
  u8 chunk[8192];
  std::size_t got = 0;
  while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
    if (bytes.size() + got > limits_.max_journal_bytes * 4) {
      std::fclose(f);
      return Status::of(ErrorCode::Oversized, "snapshot exceeds configured bound");
    }
    bytes.insert(bytes.end(), chunk, chunk + got);
  }
  std::fclose(f);

  if (bytes.size() < kSnapshotHeaderBytes) {
    return Status::of(ErrorCode::IntegrityFailure, "snapshot header is truncated");
  }
  if (std::memcmp(bytes.data(), kSnapshotMagic, 4) != 0) {
    return Status::of(ErrorCode::IntegrityFailure, "snapshot magic mismatch");
  }
  const u32 version = get_u32(bytes.data() + 4);
  if (version != kStateFormatVersion) {
    return Status::of(ErrorCode::Unsupported, "snapshot format version is not supported");
  }
  report_.format_version = version;
  const u64 record_count = get_u64(bytes.data() + 16);
  const u64 barrier = get_u64(bytes.data() + 24);
  const u32 expected_crc = get_u32(bytes.data() + 32);

  const std::span<const u8> body(bytes.data() + kSnapshotHeaderBytes, bytes.size() - kSnapshotHeaderBytes);
  const u32 actual_crc = crc32c(body);
  if (actual_crc != expected_crc) {
    return Status::of(ErrorCode::IntegrityFailure, "snapshot body checksum mismatch");
  }

  std::size_t offset = 0;
  for (u64 i = 0; i < record_count; ++i) {
    if (body.size() - offset < kRecordHeaderBytes) {
      return Status::of(ErrorCode::IntegrityFailure, "snapshot record header is truncated");
    }
    const u8* hdr = body.data() + offset;
    DurableRecord rec{};
    rec.type = static_cast<DurableRecordType>(static_cast<u16>(hdr[0]) | (static_cast<u16>(hdr[1]) << 8));
    const u32 payload_len = get_u32(hdr + 4);
    rec.sequence = get_u64(hdr + 8);
    rec.timestamp_ns = get_u64(hdr + 16);
    const u32 payload_crc = get_u32(hdr + 24);
    offset += kRecordHeaderBytes;
    if (payload_len > limits_.max_durable_record_bytes) {
      return Status::of(ErrorCode::Oversized, "snapshot record payload above bound");
    }
    if (body.size() - offset < payload_len) {
      return Status::of(ErrorCode::IntegrityFailure, "snapshot record payload is truncated");
    }
    const std::span<const u8> payload(body.data() + offset, payload_len);
    if (crc32c(payload) != payload_crc) {
      return Status::of(ErrorCode::IntegrityFailure, "snapshot record checksum mismatch");
    }
    rec.payload.assign(payload.begin(), payload.end());
    offset += payload_len;
    report_.snapshot_records += 1;
    recovered_.push_back(std::move(rec));
  }
  sequence_barrier_ = barrier;
  return Status::success();
}

Status DurableStore::load_journal() {
  const std::string path = journal_path();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    report_.journal_present = false;
    return Status::success();
  }
  report_.journal_present = true;

  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return Status::of(ErrorCode::IoFailure, "cannot open journal");
  std::vector<u8> bytes;
  u8 chunk[8192];
  std::size_t got = 0;
  while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
    if (bytes.size() + got > limits_.max_journal_bytes) {
      std::fclose(f);
      // A journal beyond its configured bound is not silently truncated: the
      // operator must intervene, because dropping records would lose authority
      // lineage that recovery depends on.
      return Status::of(ErrorCode::Oversized, "journal exceeds configured bound");
    }
    bytes.insert(bytes.end(), chunk, chunk + got);
  }
  std::fclose(f);

  if (bytes.empty()) return Status::success();
  if (bytes.size() < kJournalHeaderBytes) {
    report_.torn_tail_bytes = bytes.size();
    report_.detail = "journal header incomplete; treated as empty journal";
    return Status::success();
  }
  if (std::memcmp(bytes.data(), kJournalMagic, 4) != 0) {
    return Status::of(ErrorCode::IntegrityFailure, "journal magic mismatch");
  }
  const u32 version = get_u32(bytes.data() + 4);
  if (version != kStateFormatVersion) {
    return Status::of(ErrorCode::Unsupported, "journal format version is not supported");
  }
  report_.format_version = version;

  std::size_t offset = kJournalHeaderBytes;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    if (remaining < kRecordHeaderBytes) {
      report_.torn_tail_bytes += remaining;
      break;
    }
    const u8* hdr = bytes.data() + offset;
    const u16 raw_type = static_cast<u16>(hdr[0]) | (static_cast<u16>(hdr[1]) << 8);
    const u32 payload_len = get_u32(hdr + 4);
    const u64 sequence = get_u64(hdr + 8);
    const u64 timestamp = get_u64(hdr + 16);
    const u32 payload_crc = get_u32(hdr + 24);

    if (payload_len > limits_.max_durable_record_bytes) {
      report_.corrupt_records += 1;
      report_.torn_tail_bytes += remaining;
      report_.detail = "journal record payload above bound; replay stopped";
      break;
    }
    if (remaining - kRecordHeaderBytes < payload_len) {
      report_.torn_tail_bytes += remaining;
      break;
    }
    const std::span<const u8> payload(bytes.data() + offset + kRecordHeaderBytes, payload_len);
    if (crc32c(payload) != payload_crc) {
      report_.corrupt_records += 1;
      report_.torn_tail_bytes += remaining;
      report_.detail = "journal record checksum mismatch; replay stopped";
      break;
    }
    offset += kRecordHeaderBytes + payload_len;
    if (sequence <= sequence_barrier_) {
      report_.ignored_records += 1;
      continue;
    }
    DurableRecord rec{};
    rec.type = static_cast<DurableRecordType>(raw_type);
    rec.sequence = sequence;
    rec.timestamp_ns = timestamp;
    rec.payload.assign(payload.begin(), payload.end());
    report_.journal_records += 1;
    recovered_.push_back(std::move(rec));
  }
  return Status::success();
}

Status DurableStore::append(DurableRecord record) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!open_ || journal_ == nullptr) return Status::of(ErrorCode::InvalidState, "durable store is not open");
  if (record.payload.size() > limits_.max_durable_record_bytes) {
    return Status::of(ErrorCode::Oversized, "durable record above configured bound");
  }
  u32 payload_len = 0;
  if (!checked::narrow_u32(record.payload.size(), payload_len)) {
    return Status::of(ErrorCode::Oversized, "durable record payload length is not representable");
  }
  const u64 total = static_cast<u64>(kRecordHeaderBytes) + payload_len;
  if (journal_bytes_ + total > limits_.max_journal_bytes) {
    return Status::of(ErrorCode::ResourceExhausted, "journal is at its configured bound; checkpoint required");
  }

  u8 header[kRecordHeaderBytes];
  const u32 payload_crc = crc32c(record.payload);
  encode_record_header(header, record.type, payload_len, record.sequence, record.timestamp_ns, payload_crc);

  if (std::fwrite(header, 1, kRecordHeaderBytes, journal_) != kRecordHeaderBytes) {
    return Status::of(ErrorCode::IoFailure, "journal header write failed");
  }
  if (payload_len > 0 && std::fwrite(record.payload.data(), 1, payload_len, journal_) != payload_len) {
    return Status::of(ErrorCode::IoFailure, "journal payload write failed");
  }
  // The append is not acknowledged until the bytes are on stable storage.
  if (!flush_to_stable(journal_)) {
    return Status::of(ErrorCode::IoFailure, "journal flush failed");
  }
  journal_bytes_ += total;
  journal_records_ += 1;
  if (record.sequence > sequence_barrier_) sequence_barrier_ = record.sequence;
  return Status::success();
}

Status DurableStore::checkpoint(const std::vector<DurableRecord>& compacted) {
  std::vector<u8> body;
  body.reserve(compacted.size() * 64);
  u64 barrier = 0;
  u32 count = 0;
  for (const auto& rec : compacted) {
    if (rec.payload.size() > limits_.max_durable_record_bytes) {
      return Status::of(ErrorCode::Oversized, "compacted record above configured bound");
    }
    u32 payload_len = 0;
    if (!checked::narrow_u32(rec.payload.size(), payload_len)) {
      return Status::of(ErrorCode::Oversized, "compacted payload length is not representable");
    }
    if (count >= limits_.max_compaction_batch) {
      return Status::of(ErrorCode::ResourceExhausted, "compaction batch above configured bound");
    }
    body.resize(body.size() + kRecordHeaderBytes);
    u8* hdr = body.data() + body.size() - kRecordHeaderBytes;
    encode_record_header(hdr, rec.type, payload_len, rec.sequence, rec.timestamp_ns, crc32c(rec.payload));
    body.insert(body.end(), rec.payload.begin(), rec.payload.end());
    if (rec.sequence > barrier) barrier = rec.sequence;
    count += 1;
  }

  std::vector<u8> file;
  file.resize(kSnapshotHeaderBytes);
  std::memcpy(file.data(), kSnapshotMagic, 4);
  put_u32(file, 4, kStateFormatVersion);
  put_u32(file, 8, 0);
  put_u32(file, 12, 0);
  put_u64(file, 16, count);
  put_u64(file, 24, barrier);
  put_u32(file, 32, crc32c(body));
  put_u32(file, 36, 0);
  file.insert(file.end(), body.begin(), body.end());

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!open_) return Status::of(ErrorCode::InvalidState, "durable store is not open");

    const std::string tmp = snapshot_path() + ".tmp";
    std::FILE* out = std::fopen(tmp.c_str(), "wb");
    if (out == nullptr) return Status::of(ErrorCode::IoFailure, "cannot create snapshot temporary file");
    bool wrote = std::fwrite(file.data(), 1, file.size(), out) == file.size();
    bool flushed = wrote && flush_to_stable(out);
    if (std::fclose(out) != 0) flushed = false;
    if (!flushed) {
      std::error_code rmec;
      std::filesystem::remove(tmp, rmec);
      return Status::of(ErrorCode::IoFailure, "snapshot temporary file was not made durable");
    }

    std::string replace_error;
    if (!replace_atomically(tmp, snapshot_path(), replace_error)) {
      std::error_code rmec;
      std::filesystem::remove(tmp, rmec);
      return Status::of(ErrorCode::IoFailure, replace_error);
    }

    // Truncate the journal only after the new snapshot is durably in place.
    if (journal_ != nullptr) {
      std::fclose(journal_);
      journal_ = nullptr;
    }
    std::FILE* j = std::fopen(journal_path().c_str(), "wb");
    if (j == nullptr) return Status::of(ErrorCode::IoFailure, "cannot truncate journal");
    u8 jh[kJournalHeaderBytes] = {0};
    std::memcpy(jh, kJournalMagic, 4);
    for (int i = 0; i < 4; ++i) {
      jh[4 + static_cast<std::size_t>(i)] = static_cast<u8>((kStateFormatVersion >> (8 * i)) & 0xFF);
    }
    const bool ok = std::fwrite(jh, 1, kJournalHeaderBytes, j) == kJournalHeaderBytes && flush_to_stable(j);
    if (std::fclose(j) != 0 || !ok) {
      return Status::of(ErrorCode::IoFailure, "journal truncation was not made durable");
    }
    std::FILE* reopened = std::fopen(journal_path().c_str(), "ab");
    if (reopened == nullptr) return Status::of(ErrorCode::IoFailure, "cannot reopen journal");
    journal_ = reopened;
    journal_bytes_ = kJournalHeaderBytes;
    journal_records_ = 0;
    sequence_barrier_ = barrier;
    report_.snapshot_records = count;
  }
  return Status::success();
}

Status DurableStore::close() {
  std::lock_guard<std::mutex> lock(mu_);
  if (journal_ != nullptr) {
    (void)flush_to_stable(journal_);
    std::fclose(journal_);
    journal_ = nullptr;
  }
  open_ = false;
  return Status::success();
}

}  // namespace pacing
