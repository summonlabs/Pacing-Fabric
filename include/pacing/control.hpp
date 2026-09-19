// Pacing Fabric - control-plane protocol, dispatcher, client and server.
//
// Wire format
// -----------
// Request  payload: u16 operation, then the caller stamp (five u64), then
//                   operation-specific fields (see control.cpp).
// Response payload: u16 error code, bounded detail string, then
//                   operation-specific fields.
//
// Frames are carried by the framed transport (see transport.hpp). A request
// stamped with a coordinator epoch or boot that the server no longer holds is
// refused: it is never reinterpreted against current authority.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_CONTROL_HPP
#define PACING_FABRIC_CONTROL_HPP

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pacing/backends.hpp"
#include "pacing/fabric.hpp"
#include "pacing/transport.hpp"

namespace pacing {

struct ControlRequest {
  Operation op{Operation::None};
  CallerStamp stamp{};
  std::vector<u8> fields{};
};

struct ControlResponse {
  ErrorCode code{ErrorCode::Ok};
  std::string detail{};
  std::vector<u8> payload{};
};

void encode_request(ByteWriter& writer, const ControlRequest& request);
Status decode_request(ByteReader& reader, const Limits& limits, ControlRequest& out);
void encode_response(ByteWriter& writer, const ControlResponse& response);
Status decode_response(ByteReader& reader, const Limits& limits, ControlResponse& out);

// Applies one decoded request to a fabric. Free of I/O so it can be exercised
// directly, which is how the protocol is unit tested.
class ControlDispatcher {
 public:
  ControlDispatcher(PacingFabric& fabric, Limits limits);

  [[nodiscard]] ControlResponse dispatch(const ControlRequest& request);

  [[nodiscard]] u64 requests_served() const noexcept { return requests_served_.load(); }
  [[nodiscard]] u64 requests_refused() const noexcept { return requests_refused_.load(); }
  [[nodiscard]] u64 stale_rejections() const noexcept { return stale_rejections_.load(); }

 private:
  [[nodiscard]] ControlResponse stale_check(const ControlRequest& request) const;

  PacingFabric& fabric_;
  Limits limits_;
  mutable std::atomic<u64> requests_served_{0};
  mutable std::atomic<u64> requests_refused_{0};
  mutable std::atomic<u64> stale_rejections_{0};
};

// Listener plus connection pool. Connection threads are detached and the
// server waits for them to drain before run() returns, so no thread outlives
// the objects it touches.
class ControlServer {
 public:
  struct Config {
    Limits limits{};
    u16 port{0};
    u32 max_connections{64};
    std::string name{"pacingd"};
  };

  ControlServer(Config config, PacingFabric& fabric);
  ~ControlServer();

  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  // Binds the listener and reports the port actually bound.
  [[nodiscard]] Status start(u16& bound_port);
  // Serves until stop() is requested or the listener fails. Blocks.
  [[nodiscard]] Status run();
  void stop();
  [[nodiscard]] u32 active_connections() const;
  [[nodiscard]] const ControlDispatcher& dispatcher() const noexcept { return dispatcher_; }

 private:
  struct ConnectionState {
    bool established{false};
    CallerStamp claimed{};
    std::string agent_name{};
  };

  void serve(const std::shared_ptr<FramedConnection>& connection);
  [[nodiscard]] Status send_response(const std::shared_ptr<FramedConnection>& connection,
                                     u64 request_id, bool fault, const ControlResponse& response);

  Config config_;
  PacingFabric& fabric_;
  FrameListener listener_;
  ControlDispatcher dispatcher_;
  mutable std::mutex mu_;
  std::condition_variable idle_cv_;
  std::vector<std::shared_ptr<FramedConnection>> connections_;
  u32 active_{0};
  bool stopping_{false};
  bool started_{false};
};

// Blocking client used by the CLI, the agent process and the tests. All calls
// are synchronous request/response with a monotonic request id.
class ControlClient {
 public:
  ControlClient() = default;

  [[nodiscard]] Status connect_loopback(u16 port, u32 retries, u64 retry_delay_ms);
  void close();
  [[nodiscard]] bool connected() const noexcept { return connection_.valid(); }

  // Performs the hello exchange and records the coordinator stamp the server
  // reported. Must be called before any operation request.
  [[nodiscard]] Status handshake(std::string_view agent_name);

  [[nodiscard]] ControlResponse call(Operation op, const std::vector<u8>& fields);

  // Issues a request stamped with an explicit caller stamp. Used to prove that
  // a superseded epoch or boot incarnation is fenced rather than reinterpreted,
  // and by operators that must address a coordinator they have not re-synced
  // with yet.
  [[nodiscard]] ControlResponse call_with_stamp(Operation op, const std::vector<u8>& fields,
                                                const CallerStamp& stamp);

  // Typed helpers. Each returns the raw response so a caller can inspect the
  // exact status code and detail.
  [[nodiscard]] ControlResponse epoch();
  [[nodiscard]] ControlResponse bind_flow(const FlowBinding& binding);
  [[nodiscard]] ControlResponse publish_policy(const PacingPolicy& policy);
  [[nodiscard]] ControlResponse derive_envelope(FlowId flow);
  [[nodiscard]] ControlResponse apply_envelope(EnvelopeRef envelope, BackendId backend,
                                               AttemptId requested = {});
  [[nodiscard]] ControlResponse verify_attempt(AttemptId attempt);
  [[nodiscard]] ControlResponse cancel_attempt(AttemptId attempt, std::string_view reason);
  [[nodiscard]] ControlResponse revoke_envelope(EnvelopeRef envelope, std::string_view reason);
  [[nodiscard]] ControlResponse revalidate(AttemptId attempt);
  [[nodiscard]] ControlResponse explain(FlowId flow);
  [[nodiscard]] ControlResponse stats();
  [[nodiscard]] ControlResponse advance_epoch();
  [[nodiscard]] ControlResponse backends();
  [[nodiscard]] ControlResponse recover();
  [[nodiscard]] ControlResponse stop();

  [[nodiscard]] const CallerStamp& stamp() const noexcept { return stamp_; }
  [[nodiscard]] u64 request_count() const noexcept { return request_id_; }

 private:
  FramedConnection connection_;
  Limits limits_{};
  CallerStamp stamp_{};
  u64 request_id_{0};
  std::string agent_name_{};
};

// Decoders for the response payloads that carry structured results. Returning a
// Status keeps a malformed reply from being mistaken for a result.
[[nodiscard]] Status decode_epoch_payload(ByteReader& reader, Epoch& epoch, BootId& boot, bool& running);
[[nodiscard]] Status decode_attempt_payload(ByteReader& reader, const Limits& limits,
                                            ApplicationRecord& out);
[[nodiscard]] Status decode_envelope_payload(ByteReader& reader, const Limits& limits,
                                             PacingEnvelope& out);

}  // namespace pacing

#endif  // PACING_FABRIC_CONTROL_HPP
