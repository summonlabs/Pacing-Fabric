// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/control.hpp"

#include <thread>
#include <utility>

#include "pacing/serialize.hpp"

namespace pacing {
namespace {

constexpr std::size_t kControlCapacity = 1u << 20;

ControlResponse failure(ErrorCode code, std::string_view detail) {
  ControlResponse response{};
  response.code = code;
  response.detail.assign(detail);
  return response;
}

ControlResponse success() {
  ControlResponse response{};
  response.code = ErrorCode::Ok;
  return response;
}

}  // namespace

// ---------------------------------------------------------------------------
// codec
// ---------------------------------------------------------------------------

void encode_request(ByteWriter& writer, const ControlRequest& request) {
  writer.u16v(static_cast<u16>(request.op));
  encode_stamp(writer, request.stamp);
  writer.u32v(static_cast<u32>(request.fields.size()));
  for (const u8 byte : request.fields) writer.u8v(byte);
}

Status decode_request(ByteReader& reader, const Limits& limits, ControlRequest& out) {
  const u16 raw_op = reader.u16v();
  if (!reader.ok()) return reader.status();
  if (raw_op > static_cast<u16>(Operation::Stop)) {
    return Status::of(ErrorCode::MalformedInput, "operation discriminator out of range");
  }
  out.op = static_cast<Operation>(raw_op);
  Status stamp = decode_stamp(reader, out.stamp);
  if (!stamp) return stamp;
  const u32 field_bytes = reader.u32v();
  if (!reader.ok()) return reader.status();
  if (field_bytes > limits.max_frame_payload_bytes) {
    return Status::of(ErrorCode::Oversized, "request field block above configured bound");
  }
  const auto fields = reader.tail(field_bytes);
  if (!reader.ok()) return reader.status();
  out.fields.assign(fields.begin(), fields.end());
  return Status::success();
}

void encode_response(ByteWriter& writer, const ControlResponse& response) {
  writer.u16v(static_cast<u16>(response.code));
  writer.str(response.detail);
  writer.u32v(static_cast<u32>(response.payload.size()));
  for (const u8 byte : response.payload) writer.u8v(byte);
}

Status decode_response(ByteReader& reader, const Limits& limits, ControlResponse& out) {
  const u16 raw_code = reader.u16v();
  if (!reader.ok()) return reader.status();
  out.code = static_cast<ErrorCode>(raw_code);
  out.detail = reader.str(limits.max_explanation_bytes);
  if (!reader.ok()) return reader.status();
  const u32 payload_bytes = reader.u32v();
  if (!reader.ok()) return reader.status();
  if (payload_bytes > limits.max_frame_payload_bytes) {
    return Status::of(ErrorCode::Oversized, "response payload above configured bound");
  }
  const auto payload = reader.tail(payload_bytes);
  if (!reader.ok()) return reader.status();
  out.payload.assign(payload.begin(), payload.end());
  return Status::success();
}

Status decode_epoch_payload(ByteReader& reader, Epoch& epoch, BootId& boot, bool& running) {
  epoch = Epoch::from(reader.u64v());
  boot.counter = reader.u64v();
  boot.nonce = reader.u64v();
  running = reader.boolean();
  if (!reader.ok()) return reader.status();
  return Status::success();
}

Status decode_attempt_payload(ByteReader& reader, const Limits& limits, ApplicationRecord& out) {
  return decode_attempt(reader, limits, out);
}

Status decode_envelope_payload(ByteReader& reader, const Limits& limits, PacingEnvelope& out) {
  return decode_envelope(reader, limits, out);
}

// ---------------------------------------------------------------------------
// dispatcher
// ---------------------------------------------------------------------------

ControlDispatcher::ControlDispatcher(PacingFabric& fabric, Limits limits)
    : fabric_(fabric), limits_(limits) {}

ControlResponse ControlDispatcher::stale_check(const ControlRequest& request) const {
  const Epoch current_epoch = fabric_.epoch();
  if (request.stamp.epoch == 0) {
    stale_rejections_.fetch_add(1);
    return failure(ErrorCode::StaleEpoch, "request carries no coordinator epoch");
  }
  if (request.stamp.epoch != current_epoch.value()) {
    stale_rejections_.fetch_add(1);
    return failure(ErrorCode::StaleEpoch, "request epoch is not the current coordinator epoch");
  }
  const BootId current_boot = fabric_.boot();
  if (request.stamp.boot_nonce != 0 && request.stamp.boot_nonce != current_boot.nonce) {
    stale_rejections_.fetch_add(1);
    return failure(ErrorCode::StaleBoot, "request boot nonce is not the current coordinator boot");
  }
  return success();
}

ControlResponse ControlDispatcher::dispatch(const ControlRequest& request) {
  requests_served_.fetch_add(1);

  if (request.op != Operation::Epoch && request.op != Operation::Stop) {
    ControlResponse stale = stale_check(request);
    if (stale.code != ErrorCode::Ok) {
      requests_refused_.fetch_add(1);
      return stale;
    }
  }

  switch (request.op) {
    case Operation::Epoch: {
      ControlResponse response = success();
      ByteWriter writer(kControlCapacity);
      writer.u64v(fabric_.epoch().value());
      const BootId boot = fabric_.boot();
      writer.u64v(boot.counter);
      writer.u64v(boot.nonce);
      writer.boolean(fabric_.running());
      if (!writer.ok()) return failure(writer.status().code(), writer.status().detail());
      response.payload = writer.take();
      return response;
    }
    case Operation::BindFlow: {
      ByteReader reader(std::span<const u8>(request.fields.data(), request.fields.size()));
      FlowBinding binding{};
      binding.flow = FlowId::from(reader.u64v());
      binding.resource = ResourceRef{ResourceId::from(reader.u64v()), Generation::from(reader.u64v())};
      binding.path = pacing::PathRef{pacing::PathId::from(reader.u64v()), Generation::from(reader.u64v())};
      binding.service_class = ServiceClassId::from(reader.u64v());
      binding.tenant = TenantId::from(reader.u64v());
      binding.declared_max_burst_bytes = reader.u64v();
      binding.declared_max_burst_packets = reader.u64v();
      binding.quiesced = reader.boolean();
      binding.provenance = ProvenanceId::from(reader.u64v());
      if (!reader.ok() || !reader.exhausted()) {
        return failure(ErrorCode::MalformedInput, "bind-flow fields are malformed");
      }
      Status s = fabric_.bind_flow(binding);
      if (!s) return failure(s.code(), s.detail());
      auto stored = fabric_.binding(binding.flow);
      if (!stored) return failure(stored.code(), stored.status().detail());
      ControlResponse response = success();
      ByteWriter writer(64);
      writer.u64v(stored.value().generation.value());
      response.payload = writer.take();
      return response;
    }
    case Operation::PublishPolicy: {
      ByteReader reader(std::span<const u8>(request.fields.data(), request.fields.size()));
      PacingPolicy policy{};
      Status decoded = decode_policy(reader, limits_, policy);
      if (!decoded || !reader.exhausted()) {
        return failure(decoded.ok() ? ErrorCode::MalformedInput : decoded.code(),
                       decoded.ok() ? "publish-policy fields have trailing bytes" : decoded.detail());
      }
      auto published = fabric_.publish_policy(std::move(policy));
      if (!published) return failure(published.code(), published.status().detail());
      ControlResponse response = success();
      ByteWriter writer(64);
      writer.u64v(published.value().id.value());
      writer.u64v(published.value().generation.value());
      response.payload = writer.take();
      return response;
    }
    case Operation::DeriveEnvelope: {
      ByteReader reader(std::span<const u8>(request.fields.data(), request.fields.size()));
      const FlowId flow = FlowId::from(reader.u64v());
      if (!reader.ok() || !reader.exhausted()) {
        return failure(ErrorCode::MalformedInput, "derive-envelope fields are malformed");
      }
      auto envelope = fabric_.derive_envelope(flow);
      if (!envelope) return failure(envelope.code(), envelope.status().detail());
      ControlResponse response = success();
      ByteWriter writer(kControlCapacity);
      encode_envelope(writer, envelope.value());
      if (!writer.ok()) return failure(writer.status().code(), writer.status().detail());
      response.payload = writer.take();
      return response;
    }
    case Operation::ApplyEnvelope: {
      ByteReader reader(std::span<const u8>(request.fields.data(), request.fields.size()));
      const EnvelopeRef envelope{EnvelopeId::from(reader.u64v()), Generation::from(reader.u64v())};
      const BackendId backend = BackendId::from(reader.u64v());
      const AttemptId requested = AttemptId::from(reader.u64v());
      if (!reader.ok() || !reader.exhausted()) {
        return failure(ErrorCode::MalformedInput, "apply-envelope fields are malformed");
      }
      auto attempt = fabric_.apply_envelope(envelope, backend, requested);
      if (!attempt) return failure(attempt.code(), attempt.status().detail());
      ControlResponse response = success();
      ByteWriter writer(64);
      writer.u64v(attempt.value().value());
      response.payload = writer.take();
      return response;
    }
    case Operation::VerifyAttempt:
    case Operation::Revalidate: {
      ByteReader reader(std::span<const u8>(request.fields.data(), request.fields.size()));
      const AttemptId attempt = AttemptId::from(reader.u64v());
      if (!reader.ok() || !reader.exhausted()) {
        return failure(ErrorCode::MalformedInput, "attempt fields are malformed");
      }
      const Status s = request.op == Operation::VerifyAttempt ? fabric_.verify_attempt(attempt)
                                                              : fabric_.revalidate(attempt);
      if (!s) return failure(s.code(), s.detail());
      auto record = fabric_.attempt(attempt);
      if (!record) return failure(record.code(), record.status().detail());
      ControlResponse response = success();
      ByteWriter writer(kControlCapacity);
      encode_attempt(writer, record.value());
      response.payload = writer.take();
      return response;
    }
    case Operation::CancelAttempt: {
      ByteReader reader(std::span<const u8>(request.fields.data(), request.fields.size()));
      const AttemptId attempt = AttemptId::from(reader.u64v());
      const std::string reason = reader.str(limits_.max_explanation_bytes);
      if (!reader.ok() || !reader.exhausted()) {
        return failure(ErrorCode::MalformedInput, "cancel-attempt fields are malformed");
      }
      const Status s = fabric_.cancel_attempt(attempt, reason);
      if (!s) return failure(s.code(), s.detail());
      auto record = fabric_.attempt(attempt);
      if (!record) return failure(record.code(), record.status().detail());
      ControlResponse response = success();
      ByteWriter writer(kControlCapacity);
      encode_attempt(writer, record.value());
      response.payload = writer.take();
      return response;
    }
    case Operation::RevokeEnvelope: {
      ByteReader reader(std::span<const u8>(request.fields.data(), request.fields.size()));
      const EnvelopeRef envelope{EnvelopeId::from(reader.u64v()), Generation::from(reader.u64v())};
      const std::string reason = reader.str(limits_.max_explanation_bytes);
      if (!reader.ok() || !reader.exhausted()) {
        return failure(ErrorCode::MalformedInput, "revoke-envelope fields are malformed");
      }
      const Status s = fabric_.revoke_envelope(envelope, reason);
      if (!s) return failure(s.code(), s.detail());
      return success();
    }
    case Operation::Explain: {
      ByteReader reader(std::span<const u8>(request.fields.data(), request.fields.size()));
      const FlowId flow = FlowId::from(reader.u64v());
      if (!reader.ok() || !reader.exhausted()) {
        return failure(ErrorCode::MalformedInput, "explain fields are malformed");
      }
      auto explanation = fabric_.explain(flow);
      if (!explanation) return failure(explanation.code(), explanation.status().detail());
      ControlResponse response = success();
      ByteWriter writer(kControlCapacity);
      writer.str(explanation.value().to_json(limits_.max_explanation_bytes));
      writer.u16v(static_cast<u16>(explanation.value().refusal));
      const bool has_attempt = explanation.value().has_attempt;
      const bool has_envelope = explanation.value().has_envelope;
      writer.boolean(has_attempt);
      writer.boolean(has_envelope);
      if (has_attempt) encode_attempt(writer, explanation.value().attempt);
      if (has_envelope) encode_envelope(writer, explanation.value().envelope);
      if (!writer.ok()) return failure(writer.status().code(), writer.status().detail());
      response.payload = writer.take();
      return response;
    }
    case Operation::Stats: {
      const FabricStats stats = fabric_.stats();
      ControlResponse response = success();
      ByteWriter writer(512);
      const u64 counters[] = {stats.envelopes_derived,   stats.envelopes_refused,
                              stats.applies_requested,   stats.applies_reserved,
                              stats.applies_committed,   stats.applies_unverified,
                              stats.applies_mismatched,  stats.applies_refused,
                              stats.duplicate_applies,   stats.late_completions_discarded,
                              stats.cancellations,       stats.revocations,
                              stats.fences,              stats.stale_rejections,
                              stats.revalidations,       stats.readback_unavailable,
                              stats.events_emitted,      stats.journal_appends,
                              stats.checkpoints,         stats.envelopes_evicted,
                              stats.attempts_evicted,    requests_served_.load(),
                              requests_refused_.load(),  stale_rejections_.load()};
      writer.u32v(static_cast<u32>(sizeof(counters) / sizeof(counters[0])));
      for (const u64 value : counters) writer.u64v(value);
      response.payload = writer.take();
      return response;
    }
    case Operation::AdvanceEpoch: {
      const Status s = fabric_.advance_epoch();
      if (!s) return failure(s.code(), s.detail());
      ControlResponse response = success();
      ByteWriter writer(64);
      writer.u64v(fabric_.epoch().value());
      response.payload = writer.take();
      return response;
    }
    case Operation::Backends: {
      const auto descriptors = fabric_.list_backends();
      ControlResponse response = success();
      ByteWriter writer(kControlCapacity);
      writer.u32v(static_cast<u32>(descriptors.size()));
      for (const auto& descriptor : descriptors) encode_backend_descriptor(writer, descriptor);
      if (!writer.ok()) return failure(writer.status().code(), writer.status().detail());
      response.payload = writer.take();
      return response;
    }
    case Operation::Recover: {
      const RecoveryReport report = fabric_.recovery_report();
      ControlResponse response = success();
      ByteWriter writer(1024);
      writer.boolean(report.performed);
      writer.boolean(report.durable_directory_used);
      writer.u64v(report.bindings_restored);
      writer.u64v(report.policies_restored);
      writer.u64v(report.envelopes_invalidated);
      writer.u64v(report.attempts_requiring_revalidation);
      writer.u64v(report.attempts_ambiguous);
      writer.u64v(report.backends_requiring_registration);
      writer.u64v(report.records_replayed);
      writer.u64v(report.records_rejected);
      writer.u64v(fabric_.epoch().value());
      writer.u64v(fabric_.boot().counter);
      writer.u64v(fabric_.boot().nonce);
      writer.u16v(static_cast<u16>(report.code));
      writer.str(report.detail);
      response.payload = writer.take();
      return response;
    }
    case Operation::Stop:
      return success();
    case Operation::None:
    default:
      requests_refused_.fetch_add(1);
      return failure(ErrorCode::Unsupported, "operation is not supported");
  }
}

// ---------------------------------------------------------------------------
// server
// ---------------------------------------------------------------------------

ControlServer::ControlServer(Config config, PacingFabric& fabric)
    : config_(std::move(config)), fabric_(fabric), dispatcher_(fabric, config_.limits) {}

ControlServer::~ControlServer() {
  stop();
  std::unique_lock<std::mutex> lock(mu_);
  idle_cv_.wait(lock, [this] { return active_ == 0; });
}

Status ControlServer::start(u16& bound_port) {
  std::lock_guard<std::mutex> lock(mu_);
  if (started_) return Status::of(ErrorCode::InvalidState, "server already started");
  Status s = listener_.listen_loopback(config_.port, bound_port);
  if (!s) return s;
  started_ = true;
  return Status::success();
}

void ControlServer::stop() {
  std::vector<std::shared_ptr<FramedConnection>> connections;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_) return;
    stopping_ = true;
    connections = connections_;
  }
  // Closing the listener unblocks a thread parked in accept(); closing each
  // connection unblocks a thread parked in receive().
  listener_.close();
  for (auto& connection : connections) {
    if (connection) connection->close();
  }
}

u32 ControlServer::active_connections() const {
  std::lock_guard<std::mutex> lock(mu_);
  return active_;
}

Status ControlServer::send_response(const std::shared_ptr<FramedConnection>& connection,
                                    u64 request_id, bool fault, const ControlResponse& response) {
  ByteWriter writer(kControlCapacity);
  encode_response(writer, response);
  if (!writer.ok()) return writer.status();
  Frame frame{};
  frame.type = fault ? FrameType::Fault : FrameType::Response;
  frame.request_id = request_id;
  frame.payload = writer.take();
  return connection->send(frame, config_.limits);
}

void ControlServer::serve(const std::shared_ptr<FramedConnection>& connection) {
  ConnectionState state{};
  for (;;) {
    Frame frame{};
    const Status received = connection->receive(frame, config_.limits);
    if (!received) break;

    if (frame.type == FrameType::Shutdown) break;

    if (frame.type == FrameType::Hello) {
      ByteReader reader(std::span<const u8>(frame.payload.data(), frame.payload.size()));
      state.agent_name = reader.str(config_.limits.max_name_bytes);
      CallerStamp claimed{};
      Status stamp = decode_stamp(reader, claimed);
      if (!stamp || !reader.ok() || !reader.exhausted()) {
        (void)send_response(connection, frame.request_id, true,
                            failure(ErrorCode::MalformedInput, "hello payload is malformed"));
        break;
      }
      state.established = true;
      state.claimed = claimed;

      ControlResponse welcome = success();
      ByteWriter writer(128);
      writer.u64v(fabric_.epoch().value());
      const BootId boot = fabric_.boot();
      writer.u64v(boot.counter);
      writer.u64v(boot.nonce);
      writer.boolean(fabric_.running());
      if (!writer.ok()) {
        (void)send_response(connection, frame.request_id, true,
                            failure(writer.status().code(), writer.status().detail()));
        break;
      }
      welcome.payload = writer.take();
      if (!send_response(connection, frame.request_id, false, welcome)) break;
      continue;
    }

    if (frame.type != FrameType::Request) {
      (void)send_response(connection, frame.request_id, true,
                          failure(ErrorCode::MalformedInput, "unexpected frame type"));
      continue;
    }

    if (!state.established) {
      (void)send_response(connection, frame.request_id, true,
                          failure(ErrorCode::InvalidState, "hello handshake required before requests"));
      continue;
    }

    ControlRequest request{};
    ByteReader reader(std::span<const u8>(frame.payload.data(), frame.payload.size()));
    Status decoded = decode_request(reader, config_.limits, request);
    if (!decoded || !reader.exhausted()) {
      (void)send_response(
          connection, frame.request_id, true,
          failure(decoded.ok() ? ErrorCode::MalformedInput : decoded.code(),
                  decoded.ok() ? "request has trailing bytes" : decoded.detail()));
      continue;
    }

    const ControlResponse response = dispatcher_.dispatch(request);
    const bool fault = response.code != ErrorCode::Ok;
    if (!send_response(connection, frame.request_id, fault, response)) break;
    if (request.op == Operation::Stop) {
      // The stop request is the server's own shutdown trigger: it unblocks the
      // accept loop and closes every open connection. Without this the accept
      // loop would wait forever for a listener that nobody closes.
      stop();
      break;
    }
  }

  connection->close();
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (active_ > 0) --active_;
    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
      if (*it == connection) {
        connections_.erase(it);
        break;
      }
    }
    idle_cv_.notify_all();
  }
}

Status ControlServer::run() {
  for (;;) {
    auto connection = std::make_shared<FramedConnection>();
    Status accepted = listener_.accept(*connection);
    if (!accepted) {
      std::lock_guard<std::mutex> lock(mu_);
      if (stopping_) break;
      // A transient accept failure (for example a peer that reset the
      // connection during the handshake) must not end the server.
      continue;
    }
    bool admitted = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!stopping_ && active_ < config_.max_connections) {
        connections_.push_back(connection);
        ++active_;
        admitted = true;
      }
    }
    if (!admitted) {
      connection->close();
      continue;
    }
    std::thread([this, connection] { serve(connection); }).detach();
  }

  std::unique_lock<std::mutex> lock(mu_);
  idle_cv_.wait(lock, [this] { return active_ == 0; });
  lock.unlock();
  listener_.close();
  return Status::success();
}

// ---------------------------------------------------------------------------
// client
// ---------------------------------------------------------------------------

Status ControlClient::connect_loopback(u16 port, u32 retries, u64 retry_delay_ms) {
  return connection_.connect_loopback(port, retries, retry_delay_ms);
}

void ControlClient::close() { connection_.close(); }

Status ControlClient::handshake(std::string_view agent_name) {
  agent_name_.assign(agent_name);
  ByteWriter writer(kControlCapacity);
  writer.str(agent_name_);
  encode_stamp(writer, stamp_);
  if (!writer.ok()) return writer.status();

  Frame frame{};
  frame.type = FrameType::Hello;
  frame.request_id = ++request_id_;
  frame.payload = writer.take();
  Status sent = connection_.send(frame, limits_);
  if (!sent) return sent;

  Frame reply{};
  Status received = connection_.receive(reply, limits_);
  if (!received) return received;
  if (reply.type != FrameType::Response) {
    return Status::of(ErrorCode::InvalidState, "server did not answer the handshake with a welcome");
  }
  ByteReader reader(std::span<const u8>(reply.payload.data(), reply.payload.size()));
  ControlResponse response{};
  Status decoded = decode_response(reader, limits_, response);
  if (!decoded || !reader.exhausted()) {
    return Status::of(ErrorCode::MalformedInput, "welcome payload is malformed");
  }
  if (response.code != ErrorCode::Ok) {
    return Status::of(response.code, response.detail);
  }
  ByteReader payload_reader(std::span<const u8>(response.payload.data(), response.payload.size()));
  Epoch epoch{};
  BootId boot{};
  bool running = false;
  Status payload = decode_epoch_payload(payload_reader, epoch, boot, running);
  if (!payload || !payload_reader.exhausted()) {
    return Status::of(ErrorCode::MalformedInput, "welcome payload is incomplete");
  }
  stamp_.epoch = epoch.value();
  stamp_.boot_counter = boot.counter;
  stamp_.boot_nonce = boot.nonce;
  stamp_.worker_incarnation = boot.counter;
  stamp_.worker_nonce = boot.nonce;
  return Status::success();
}

ControlResponse ControlClient::call(Operation op, const std::vector<u8>& fields) {
  return call_with_stamp(op, fields, stamp_);
}

ControlResponse ControlClient::call_with_stamp(Operation op, const std::vector<u8>& fields,
                                               const CallerStamp& stamp) {
  ControlRequest request{};
  request.op = op;
  request.stamp = stamp;
  request.fields = fields;

  ByteWriter writer(kControlCapacity);
  encode_request(writer, request);
  if (!writer.ok()) return failure(writer.status().code(), writer.status().detail());

  Frame frame{};
  frame.type = FrameType::Request;
  frame.request_id = ++request_id_;
  frame.payload = writer.take();
  Status sent = connection_.send(frame, limits_);
  if (!sent) return failure(sent.code(), sent.detail());

  Frame reply{};
  Status received = connection_.receive(reply, limits_);
  if (!received) return failure(received.code(), received.detail());
  if (reply.type != FrameType::Response && reply.type != FrameType::Fault) {
    return failure(ErrorCode::MalformedInput, "unexpected reply frame type");
  }
  ByteReader reader(std::span<const u8>(reply.payload.data(), reply.payload.size()));
  ControlResponse response{};
  Status decoded = decode_response(reader, limits_, response);
  if (!decoded || !reader.exhausted()) {
    return failure(ErrorCode::MalformedInput, "reply payload is malformed");
  }
  return response;
}

ControlResponse ControlClient::epoch() { return call(Operation::Epoch, {}); }

ControlResponse ControlClient::bind_flow(const FlowBinding& binding) {
  ByteWriter writer(kControlCapacity);
  writer.u64v(binding.flow.value());
  writer.u64v(binding.resource.id.value());
  writer.u64v(binding.resource.generation.value());
  writer.u64v(binding.path.id.value());
  writer.u64v(binding.path.generation.value());
  writer.u64v(binding.service_class.value());
  writer.u64v(binding.tenant.value());
  writer.u64v(binding.declared_max_burst_bytes);
  writer.u64v(binding.declared_max_burst_packets);
  writer.boolean(binding.quiesced);
  writer.u64v(binding.provenance.value());
  if (!writer.ok()) return failure(writer.status().code(), writer.status().detail());
  return call(Operation::BindFlow, writer.take());
}

ControlResponse ControlClient::publish_policy(const PacingPolicy& policy) {
  ByteWriter writer(kControlCapacity);
  encode_policy(writer, policy);
  if (!writer.ok()) return failure(writer.status().code(), writer.status().detail());
  return call(Operation::PublishPolicy, writer.take());
}

ControlResponse ControlClient::derive_envelope(FlowId flow) {
  ByteWriter writer(64);
  writer.u64v(flow.value());
  return call(Operation::DeriveEnvelope, writer.take());
}

ControlResponse ControlClient::apply_envelope(EnvelopeRef envelope, BackendId backend,
                                              AttemptId requested) {
  ByteWriter writer(64);
  writer.u64v(envelope.id.value());
  writer.u64v(envelope.generation.value());
  writer.u64v(backend.value());
  writer.u64v(requested.value());
  return call(Operation::ApplyEnvelope, writer.take());
}

ControlResponse ControlClient::verify_attempt(AttemptId attempt) {
  ByteWriter writer(64);
  writer.u64v(attempt.value());
  return call(Operation::VerifyAttempt, writer.take());
}

ControlResponse ControlClient::cancel_attempt(AttemptId attempt, std::string_view reason) {
  ByteWriter writer(512);
  writer.u64v(attempt.value());
  writer.str(reason);
  return call(Operation::CancelAttempt, writer.take());
}

ControlResponse ControlClient::revoke_envelope(EnvelopeRef envelope, std::string_view reason) {
  ByteWriter writer(512);
  writer.u64v(envelope.id.value());
  writer.u64v(envelope.generation.value());
  writer.str(reason);
  return call(Operation::RevokeEnvelope, writer.take());
}

ControlResponse ControlClient::revalidate(AttemptId attempt) {
  ByteWriter writer(64);
  writer.u64v(attempt.value());
  return call(Operation::Revalidate, writer.take());
}

ControlResponse ControlClient::explain(FlowId flow) {
  ByteWriter writer(64);
  writer.u64v(flow.value());
  return call(Operation::Explain, writer.take());
}

ControlResponse ControlClient::stats() { return call(Operation::Stats, {}); }
ControlResponse ControlClient::advance_epoch() { return call(Operation::AdvanceEpoch, {}); }
ControlResponse ControlClient::backends() { return call(Operation::Backends, {}); }
ControlResponse ControlClient::recover() { return call(Operation::Recover, {}); }
ControlResponse ControlClient::stop() { return call(Operation::Stop, {}); }

}  // namespace pacing
