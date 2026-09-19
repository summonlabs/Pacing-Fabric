// Pacing Fabric - bounded, deterministic binary encoding.
//
// Every externally supplied byte string is decoded through ByteReader, which
// bounds-checks each field and refuses lossy or oversized input instead of
// trusting a length prefix. Encoding is stable and endian-explicit so that
// durable state and wire frames are reproducible across builds.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_CODEC_HPP
#define PACING_FABRIC_CODEC_HPP

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pacing/checked.hpp"
#include "pacing/status.hpp"

namespace pacing {

// Append-only little-endian writer with a hard capacity bound. Exceeding the
// bound poisons the writer rather than silently growing without limit.
class ByteWriter {
 public:
  explicit ByteWriter(std::size_t capacity) : capacity_(capacity) { buf_.reserve(capacity < 4096 ? capacity : 4096); }

  void u8v(u8 v) { raw(&v, 1); }

  void u16v(u16 v) {
    u8 b[2] = {static_cast<u8>(v & 0xFF), static_cast<u8>((v >> 8) & 0xFF)};
    raw(b, 2);
  }

  void u32v(u32 v) {
    u8 b[4] = {static_cast<u8>(v & 0xFF), static_cast<u8>((v >> 8) & 0xFF), static_cast<u8>((v >> 16) & 0xFF),
               static_cast<u8>((v >> 24) & 0xFF)};
    raw(b, 4);
  }

  void u64v(u64 v) {
    u8 b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<u8>((v >> (8 * i)) & 0xFF);
    raw(b, 8);
  }

  void boolean(bool v) { u8v(v ? 1u : 0u); }

  // Length-prefixed string. Rejects strings above the writer capacity.
  void str(std::string_view s) {
    if (poisoned_) return;
    if (s.size() > kMaxStringBytes) {
      poison(Status::of(ErrorCode::Oversized, "string exceeds codec bound"));
      return;
    }
    u32v(static_cast<u32>(s.size()));
    raw(reinterpret_cast<const u8*>(s.data()), s.size());
  }

  // Length-prefixed opaque blob.
  void blob(std::span<const u8> b) {
    if (poisoned_) return;
    u32v(static_cast<u32>(b.size()));
    raw(b.data(), b.size());
  }

  [[nodiscard]] bool ok() const noexcept { return !poisoned_; }
  [[nodiscard]] const Status& status() const noexcept { return poison_; }
  [[nodiscard]] const std::vector<u8>& bytes() const noexcept { return buf_; }
  [[nodiscard]] std::vector<u8> take() { return std::move(buf_); }
  [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }

  static constexpr std::size_t kMaxStringBytes = 1u << 20;

 private:
  void raw(const u8* data, std::size_t n) {
    if (poisoned_) return;
    if (n > capacity_ || buf_.size() > capacity_ - n) {
      poison(Status::of(ErrorCode::Oversized, "codec capacity exceeded"));
      return;
    }
    buf_.insert(buf_.end(), data, data + n);
  }

  void poison(Status s) {
    poisoned_ = true;
    poison_ = std::move(s);
  }

  std::size_t capacity_;
  std::vector<u8> buf_{};
  bool poisoned_{false};
  Status poison_{};
};

// Bounds-checked reader. Every accessor validates remaining length first and
// marks the reader failed; callers check once at the end.
class ByteReader {
 public:
  explicit ByteReader(std::span<const u8> data) : data_(data) {}

  [[nodiscard]] bool ok() const noexcept { return !failed_; }
  [[nodiscard]] const Status& status() const noexcept { return failure_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }
  [[nodiscard]] std::size_t consumed() const noexcept { return pos_; }
  // True when every byte was consumed exactly once.
  [[nodiscard]] bool exhausted() const noexcept { return !failed_ && pos_ == data_.size(); }

  u8 u8v() {
    if (!need(1)) return 0;
    return data_[pos_++];
  }

  u16 u16v() {
    if (!need(2)) return 0;
    const u16 v = static_cast<u16>(data_[pos_]) | static_cast<u16>(static_cast<u16>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
  }

  u32 u32v() {
    if (!need(4)) return 0;
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<u32>(data_[pos_ + static_cast<std::size_t>(i)]) << (8 * i);
    pos_ += 4;
    return v;
  }

  u64 u64v() {
    if (!need(8)) return 0;
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<u64>(data_[pos_ + static_cast<std::size_t>(i)]) << (8 * i);
    pos_ += 8;
    return v;
  }

  bool boolean() { return u8v() != 0; }

  std::string str(std::size_t max_len = ByteWriter::kMaxStringBytes) {
    const u32 n = u32v();
    if (!ok()) return {};
    if (static_cast<std::size_t>(n) > max_len) {
      fail(Status::of(ErrorCode::Oversized, "decoded string exceeds bound"));
      return {};
    }
    if (!need(n)) return {};
    std::string s(reinterpret_cast<const char*>(data_.data() + pos_), n);
    pos_ += n;
    return s;
  }

  std::vector<u8> blob(std::size_t max_len = ByteWriter::kMaxStringBytes) {
    const u32 n = u32v();
    if (!ok()) return {};
    if (static_cast<std::size_t>(n) > max_len) {
      fail(Status::of(ErrorCode::Oversized, "decoded blob exceeds bound"));
      return {};
    }
    if (!need(n)) return {};
    std::vector<u8> out(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                        data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
    pos_ += n;
    return out;
  }

  // Reads exactly n bytes as the trailing region of the input.
  std::span<const u8> tail(std::size_t n) {
    if (!need(n)) return {};
    auto s = data_.subspan(pos_, n);
    pos_ += n;
    return s;
  }

  void fail(Status s) {
    if (!failed_) {
      failed_ = true;
      failure_ = std::move(s);
    }
  }

 private:
  bool need(std::size_t n) {
    if (failed_) return false;
    if (n > data_.size() - pos_) {
      fail(Status::of(ErrorCode::MalformedInput, "truncated encoded input"));
      return false;
    }
    return true;
  }

  std::span<const u8> data_;
  std::size_t pos_{0};
  bool failed_{false};
  Status failure_{};
};

// Helpers for fixed-layout ints stored as u64 (negative values are rejected by
// all fabric call sites; i64 is only used for deltas that are checked first).
inline u64 to_bits(i64 v) noexcept { return static_cast<u64>(v); }
inline i64 from_bits(u64 v) noexcept { return static_cast<i64>(v); }

}  // namespace pacing

#endif  // PACING_FABRIC_CODEC_HPP
