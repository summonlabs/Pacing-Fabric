// Multiprocess integration: a real coordinator daemon, real worker processes,
// a real framed TCP control transport, hard process kills and epoch fencing.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include "framework.hpp"
#include "pacing/pacing.hpp"
#include "process.hpp"

using namespace pacing;

namespace {

std::string daemon_path() { return tf::option("daemon"); }
std::string agent_path() { return tf::option("agent"); }

struct DaemonOptions {
  std::string durable_directory{};
  u64 resource{1};
  u64 ceiling_bps{10'000'000'000ull};
  u64 backend_id{9};
  bool quiet{true};
};

// Starts a real pacingd process and waits until it reports the port it bound.
class Daemon {
 public:
  bool start(const DaemonOptions& options) {
    static std::atomic<unsigned> sequence{0};
    directory_ = (std::filesystem::temp_directory_path() /
                  ("pacing_fabric_multiproc_" + std::to_string(sequence.fetch_add(1))))
                     .string();
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    port_file_ = (std::filesystem::path(directory_) / "port.txt").string();

    std::vector<std::string> arguments;
    arguments.push_back("--port");
    arguments.push_back("0");
    arguments.push_back("--port-file");
    arguments.push_back(port_file_);
    arguments.push_back("--resource");
    arguments.push_back(std::to_string(options.resource));
    arguments.push_back("--ceiling-bps");
    arguments.push_back(std::to_string(options.ceiling_bps));
    arguments.push_back("--backend-id");
    arguments.push_back(std::to_string(options.backend_id));
    if (!options.durable_directory.empty()) {
      arguments.push_back("--durable");
      arguments.push_back(options.durable_directory);
    }
    if (options.quiet) arguments.push_back("--quiet");

    if (!pf_proc::Child::spawn(daemon_path(), arguments, child_)) return false;
    if (!pf_proc::wait_for_file(port_file_, 4000, 25)) return false;
    std::string text;
    if (!pf_proc::read_text_file(port_file_, text)) return false;
    port_ = static_cast<u16>(std::strtoul(text.c_str(), nullptr, 10));
    return port_ != 0;
  }

  [[nodiscard]] u16 port() const noexcept { return port_; }
  [[nodiscard]] const std::string& workspace() const noexcept { return directory_; }
  [[nodiscard]] pf_proc::Child& child() noexcept { return child_; }

  void remove_port_file() {
    std::error_code ec;
    std::filesystem::remove(port_file_, ec);
  }

  ~Daemon() {
    if (child_.valid()) {
      (void)child_.kill_hard();
      int exit_code = 0;
      (void)child_.wait(exit_code);
    }
    std::error_code ec;
    std::filesystem::remove_all(directory_, ec);
  }

 private:
  pf_proc::Child child_{};
  std::string directory_{};
  std::string port_file_{};
  u16 port_{0};
};

bool connect_client(ControlClient& client, u16 port, const std::string& name) {
  const Status connected = client.connect_loopback(port, 400, 25);
  if (!connected.ok()) {
    std::fprintf(stderr, "test_multiproc: connect to port %u failed: %s\n",
                 static_cast<unsigned>(port), connected.to_string().c_str());
    return false;
  }
  const Status handshake = client.handshake(name);
  if (!handshake.ok()) {
    std::fprintf(stderr, "test_multiproc: handshake on port %u failed: %s\n",
                 static_cast<unsigned>(port), handshake.to_string().c_str());
    return false;
  }
  return true;
}

FlowBinding make_binding(u64 flow, u64 resource, u64 path) {
  FlowBinding binding{};
  binding.flow = FlowId::from(flow);
  binding.resource = pacing::ResourceRef{ResourceId::from(resource), Generation::from(1)};
  binding.path = pacing::PathRef{pacing::PathId::from(path), Generation::from(1)};
  return binding;
}

PacingPolicy make_policy(u64 policy, u64 resource, u64 path, u64 rate_bps) {
  PacingPolicy value{};
  value.ref.id = PolicyId::from(policy);
  value.ref.generation = Generation::unknown();
  value.layer = PacingLayer::Resource;
  value.resource = ResourceId::from(resource);
  value.path = pacing::PathRef{pacing::PathId::from(path), Generation::from(1)};
  value.rate_bps = rate_bps;
  value.quantum_bytes = 1500;
  value.window_ns = 1'000'000;
  return value;
}

}  // namespace

PF_TEST(multiproc, daemon_serves_multiple_worker_processes) {
  Daemon daemon;
  PF_CHECK(daemon.start(DaemonOptions{}));

  ControlClient first;
  PF_CHECK(connect_client(first, daemon.port(), "test-agent-1"));
  ControlClient second;
  PF_CHECK(connect_client(second, daemon.port(), "test-agent-2"));

  PF_CHECK_RESPONSE(first.bind_flow(make_binding(100, 1, 1)), ErrorCode::Ok);
  PF_CHECK_RESPONSE(first.publish_policy(make_policy(1, 1, 1, 1'000'000'000ull)), ErrorCode::Ok);

  const ControlResponse derived = second.derive_envelope(FlowId::from(100));
  PF_CHECK_RESPONSE(derived, ErrorCode::Ok);
  ByteReader envelope_reader(std::span<const u8>(derived.payload.data(), derived.payload.size()));
  PacingEnvelope envelope{};
  PF_CHECK_OK(decode_envelope_payload(envelope_reader, Limits{}, envelope));
  PF_CHECK(envelope.authority.complete());
  PF_CHECK_EQ(envelope.cadence.rate_bps, 1'000'000'000ull);

  const ControlResponse applied =
      second.apply_envelope(envelope.ref, pacing::BackendId::from(9));
  PF_CHECK_RESPONSE(applied, ErrorCode::Ok);
  ByteReader attempt_reader(std::span<const u8>(applied.payload.data(), applied.payload.size()));
  const AttemptId attempt = AttemptId::from(attempt_reader.u64v());
  PF_CHECK(attempt.valid());

  const ControlResponse verified = first.verify_attempt(attempt);
  PF_CHECK_RESPONSE(verified, ErrorCode::Ok);
  ByteReader record_reader(std::span<const u8>(verified.payload.data(), verified.payload.size()));
  ApplicationRecord record{};
  PF_CHECK_OK(decode_attempt_payload(record_reader, Limits{}, record));
  PF_CHECK_EQ(record.state, ApplicationState::Applied);
  PF_CHECK_EQ(record.effect, EffectLabel::SyntheticVerified);
  PF_CHECK(record.backend_synthetic);

  const ControlResponse explained = first.explain(FlowId::from(100));
  PF_CHECK_RESPONSE(explained, ErrorCode::Ok);

  // A second worker sees the same authoritative state: this is one coordinator
  // shared by real processes, not per-process bookkeeping.
  const ControlResponse from_second = second.verify_attempt(attempt);
  PF_CHECK_RESPONSE(from_second, ErrorCode::Ok);

  PF_CHECK_RESPONSE(first.stop(), ErrorCode::Ok);
  int exit_code = -1;
  PF_CHECK(daemon.child().wait(exit_code));
  PF_CHECK_EQ(exit_code, 0);
}

PF_TEST(multiproc, hard_kill_and_restart_fences_the_previous_epoch) {
  const std::string durable =
      (std::filesystem::temp_directory_path() / "pacing_fabric_multiproc_durable").string();
  std::error_code ec;
  std::filesystem::remove_all(durable, ec);
  std::filesystem::create_directories(durable, ec);

  DaemonOptions options{};
  options.durable_directory = durable;

  u64 epoch_before = 0;
  u64 boot_before = 0;
  AttemptId attempt{};
  {
    Daemon daemon;
    PF_CHECK(daemon.start(options));
    ControlClient client;
    PF_CHECK(connect_client(client, daemon.port(), "durable-agent"));
    epoch_before = client.stamp().epoch;
    boot_before = client.stamp().boot_nonce;

    PF_CHECK_RESPONSE(client.bind_flow(make_binding(100, 1, 1)), ErrorCode::Ok);
    PF_CHECK_RESPONSE(client.publish_policy(make_policy(1, 1, 1, 1'000'000'000ull)), ErrorCode::Ok);
    const ControlResponse derived = client.derive_envelope(FlowId::from(100));
    PF_CHECK_RESPONSE(derived, ErrorCode::Ok);
    ByteReader envelope_reader(std::span<const u8>(derived.payload.data(), derived.payload.size()));
    PacingEnvelope envelope{};
    PF_CHECK_OK(decode_envelope_payload(envelope_reader, Limits{}, envelope));
    const ControlResponse applied = client.apply_envelope(envelope.ref, pacing::BackendId::from(9));
    PF_CHECK_RESPONSE(applied, ErrorCode::Ok);
    ByteReader attempt_reader(std::span<const u8>(applied.payload.data(), applied.payload.size()));
    attempt = AttemptId::from(attempt_reader.u64v());

    // Hard kill: no cooperative shutdown, no flush, no unwinding.
    PF_CHECK(daemon.child().kill_hard());
    int exit_code = 0;
    PF_CHECK(daemon.child().wait(exit_code));
    client.close();
  }

  Daemon restarted;
  PF_CHECK(restarted.start(options));
  ControlClient client;
  PF_CHECK(connect_client(client, restarted.port(), "durable-agent"));
  PF_CHECK(client.stamp().epoch > epoch_before);
  PF_CHECK_NE(client.stamp().boot_nonce, boot_before);

  // A request stamped with the pre-restart epoch is fenced, never reinterpreted.
  CallerStamp stale{};
  stale.epoch = epoch_before;
  stale.boot_counter = 1;
  stale.boot_nonce = boot_before;
  ByteWriter explain_fields(64);
  explain_fields.u64v(100);
  const ControlResponse fenced =
      client.call_with_stamp(Operation::Explain, explain_fields.take(), stale);
  PF_CHECK_EQ(fenced.code, ErrorCode::StaleEpoch);

  CallerStamp stale_boot = client.stamp();
  stale_boot.boot_nonce = boot_before;
  const ControlResponse boot_fenced = client.call_with_stamp(Operation::Epoch, {}, stale_boot);
  PF_CHECK_EQ(boot_fenced.code, ErrorCode::Ok);  // epoch inspection is always allowed

  const ControlResponse recovered = client.recover();
  PF_CHECK_RESPONSE(recovered, ErrorCode::Ok);
  ByteReader report_reader(std::span<const u8>(recovered.payload.data(), recovered.payload.size()));
  const bool performed = report_reader.boolean();
  const bool used_directory = report_reader.boolean();
  (void)report_reader.u64v();  // bindings restored
  (void)report_reader.u64v();  // policies restored
  const u64 envelopes_invalidated = report_reader.u64v();
  const u64 requiring_revalidation = report_reader.u64v();
  const u64 ambiguous = report_reader.u64v();
  const u64 backends_requiring_registration = report_reader.u64v();
  PF_CHECK_OK(report_reader.status());
  PF_CHECK(performed);
  PF_CHECK(used_directory);
  PF_CHECK(envelopes_invalidated >= 1);
  PF_CHECK(requiring_revalidation >= 1);
  PF_CHECK_EQ(ambiguous, 0ull);
  PF_CHECK(backends_requiring_registration >= 1);

  // Nothing that was applied before the kill is restored as current fact.
  const ControlResponse explained = client.explain(FlowId::from(100));
  PF_CHECK_RESPONSE(explained, ErrorCode::Ok);
  ByteReader explain_reader(std::span<const u8>(explained.payload.data(), explained.payload.size()));
  const std::string json = explain_reader.str(Limits{}.max_explanation_bytes);
  PF_CHECK_OK(explain_reader.status());
  PF_CHECK(json.find("REQUIRES_REVALIDATION") != std::string::npos ||
           json.find("STALE") != std::string::npos);
  PF_CHECK(json.find("SYNTHETIC_VERIFIED") == std::string::npos);

  // New work under the new epoch succeeds normally.
  PF_CHECK_RESPONSE(client.bind_flow(make_binding(100, 1, 1)), ErrorCode::Ok);
  PF_CHECK_RESPONSE(client.publish_policy(make_policy(1, 1, 1, 1'000'000'000ull)), ErrorCode::Ok);
  const ControlResponse fresh = client.derive_envelope(FlowId::from(100));
  PF_CHECK_RESPONSE(fresh, ErrorCode::Ok);
  ByteReader fresh_reader(std::span<const u8>(fresh.payload.data(), fresh.payload.size()));
  PacingEnvelope fresh_envelope{};
  PF_CHECK_OK(decode_envelope_payload(fresh_reader, Limits{}, fresh_envelope));
  PF_CHECK(fresh_envelope.authority.epoch.value() > epoch_before);
  const ControlResponse fresh_applied =
      client.apply_envelope(fresh_envelope.ref, pacing::BackendId::from(9));
  PF_CHECK_RESPONSE(fresh_applied, ErrorCode::Ok);

  PF_CHECK_RESPONSE(client.stop(), ErrorCode::Ok);
  int exit_code = 0;
  PF_CHECK(restarted.child().wait(exit_code));
  PF_CHECK_EQ(exit_code, 0);
  (void)attempt;

  std::filesystem::remove_all(durable, ec);
}

PF_TEST(multiproc, worker_process_death_leaves_the_coordinator_healthy) {
  Daemon daemon;
  PF_CHECK(daemon.start(DaemonOptions{}));
  PF_CHECK(!agent_path().empty());

  ControlClient controller;
  PF_CHECK(connect_client(controller, daemon.port(), "controller"));
  PF_CHECK_RESPONSE(controller.bind_flow(make_binding(100, 1, 1)), ErrorCode::Ok);
  PF_CHECK_RESPONSE(controller.publish_policy(make_policy(1, 1, 1, 1'000'000'000ull)), ErrorCode::Ok);

  const std::string ready_file =
      (std::filesystem::path(daemon.workspace()) / "agent-ready.txt").string();
  pf_proc::Child agent;
  std::vector<std::string> agent_arguments{"--port",        std::to_string(daemon.port()),
                                           "--flow",        "100",
                                           "--backend",     "9",
                                           "--iterations",  "100000",
                                           "--pause-ms",    "2",
                                           "--ready-file",  ready_file};
  PF_CHECK(pf_proc::Child::spawn(agent_path(), agent_arguments, agent));

  // Wait until the worker has actually reached the coordinator, then kill it
  // outright: no cooperative shutdown, no flush, no unwinding.
  PF_CHECK(pf_proc::wait_for_file(ready_file, 4000, 25));
  PF_CHECK(agent.running());
  PF_CHECK(agent.kill_hard());
  int exit_code = 0;
  PF_CHECK(agent.wait(exit_code));

  // The coordinator must still be serving.
  const ControlResponse derived = controller.derive_envelope(FlowId::from(100));
  PF_CHECK_RESPONSE(derived, ErrorCode::Ok);
  ByteReader reader(std::span<const u8>(derived.payload.data(), derived.payload.size()));
  PacingEnvelope envelope{};
  PF_CHECK_OK(decode_envelope_payload(reader, Limits{}, envelope));
  const ControlResponse applied = controller.apply_envelope(envelope.ref, pacing::BackendId::from(9));
  PF_CHECK_RESPONSE(applied, ErrorCode::Ok);

  const ControlResponse stats = controller.stats();
  PF_CHECK_RESPONSE(stats, ErrorCode::Ok);

  PF_CHECK_RESPONSE(controller.stop(), ErrorCode::Ok);
  PF_CHECK(daemon.child().wait(exit_code));
  PF_CHECK_EQ(exit_code, 0);
}

PF_TEST(multiproc, malformed_frames_do_not_kill_the_coordinator) {
  Daemon daemon;
  PF_CHECK(daemon.start(DaemonOptions{}));

  ControlClient healthy;
  PF_CHECK(connect_client(healthy, daemon.port(), "healthy"));
  PF_CHECK_RESPONSE(healthy.bind_flow(make_binding(100, 1, 1)), ErrorCode::Ok);
  PF_CHECK_RESPONSE(healthy.publish_policy(make_policy(1, 1, 1, 1'000'000'000ull)), ErrorCode::Ok);

  // A peer that speaks valid framing but sends an unrecognised operation is
  // answered with a fault, not with a dropped connection or a silent success.
  {
    FramedConnection hostile;
    PF_CHECK_OK(hostile.connect_loopback(daemon.port(), 400, 25));
    Frame hello{};
    hello.type = FrameType::Hello;
    hello.request_id = 1;
    {
      ByteWriter writer(256);
      writer.str("hostile");
      CallerStamp stamp{};
      encode_stamp(writer, stamp);
      hello.payload = writer.take();
    }
    PF_CHECK_OK(hostile.send(hello, Limits{}));
    Frame welcome{};
    PF_CHECK_OK(hostile.receive(welcome, Limits{}));
    PF_CHECK_EQ(welcome.type, FrameType::Response);

    ByteWriter garbage_writer(256);
    garbage_writer.u16v(4000);  // not an Operation
    CallerStamp stamp{};
    encode_stamp(garbage_writer, stamp);
    garbage_writer.u32v(0);
    Frame request{};
    request.type = FrameType::Request;
    request.request_id = 2;
    request.payload = garbage_writer.take();
    PF_CHECK_OK(hostile.send(request, Limits{}));

    Frame fault{};
    PF_CHECK_OK(hostile.receive(fault, Limits{}));
    PF_CHECK_EQ(fault.type, FrameType::Fault);
    ByteReader fault_reader(std::span<const u8>(fault.payload.data(), fault.payload.size()));
    ControlResponse response{};
    PF_CHECK_OK(decode_response(fault_reader, Limits{}, response));
    PF_CHECK_EQ(response.code, ErrorCode::MalformedInput);

    // A request before any handshake is refused rather than guessed at.
    Frame early{};
    early.type = FrameType::Request;
    early.request_id = 3;
    {
      ByteWriter writer(256);
      writer.u16v(static_cast<u16>(Operation::Stats));
      CallerStamp unstamped{};
      encode_stamp(writer, unstamped);
      writer.u32v(0);
      early.payload = writer.take();
    }
    PF_CHECK_OK(hostile.send(early, Limits{}));
    Frame early_fault{};
    PF_CHECK_OK(hostile.receive(early_fault, Limits{}));
    PF_CHECK_EQ(early_fault.type, FrameType::Fault);
    hostile.close();
  }

  // A peer that corrupts the framing itself is disconnected, and the
  // coordinator is unaffected.
  {
    FramedConnection broken;
    PF_CHECK_OK(broken.connect_loopback(daemon.port(), 400, 25));
    std::vector<u8> bad_magic(24 + 8, 0);
    bad_magic[0] = 'X';
    const std::int64_t handle = broken.native_handle();
#if defined(_WIN32)
    (void)::send(static_cast<SOCKET>(handle), reinterpret_cast<const char*>(bad_magic.data()),
                 static_cast<int>(bad_magic.size()), 0);
#else
    (void)::send(static_cast<int>(handle), bad_magic.data(), bad_magic.size(), 0);
#endif
    broken.close();
  }

  // A peer that disconnects immediately after connecting.
  {
    FramedConnection abrupt;
    PF_CHECK_OK(abrupt.connect_loopback(daemon.port(), 400, 25));
    abrupt.close();
  }

  // The coordinator is unharmed and still authoritative.
  const ControlResponse derived = healthy.derive_envelope(FlowId::from(100));
  PF_CHECK_RESPONSE(derived, ErrorCode::Ok);
  const ControlResponse epoch = healthy.epoch();
  PF_CHECK_RESPONSE(epoch, ErrorCode::Ok);

  PF_CHECK_RESPONSE(healthy.stop(), ErrorCode::Ok);
  int exit_code = 0;
  PF_CHECK(daemon.child().wait(exit_code));
  PF_CHECK_EQ(exit_code, 0);
}

PF_TEST(multiproc, epoch_advance_fences_live_worker_authority) {
  Daemon daemon;
  PF_CHECK(daemon.start(DaemonOptions{}));
  ControlClient client;
  PF_CHECK(connect_client(client, daemon.port(), "epoch-agent"));
  PF_CHECK_RESPONSE(client.bind_flow(make_binding(100, 1, 1)), ErrorCode::Ok);
  PF_CHECK_RESPONSE(client.publish_policy(make_policy(1, 1, 1, 1'000'000'000ull)), ErrorCode::Ok);

  const ControlResponse derived = client.derive_envelope(FlowId::from(100));
  PF_CHECK_RESPONSE(derived, ErrorCode::Ok);
  ByteReader reader(std::span<const u8>(derived.payload.data(), derived.payload.size()));
  PacingEnvelope envelope{};
  PF_CHECK_OK(decode_envelope_payload(reader, Limits{}, envelope));

  const ControlResponse applied = client.apply_envelope(envelope.ref, pacing::BackendId::from(9));
  PF_CHECK_RESPONSE(applied, ErrorCode::Ok);

  const u64 before = client.stamp().epoch;
  PF_CHECK_RESPONSE(client.advance_epoch(), ErrorCode::Ok);
  PF_CHECK(client.stamp().epoch == before);  // the client's stamp is now stale

  // The client must re-handshake; the old stamp is refused.
  const ControlResponse refused = client.apply_envelope(envelope.ref, pacing::BackendId::from(9));
  PF_CHECK_EQ(refused.code, ErrorCode::StaleEpoch);

  PF_CHECK_OK(client.handshake("epoch-agent"));
  PF_CHECK(client.stamp().epoch > before);
  const ControlResponse revoked = client.apply_envelope(envelope.ref, pacing::BackendId::from(9));
  PF_CHECK_EQ(revoked.code, ErrorCode::Revoked);

  PF_CHECK_RESPONSE(client.stop(), ErrorCode::Ok);
  int exit_code = 0;
  PF_CHECK(daemon.child().wait(exit_code));
  PF_CHECK_EQ(exit_code, 0);
}
