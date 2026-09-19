// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/transport.hpp"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include "pacing/crc32c.hpp"
#include "pacing/version.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pacing {
namespace {

#if defined(_WIN32)
using native_socket = SOCKET;
constexpr std::int64_t kInvalid = -1;

std::int64_t to_handle(SOCKET s) { return s == INVALID_SOCKET ? kInvalid : static_cast<std::int64_t>(s); }
SOCKET to_socket(std::int64_t h) { return static_cast<SOCKET>(h); }
#else
using native_socket = int;
constexpr std::int64_t kInvalid = -1;
std::int64_t to_handle(int s) { return s < 0 ? kInvalid : static_cast<std::int64_t>(s); }
int to_socket(std::int64_t h) { return static_cast<int>(h); }
#endif

void close_socket(std::int64_t handle) {
  if (handle == kInvalid) return;
#if defined(_WIN32)
  ::closesocket(to_socket(handle));
#else
  ::close(to_socket(handle));
#endif
}

bool send_all(std::int64_t handle, const u8* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
    const std::size_t remaining = size - sent;
    const int chunk = static_cast<int>(remaining > (1u << 20) ? (1u << 20) : remaining);
#if defined(_WIN32)
    const int n = ::send(to_socket(handle), reinterpret_cast<const char*>(data + sent), chunk, 0);
#else
    const int n = static_cast<int>(::send(to_socket(handle), data + sent, static_cast<std::size_t>(chunk), 0));
#endif
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

// Reads exactly size bytes, or fails. A peer that closes mid-frame produces a
// failure, never a short frame that a caller might mistake for a valid one.
bool recv_exact(std::int64_t handle, u8* data, std::size_t size) {
  std::size_t got = 0;
  while (got < size) {
    const std::size_t remaining = size - got;
    const int chunk = static_cast<int>(remaining > (1u << 20) ? (1u << 20) : remaining);
#if defined(_WIN32)
    const int n = ::recv(to_socket(handle), reinterpret_cast<char*>(data + got), chunk, 0);
#else
    const int n = static_cast<int>(::recv(to_socket(handle), data + got, static_cast<std::size_t>(chunk), 0));
#endif
    if (n <= 0) return false;
    got += static_cast<std::size_t>(n);
  }
  return true;
}

void put_u16(u8* p, u16 v) {
  p[0] = static_cast<u8>(v & 0xFF);
  p[1] = static_cast<u8>((v >> 8) & 0xFF);
}

void put_u32(u8* p, u32 v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<u8>((v >> (8 * i)) & 0xFF);
}

void put_u64(u8* p, u64 v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<u8>((v >> (8 * i)) & 0xFF);
}

u16 get_u16(const u8* p) { return static_cast<u16>(p[0]) | static_cast<u16>(static_cast<u16>(p[1]) << 8); }

u32 get_u32(const u8* p) {
  return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
         (static_cast<u32>(p[3]) << 24);
}

u64 get_u64(const u8* p) {
  u64 v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
  return v;
}

#if defined(_WIN32)
std::mutex g_wsa_mu;
int g_wsa_refs = 0;
#endif

}  // namespace

std::string_view to_string(Operation op) noexcept {
  switch (op) {
    case Operation::None: return "none";
    case Operation::Epoch: return "epoch";
    case Operation::BindFlow: return "bind-flow";
    case Operation::PublishPolicy: return "publish-policy";
    case Operation::DeriveEnvelope: return "derive-envelope";
    case Operation::ApplyEnvelope: return "apply-envelope";
    case Operation::VerifyAttempt: return "verify-attempt";
    case Operation::CancelAttempt: return "cancel-attempt";
    case Operation::RevokeEnvelope: return "revoke-envelope";
    case Operation::Explain: return "explain";
    case Operation::Stats: return "stats";
    case Operation::AdvanceEpoch: return "advance-epoch";
    case Operation::Revalidate: return "revalidate";
    case Operation::Backends: return "backends";
    case Operation::Recover: return "recover";
    case Operation::Stop: return "stop";
  }
  return "none";
}

bool parse_operation(std::string_view text, Operation& out) noexcept {
  const Operation candidates[] = {
      Operation::Epoch,          Operation::BindFlow,       Operation::PublishPolicy,
      Operation::DeriveEnvelope, Operation::ApplyEnvelope,  Operation::VerifyAttempt,
      Operation::CancelAttempt,  Operation::RevokeEnvelope, Operation::Explain,
      Operation::Stats,          Operation::AdvanceEpoch,   Operation::Revalidate,
      Operation::Backends,       Operation::Recover,        Operation::Stop,
  };
  for (const Operation candidate : candidates) {
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

void encode_stamp(ByteWriter& writer, const CallerStamp& stamp) {
  writer.u64v(stamp.epoch);
  writer.u64v(stamp.boot_counter);
  writer.u64v(stamp.boot_nonce);
  writer.u64v(stamp.worker_incarnation);
  writer.u64v(stamp.worker_nonce);
}

Status decode_stamp(ByteReader& reader, CallerStamp& stamp) {
  stamp.epoch = reader.u64v();
  stamp.boot_counter = reader.u64v();
  stamp.boot_nonce = reader.u64v();
  stamp.worker_incarnation = reader.u64v();
  stamp.worker_nonce = reader.u64v();
  if (!reader.ok()) return reader.status();
  return Status::success();
}

FramedConnection FramedConnection::adopt(std::int64_t handle) {
  FramedConnection connection;
  connection.handle_.store(handle);
  connection.poisoned_.store(false);
  return connection;
}

FramedConnection::~FramedConnection() { close(); }

FramedConnection::FramedConnection(FramedConnection&& other) noexcept {
  handle_.store(other.handle_.exchange(kInvalid));
  poisoned_.store(other.poisoned_.load());
  std::lock_guard<std::mutex> lock(other.reason_mu_);
  poison_reason_ = other.poison_reason_;
}

FramedConnection& FramedConnection::operator=(FramedConnection&& other) noexcept {
  if (this != &other) {
    close();
    handle_.store(other.handle_.exchange(kInvalid));
    poisoned_.store(other.poisoned_.load());
    std::lock_guard<std::mutex> lock(other.reason_mu_);
    poison_reason_ = other.poison_reason_;
  }
  return *this;
}

void FramedConnection::close() {
  const std::int64_t handle = handle_.exchange(kInvalid);
  close_socket(handle);
}

void FramedConnection::poison(Status reason) {
  bool expected = false;
  if (!poisoned_.compare_exchange_strong(expected, true)) return;
  {
    std::lock_guard<std::mutex> lock(reason_mu_);
    poison_reason_ = std::move(reason);
  }
  close();
}

Status FramedConnection::poison_reason() const {
  std::lock_guard<std::mutex> lock(reason_mu_);
  return poison_reason_;
}

void FramedConnection::set_nodelay(bool enabled) {
  const std::int64_t handle = handle_.load();
  if (handle == kInvalid) return;
  const int value = enabled ? 1 : 0;
#if defined(_WIN32)
  (void)::setsockopt(to_socket(handle), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value),
                     sizeof(value));
#else
  (void)::setsockopt(to_socket(handle), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
#endif
}

Status FramedConnection::send(const Frame& frame, const Limits& limits) {
  if (poisoned_.load()) {
    return Status::of(ErrorCode::InvalidState, "connection is unusable after a framing failure");
  }
  const std::int64_t handle = handle_.load();
  if (handle == kInvalid) return Status::of(ErrorCode::BackendUnavailable, "connection is closed");
  if (frame.payload.size() > limits.max_frame_payload_bytes) {
    return Status::of(ErrorCode::Oversized, "frame payload above configured bound");
  }
  u32 payload_len = 0;
  if (!checked::narrow_u32(frame.payload.size(), payload_len)) {
    return Status::of(ErrorCode::Oversized, "frame payload length is not representable");
  }
  u8 header[kFrameHeaderBytes] = {0};
  put_u32(header, kFrameMagic);
  put_u16(header + 4, kWireProtocolVersion);
  put_u16(header + 6, static_cast<u16>(frame.type));
  put_u64(header + 8, frame.request_id);
  put_u32(header + 16, payload_len);
  const u32 crc = crc32c_extend(crc32c(std::span<const u8>(header + 4, 16)), frame.payload);
  put_u32(header + 20, crc);

  if (!send_all(handle, header, kFrameHeaderBytes)) {
    poison(Status::of(ErrorCode::IoFailure, "frame header send failed"));
    return Status::of(ErrorCode::IoFailure, "frame header send failed");
  }
  if (!frame.payload.empty() && !send_all(handle, frame.payload.data(), frame.payload.size())) {
    poison(Status::of(ErrorCode::IoFailure, "frame payload send failed"));
    return Status::of(ErrorCode::IoFailure, "frame payload send failed");
  }
  return Status::success();
}

Status FramedConnection::receive(Frame& frame, const Limits& limits) {
  if (poisoned_.load()) {
    return Status::of(ErrorCode::InvalidState, "connection is unusable after a framing failure");
  }
  const std::int64_t handle = handle_.load();
  if (handle == kInvalid) return Status::of(ErrorCode::BackendUnavailable, "connection is closed");

  u8 header[kFrameHeaderBytes] = {0};
  if (!recv_exact(handle, header, kFrameHeaderBytes)) {
    poison(Status::of(ErrorCode::IoFailure, "frame header read failed or peer closed"));
    return Status::of(ErrorCode::IoFailure, "frame header read failed or peer closed");
  }
  if (get_u32(header) != kFrameMagic) {
    poison(Status::of(ErrorCode::MalformedInput, "frame magic mismatch"));
    return Status::of(ErrorCode::MalformedInput, "frame magic mismatch");
  }
  if (get_u16(header + 4) != kWireProtocolVersion) {
    poison(Status::of(ErrorCode::Unsupported, "frame protocol version mismatch"));
    return Status::of(ErrorCode::Unsupported, "frame protocol version mismatch");
  }
  const u16 raw_type = get_u16(header + 6);
  if (raw_type == 0 || raw_type > static_cast<u16>(FrameType::Shutdown)) {
    poison(Status::of(ErrorCode::MalformedInput, "frame type discriminator out of range"));
    return Status::of(ErrorCode::MalformedInput, "frame type discriminator out of range");
  }
  const u64 request_id = get_u64(header + 8);
  const u32 payload_len = get_u32(header + 16);
  const u32 expected_crc = get_u32(header + 20);
  if (payload_len > limits.max_frame_payload_bytes) {
    poison(Status::of(ErrorCode::Oversized, "frame payload above configured bound"));
    return Status::of(ErrorCode::Oversized, "frame payload above configured bound");
  }
  std::vector<u8> payload(payload_len);
  if (payload_len > 0 && !recv_exact(handle, payload.data(), payload_len)) {
    poison(Status::of(ErrorCode::MalformedInput, "frame payload truncated"));
    return Status::of(ErrorCode::MalformedInput, "frame payload truncated");
  }
  if (crc32c_extend(crc32c(std::span<const u8>(header + 4, 16)), payload) != expected_crc) {
    poison(Status::of(ErrorCode::IntegrityFailure, "frame checksum mismatch"));
    return Status::of(ErrorCode::IntegrityFailure, "frame checksum mismatch");
  }
  frame.type = static_cast<FrameType>(raw_type);
  frame.request_id = request_id;
  frame.payload = std::move(payload);
  return Status::success();
}

Status FramedConnection::connect_to(const std::string& host, u16 port) {
  // The socket layer is initialised lazily as well as explicitly, so a caller
  // that forgets transport_global_init cannot silently get a connection that
  // always fails.
  Status ready = transport_global_init();
  if (!ready) return ready;
  if (poisoned_.load()) {
    return Status::of(ErrorCode::InvalidState, "connection is unusable after a framing failure");
  }
  close();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
    return Status::of(ErrorCode::IoFailure, "address resolution failed");
  }
  std::int64_t connected = kInvalid;
  for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
    native_socket s = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (to_handle(s) == kInvalid) continue;
    if (::connect(s, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
      connected = to_handle(s);
      break;
    }
    close_socket(to_handle(s));
  }
  ::freeaddrinfo(result);
  if (connected == kInvalid) return Status::of(ErrorCode::BackendUnavailable, "connect failed");
  handle_.store(connected);
  set_nodelay(true);
  return Status::success();
}

Status FramedConnection::connect_loopback(u16 port, u32 retries, u64 retry_delay_ms) {
  for (u32 attempt = 0; attempt <= retries; ++attempt) {
    Status s = connect_to("127.0.0.1", port);
    if (s.ok()) return s;
    if (attempt < retries) {
      std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
    }
  }
  return Status::of(ErrorCode::BackendUnavailable, "listener did not become reachable");
}

FrameListener::~FrameListener() { close(); }

void FrameListener::close() {
  const std::int64_t handle = handle_.exchange(kInvalid);
  close_socket(handle);
}

Status FrameListener::listen_loopback(u16 port, u16& bound_port) {
  Status ready = transport_global_init();
  if (!ready) return ready;
  close();
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  native_socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (to_handle(s) == kInvalid) return Status::of(ErrorCode::IoFailure, "listener socket creation failed");
  const int reuse = 1;
#if defined(_WIN32)
  (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
  (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
  if (::bind(s, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
    close_socket(to_handle(s));
    return Status::of(ErrorCode::IoFailure, "listener bind failed");
  }
  if (::listen(s, 64) != 0) {
    close_socket(to_handle(s));
    return Status::of(ErrorCode::IoFailure, "listener listen failed");
  }
  sockaddr_in actual{};
#if defined(_WIN32)
  int actual_len = static_cast<int>(sizeof(actual));
#else
  socklen_t actual_len = sizeof(actual);
#endif
  if (::getsockname(s, reinterpret_cast<sockaddr*>(&actual), &actual_len) != 0) {
    close_socket(to_handle(s));
    return Status::of(ErrorCode::IoFailure, "listener address lookup failed");
  }
  bound_port = ntohs(actual.sin_port);
  handle_.store(to_handle(s));
  return Status::success();
}

Status FrameListener::accept(FramedConnection& connection) {
  const std::int64_t handle = handle_.load();
  if (handle == kInvalid) return Status::of(ErrorCode::InvalidState, "listener is closed");
  native_socket s = ::accept(to_socket(handle), nullptr, nullptr);
  if (to_handle(s) == kInvalid) return Status::of(ErrorCode::IoFailure, "accept failed");
  connection = FramedConnection::adopt(to_handle(s));
  connection.set_nodelay(true);
  return Status::success();
}

Status loopback_hostname(std::string& out) {
  out = "127.0.0.1";
  return Status::success();
}

Status transport_global_init() {
#if defined(_WIN32)
  std::lock_guard<std::mutex> lock(g_wsa_mu);
  if (g_wsa_refs == 0) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      return Status::of(ErrorCode::Internal, "WSAStartup failed");
    }
  }
  ++g_wsa_refs;
#endif
  return Status::success();
}

void transport_global_shutdown() {
#if defined(_WIN32)
  std::lock_guard<std::mutex> lock(g_wsa_mu);
  if (g_wsa_refs > 0) {
    --g_wsa_refs;
    if (g_wsa_refs == 0) WSACleanup();
  }
#endif
}

}  // namespace pacing
