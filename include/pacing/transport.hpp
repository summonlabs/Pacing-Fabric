// Pacing Fabric - framed control-plane transport.
//
// A real, byte-oriented framed transport over a real stream socket. It carries
// control-plane intent only: it never transports packets, never influences
// data-plane scheduling and never substitutes for a pacing backend.
//
// Frame layout (24-byte header, little-endian):
//   u32 magic 'PFR1' | u16 version | u16 type | u64 request_id | u32 payload_len | u32 crc32c
// The CRC covers the header fields from version through payload_len extended
// with the payload, so header corruption and payload corruption are both
// detected. payload_len is validated against Limits before any allocation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_TRANSPORT_HPP
#define PACING_FABRIC_TRANSPORT_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pacing/codec.hpp"
#include "pacing/limits.hpp"
#include "pacing/status.hpp"

namespace pacing {

inline constexpr std::uint32_t kFrameMagic = 0x31524650u;  // 'PFR1' little-endian
inline constexpr std::size_t kFrameHeaderBytes = 24;

enum class FrameType : u16 {
  Hello = 1,
  Welcome = 2,
  Request = 3,
  Response = 4,
  Fault = 5,
  Shutdown = 6,
};

// Control-plane operations. The numeric values are part of the wire contract.
enum class Operation : u16 {
  None = 0,
  Epoch = 1,
  BindFlow = 2,
  PublishPolicy = 3,
  DeriveEnvelope = 4,
  ApplyEnvelope = 5,
  VerifyAttempt = 6,
  CancelAttempt = 7,
  RevokeEnvelope = 8,
  Explain = 9,
  Stats = 10,
  AdvanceEpoch = 11,
  Revalidate = 12,
  Backends = 13,
  Recover = 14,
  Stop = 15,
};

std::string_view to_string(Operation op) noexcept;
bool parse_operation(std::string_view text, Operation& out) noexcept;

// Every request carries the caller's view of coordinator authority. A request
// stamped with a superseded epoch or boot is rejected, never reinterpreted.
struct CallerStamp {
  u64 epoch{0};
  u64 boot_counter{0};
  u64 boot_nonce{0};
  u64 worker_incarnation{0};
  u64 worker_nonce{0};

  friend bool operator==(const CallerStamp&, const CallerStamp&) noexcept = default;
};

struct Frame {
  FrameType type{FrameType::Request};
  u64 request_id{0};
  std::vector<u8> payload{};
};

// Owns one connected stream socket. Not copyable; movable.
class FramedConnection {
 public:
  FramedConnection() = default;
  ~FramedConnection();
  FramedConnection(const FramedConnection&) = delete;
  FramedConnection& operator=(const FramedConnection&) = delete;
  FramedConnection(FramedConnection&& other) noexcept;
  FramedConnection& operator=(FramedConnection&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept { return handle_.load() >= 0; }
  // Closes the underlying socket. Safe to call concurrently with a blocked
  // send/receive on another thread: the handle is atomic, and the blocked call
  // observes the closed socket as an I/O failure rather than as fresh data.
  void close();
  [[nodiscard]] std::int64_t native_handle() const noexcept { return handle_.load(); }

  [[nodiscard]] Status send(const Frame& frame, const Limits& limits);
  [[nodiscard]] Status receive(Frame& frame, const Limits& limits);

  // True once a framing failure has desynchronised the stream. A connection in
  // this state refuses every further operation: a byte stream whose framing is
  // no longer trustworthy must never be reinterpreted as fresh frames.
  [[nodiscard]] bool poisoned() const noexcept { return poisoned_.load(); }
  [[nodiscard]] Status poison_reason() const;

  // Connect to a loopback listener. Retries are bounded and explicit; there is
  // no unbounded wait and no timeout-based liveness guess.
  [[nodiscard]] Status connect_loopback(u16 port, u32 retries, u64 retry_delay_ms);
  [[nodiscard]] Status connect_to(const std::string& host, u16 port);

  void set_nodelay(bool enabled);

  // Adopts an accepted socket handle. Only the listener may do this, so a
  // connection handle can never be forged from arbitrary integers.
  [[nodiscard]] static FramedConnection adopt(std::int64_t handle);

 private:
  friend class FrameListener;
  void poison(Status reason);
  std::atomic<std::int64_t> handle_{-1};
  std::atomic<bool> poisoned_{false};
  mutable std::mutex reason_mu_;
  Status poison_reason_{};
};

class FrameListener {
 public:
  FrameListener() = default;
  ~FrameListener();
  FrameListener(const FrameListener&) = delete;
  FrameListener& operator=(const FrameListener&) = delete;

  // Binds a loopback listener. With port == 0 the kernel assigns a port, which
  // is reported back through bound_port.
  [[nodiscard]] Status listen_loopback(u16 port, u16& bound_port);
  [[nodiscard]] Status accept(FramedConnection& connection);
  // Closing the listener from another thread unblocks a concurrent accept().
  void close();
  [[nodiscard]] bool valid() const noexcept { return handle_.load() >= 0; }

 private:
  std::atomic<std::int64_t> handle_{-1};
};

// Process/platform bootstrap for the socket layer. Idempotent.
[[nodiscard]] Status transport_global_init();
void transport_global_shutdown();

// Encodes/decodes the control-plane stamp that prefixes every request payload.
void encode_stamp(ByteWriter& writer, const CallerStamp& stamp);
Status decode_stamp(ByteReader& reader, CallerStamp& stamp);

// Loopback helpers used by the coordinator daemon and the test harness.
[[nodiscard]] Status loopback_hostname(std::string& out);

}  // namespace pacing

#endif  // PACING_FABRIC_TRANSPORT_HPP
