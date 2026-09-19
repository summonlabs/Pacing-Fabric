// Adversarial input handling: truncated, oversized, contradictory and
// structurally invalid input must be refused, never absorbed.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "framework.hpp"
#include "harness.hpp"
#include "pacing/crc32c.hpp"
#include "pacing/serialize.hpp"
#include "pacing/transport.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

using namespace pacing;
using pf_test::Harness;
using pf_test::HarnessOptions;
using pf_test::LoopbackBackend;

namespace {

AuthorityVector sample_authority() {
  AuthorityVector authority{};
  authority.flow = FlowRef{FlowId::from(4), Generation::from(1)};
  authority.resource = ResourceRef{ResourceId::from(5), Generation::from(1)};
  authority.policy = PolicyRef{PolicyId::from(6), Generation::from(1)};
  authority.rate_grant = pacing::GrantRef{pacing::GrantId::from(7), Generation::from(1)};
  authority.path = pacing::PathRef{pacing::PathId::from(8), Generation::from(1)};
  authority.epoch = Epoch::from(1);
  authority.coordinator_boot = BootId{1, 2};
  return authority;
}

Cadence sample_cadence() {
  Cadence cadence{};
  cadence.rate_bps = 1'000'000'000ull;
  cadence.quantum_bytes = 1500;
  cadence.interval_ns = 12'000;
  cadence.window_ns = 1'000'000;
  cadence.grants_per_window = 83;
  cadence.bytes_per_window = 124'500;
  cadence.burst_bytes = 1500;
  cadence.burst_packets = 1;
  cadence.shape = CadenceShape::Uniform;
  return cadence;
}

ApplicationRecord sample_record() {
  ApplicationRecord record{};
  record.attempt = AttemptId::from(11);
  record.envelope = EnvelopeRef{EnvelopeId::from(3), Generation::from(1)};
  record.flow = FlowId::from(4);
  record.resource = ResourceId::from(5);
  record.authority = sample_authority();
  record.desired = sample_cadence();
  record.state = ApplicationState::Applied;
  record.last_known_state = ApplicationState::Acknowledged;
  record.effect = EffectLabel::SyntheticVerified;
  record.evidence_kind = EvidenceKind::SyntheticReadback;
  record.observed_rate_bps = 1'000'000'000ull;
  record.observed_quantum_bytes = 1500;
  record.observed_burst_bytes = 1500;
  record.observed_interval_ns = 12'000;
  record.backend = BackendRef{BackendId::from(9), Generation::from(1)};
  record.backend_synthetic = true;
  record.detail = "sample";
  record.revision = 3;
  return record;
}

PacingEnvelope sample_envelope() {
  PacingEnvelope envelope{};
  envelope.ref = EnvelopeRef{EnvelopeId::from(3), Generation::from(1)};
  envelope.flow = FlowId::from(4);
  envelope.resource = ResourceId::from(5);
  envelope.authority = sample_authority();
  envelope.cadence = sample_cadence();
  envelope.ceiling_bps = 10'000'000'000ull;
  envelope.valid_until = Deadline::at(Instant::from_ns(1'000'000'000ull));
  return envelope;
}

FlowBinding sample_binding() {
  FlowBinding binding{};
  binding.flow = FlowId::from(4);
  binding.generation = Generation::from(1);
  binding.resource = ResourceRef{ResourceId::from(5), Generation::from(1)};
  binding.path = pacing::PathRef{pacing::PathId::from(8), Generation::from(1)};
  return binding;
}

PacingPolicy sample_policy() {
  PacingPolicy policy{};
  policy.ref = PolicyRef{PolicyId::from(6), Generation::from(1)};
  policy.layer = PacingLayer::Resource;
  policy.resource = ResourceId::from(5);
  policy.path = pacing::PathRef{pacing::PathId::from(8), Generation::from(1)};
  policy.quantum_bytes = 1500;
  policy.window_ns = 1'000'000;
  return policy;
}

BackendDescriptor sample_descriptor() {
  BackendDescriptor descriptor{};
  descriptor.ref = BackendRef{BackendId::from(9), Generation::from(1)};
  descriptor.name = "loopback-synthetic";
  descriptor.nature = BackendNature::Synthetic;
  descriptor.supports_readback = true;
  return descriptor;
}

RateGrant sample_grant() {
  RateGrant grant{};
  grant.ref = pacing::GrantRef{pacing::GrantId::from(7), Generation::from(1)};
  grant.resource = ResourceId::from(5);
  grant.ceiling_bps = 10'000'000'000ull;
  grant.authoritative = true;
  return grant;
}

std::vector<u8> encode_record() {
  ByteWriter writer(kEncodeCapacityBytes);
  encode_attempt(writer, sample_record());
  PF_CHECK(writer.ok());
  return writer.take();
}

std::vector<u8> encode_envelope_bytes() {
  ByteWriter writer(kEncodeCapacityBytes);
  encode_envelope(writer, sample_envelope());
  PF_CHECK(writer.ok());
  return writer.take();
}

std::vector<u8> encode_binding_bytes() {
  ByteWriter writer(kEncodeCapacityBytes);
  encode_binding(writer, sample_binding());
  PF_CHECK(writer.ok());
  return writer.take();
}

std::vector<u8> encode_policy_bytes() {
  ByteWriter writer(kEncodeCapacityBytes);
  encode_policy(writer, sample_policy());
  PF_CHECK(writer.ok());
  return writer.take();
}

std::vector<u8> encode_grant_bytes() {
  ByteWriter writer(kEncodeCapacityBytes);
  encode_grant(writer, sample_grant());
  PF_CHECK(writer.ok());
  return writer.take();
}

std::vector<u8> encode_descriptor_bytes() {
  ByteWriter writer(kEncodeCapacityBytes);
  encode_backend_descriptor(writer, sample_descriptor());
  PF_CHECK(writer.ok());
  return writer.take();
}

// Writes raw bytes to a connection, bypassing the framing layer. Used only to
// prove that the framing layer rejects a hostile stream.
bool raw_send(std::int64_t handle, const std::vector<u8>& bytes) {
#if defined(_WIN32)
  const int sent = ::send(static_cast<SOCKET>(handle), reinterpret_cast<const char*>(bytes.data()),
                          static_cast<int>(bytes.size()), 0);
  return sent == static_cast<int>(bytes.size());
#else
  const auto sent = ::send(static_cast<int>(handle), bytes.data(), bytes.size(), 0);
  return sent == static_cast<ssize_t>(bytes.size());
#endif
}

void put_u16(std::vector<u8>& out, std::size_t offset, u16 value) {
  out[offset] = static_cast<u8>(value & 0xFF);
  out[offset + 1] = static_cast<u8>((value >> 8) & 0xFF);
}

void put_u32(std::vector<u8>& out, std::size_t offset, u32 value) {
  for (int i = 0; i < 4; ++i) out[offset + static_cast<std::size_t>(i)] = static_cast<u8>((value >> (8 * i)) & 0xFF);
}

void put_u64(std::vector<u8>& out, std::size_t offset, u64 value) {
  for (int i = 0; i < 8; ++i) out[offset + static_cast<std::size_t>(i)] = static_cast<u8>((value >> (8 * i)) & 0xFF);
}

// Builds a wire-legal frame from scratch so a test can corrupt it afterwards.
std::vector<u8> build_raw_frame(u64 request_id, const std::vector<u8>& payload) {
  std::vector<u8> bytes(kFrameHeaderBytes + payload.size(), 0);
  put_u32(bytes, 0, 0x31524650u);
  put_u16(bytes, 4, kWireProtocolVersion);
  put_u16(bytes, 6, static_cast<u16>(FrameType::Request));
  put_u64(bytes, 8, request_id);
  put_u32(bytes, 16, static_cast<u32>(payload.size()));
  const std::span<const u8> header(bytes.data() + 4, 16);
  put_u32(bytes, 20, crc32c_extend(crc32c(header), payload));
  std::memcpy(bytes.data() + kFrameHeaderBytes, payload.data(), payload.size());
  return bytes;
}

struct SocketPair {
  FrameListener listener;
  FramedConnection client;
  FramedConnection server;
};

bool make_socket_pair(SocketPair& pair) {
  u16 port = 0;
  if (!pair.listener.listen_loopback(0, port).ok()) return false;
  if (!pair.client.connect_loopback(port, 4, 1).ok()) return false;
  return pair.listener.accept(pair.server).ok();
}

}  // namespace

PF_TEST(adversarial, encoders_roundtrip_and_consume_every_byte) {
  Limits limits{};
  {
    const std::vector<u8> bytes = encode_record();
    ByteReader reader(bytes);
    ApplicationRecord decoded{};
    PF_CHECK_OK(decode_attempt(reader, limits, decoded));
    PF_CHECK(reader.exhausted());
    PF_CHECK_EQ(decoded.attempt.value(), 11ull);
    PF_CHECK_EQ(decoded.digest(), sample_record().digest());
  }
  {
    const std::vector<u8> bytes = encode_envelope_bytes();
    ByteReader reader(bytes);
    PacingEnvelope decoded{};
    PF_CHECK_OK(decode_envelope(reader, limits, decoded));
    PF_CHECK(reader.exhausted());
    PF_CHECK_EQ(decoded.digest(), sample_envelope().digest());
  }
  {
    const std::vector<u8> bytes = encode_binding_bytes();
    ByteReader reader(bytes);
    FlowBinding decoded{};
    PF_CHECK_OK(decode_binding(reader, limits, decoded));
    PF_CHECK(reader.exhausted());
    PF_CHECK_EQ(decoded.digest(), sample_binding().digest());
  }
  {
    const std::vector<u8> bytes = encode_policy_bytes();
    ByteReader reader(bytes);
    PacingPolicy decoded{};
    PF_CHECK_OK(decode_policy(reader, limits, decoded));
    PF_CHECK(reader.exhausted());
    PF_CHECK_EQ(decoded.ref.id.value(), 6ull);
  }
  {
    const std::vector<u8> bytes = encode_grant_bytes();
    ByteReader reader(bytes);
    RateGrant decoded{};
    PF_CHECK_OK(decode_grant(reader, limits, decoded));
    PF_CHECK(reader.exhausted());
    PF_CHECK_EQ(decoded.digest(), sample_grant().digest());
  }
  {
    const std::vector<u8> bytes = encode_descriptor_bytes();
    ByteReader reader(bytes);
    BackendDescriptor decoded{};
    PF_CHECK_OK(decode_backend_descriptor(reader, limits, decoded));
    PF_CHECK(reader.exhausted());
    PF_CHECK_EQ(decoded.name, std::string("loopback-synthetic"));
  }
}

PF_TEST(adversarial, every_truncated_prefix_is_refused) {
  Limits limits{};

  const std::vector<u8> record_bytes = encode_record();
  for (std::size_t cut = 0; cut < record_bytes.size(); ++cut) {
    ByteReader reader(std::span<const u8>(record_bytes.data(), cut));
    ApplicationRecord decoded{};
    const Status status = decode_attempt(reader, limits, decoded);
    PF_CHECK(!status.ok() || !reader.exhausted());
  }

  const std::vector<u8> envelope_bytes = encode_envelope_bytes();
  for (std::size_t cut = 0; cut < envelope_bytes.size(); ++cut) {
    ByteReader reader(std::span<const u8>(envelope_bytes.data(), cut));
    PacingEnvelope decoded{};
    const Status status = decode_envelope(reader, limits, decoded);
    PF_CHECK(!status.ok() || !reader.exhausted());
  }

  const std::vector<u8> policy_bytes = encode_policy_bytes();
  for (std::size_t cut = 0; cut < policy_bytes.size(); ++cut) {
    ByteReader reader(std::span<const u8>(policy_bytes.data(), cut));
    PacingPolicy decoded{};
    const Status status = decode_policy(reader, limits, decoded);
    PF_CHECK(!status.ok() || !reader.exhausted());
  }

  const std::vector<u8> grant_bytes = encode_grant_bytes();
  for (std::size_t cut = 0; cut < grant_bytes.size(); ++cut) {
    ByteReader reader(std::span<const u8>(grant_bytes.data(), cut));
    RateGrant decoded{};
    const Status status = decode_grant(reader, limits, decoded);
    PF_CHECK(!status.ok() || !reader.exhausted());
  }
}

PF_TEST(adversarial, trailing_bytes_are_detected) {
  Limits limits{};
  std::vector<u8> bytes = encode_binding_bytes();
  bytes.push_back(0xAB);
  ByteReader reader(bytes);
  FlowBinding decoded{};
  PF_CHECK_OK(decode_binding(reader, limits, decoded));
  PF_CHECK(!reader.exhausted());
}

PF_TEST(adversarial, oversized_length_prefix_is_refused_without_allocating) {
  Limits limits{};
  std::vector<u8> bytes;
  const u32 hostile = 0xFFFFFFF0u;
  for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<u8>((hostile >> (8 * i)) & 0xFF));
  bytes.resize(bytes.size() + 64, 0);
  ByteReader reader(bytes);
  BackendDescriptor decoded{};
  const Status status = decode_backend_descriptor(reader, limits, decoded);
  PF_CHECK(!status.ok());
  PF_CHECK(status.code() == ErrorCode::Oversized || status.code() == ErrorCode::MalformedInput);
}

PF_TEST(adversarial, cadence_shape_discriminator_is_range_checked) {
  ByteWriter writer(128);
  writer.u64v(1'000'000'000ull);
  writer.u64v(1500);
  writer.u64v(12'000);
  writer.u64v(1'000'000);
  writer.u64v(83);
  writer.u64v(124'500);
  writer.u64v(1500);
  writer.u64v(1);
  writer.u8v(9);  // not a CadenceShape
  PF_CHECK(writer.ok());
  std::vector<u8> forged = writer.take();
  ByteReader reader(forged);
  Cadence cadence{};
  PF_CHECK_STATUS_CODE(decode_cadence(reader, cadence), ErrorCode::MalformedInput);
}

PF_TEST(adversarial, backend_nature_discriminator_is_range_checked) {
  Limits limits{};
  std::vector<u8> bytes = encode_descriptor_bytes();
  const std::size_t nature_offset = 16 + 4 + sample_descriptor().name.size();
  PF_CHECK(bytes.size() > nature_offset);
  bytes[nature_offset] = 7;  // not a BackendNature
  ByteReader reader(bytes);
  BackendDescriptor decoded{};
  PF_CHECK_STATUS_CODE(decode_backend_descriptor(reader, limits, decoded), ErrorCode::MalformedInput);
}

PF_TEST(adversarial, writer_capacity_is_poisoned_not_exceeded) {
  ByteWriter writer(16);
  for (int i = 0; i < 64; ++i) writer.u64v(1);
  PF_CHECK(!writer.ok());
  PF_CHECK_STATUS_CODE(writer.status(), ErrorCode::Oversized);
  const std::size_t size_after_poison = writer.size();
  for (int i = 0; i < 64; ++i) writer.u64v(1);
  PF_CHECK_EQ(writer.size(), size_after_poison);
}

PF_TEST(adversarial, envelopes_above_their_recorded_ceiling_are_refused) {
  Limits limits{};
  PacingEnvelope forged = sample_envelope();
  forged.cadence.rate_bps = forged.ceiling_bps + 1;
  ByteWriter writer(kEncodeCapacityBytes);
  encode_envelope(writer, forged);
  PF_CHECK(writer.ok());
  std::vector<u8> bytes = writer.take();
  ByteReader reader(bytes);
  PacingEnvelope decoded{};
  PF_CHECK_STATUS_CODE(decode_envelope(reader, limits, decoded), ErrorCode::CeilingExceeded);
}

PF_TEST(adversarial, incomplete_authority_is_refused) {
  Limits limits{};
  ApplicationRecord forged = sample_record();
  forged.authority.policy = PolicyRef{};
  ByteWriter writer(kEncodeCapacityBytes);
  encode_attempt(writer, forged);
  PF_CHECK(writer.ok());
  std::vector<u8> bytes = writer.take();
  ByteReader reader(bytes);
  ApplicationRecord decoded{};
  PF_CHECK_STATUS_CODE(decode_attempt(reader, limits, decoded), ErrorCode::MalformedInput);
}

PF_TEST(adversarial, attempt_state_discriminator_is_range_checked) {
  Limits limits{};
  std::vector<u8> bytes = encode_record();
  // Layout: five identity u64s, thirteen authority u64s, the cadence block
  // (eight u64s plus a shape byte), then the four enum discriminators.
  const std::size_t state_offset = (5 + 13) * 8 + (8 * 8 + 1);
  PF_CHECK(bytes.size() > state_offset);
  // The offset is self-validating: it must land exactly on the encoded state.
  PF_CHECK_EQ(bytes[state_offset], static_cast<u8>(ApplicationState::Applied));
  bytes[state_offset] = 0xFF;  // not an ApplicationState
  ByteReader reader(bytes);
  ApplicationRecord decoded{};
  PF_CHECK_STATUS_CODE(decode_attempt(reader, limits, decoded), ErrorCode::MalformedInput);
}

PF_TEST(adversarial, grant_bounds_are_enforced_on_decode) {
  Limits limits{};
  {
    RateGrant forged = sample_grant();
    forged.floor_bps = forged.ceiling_bps + 1;
    ByteWriter writer(256);
    encode_grant(writer, forged);
    PF_CHECK(writer.ok());
    std::vector<u8> bytes = writer.take();
    ByteReader reader(bytes);
    RateGrant decoded{};
    PF_CHECK_STATUS_CODE(decode_grant(reader, limits, decoded), ErrorCode::FloorExceedsCeiling);
  }
  {
    RateGrant too_fast = sample_grant();
    too_fast.ceiling_bps = limits.max_rate_bps + 1;
    ByteWriter writer(256);
    encode_grant(writer, too_fast);
    std::vector<u8> bytes = writer.take();
    ByteReader reader(bytes);
    RateGrant decoded{};
    PF_CHECK_STATUS_CODE(decode_grant(reader, limits, decoded), ErrorCode::OutOfRange);
  }
}

PF_TEST(adversarial, limit_sets_must_be_coherent) {
  Limits limits{};
  PF_CHECK(limits_are_coherent(limits));

  Limits zero_rate = limits;
  zero_rate.max_rate_bps = 0;
  PF_CHECK(!limits_are_coherent(zero_rate));

  Limits inverted_window = limits;
  inverted_window.min_window_ns = inverted_window.max_window_ns + 1;
  PF_CHECK(!limits_are_coherent(inverted_window));

  Limits record_bigger_than_journal = limits;
  record_bigger_than_journal.max_durable_record_bytes = limits.max_journal_bytes + 1;
  PF_CHECK(!limits_are_coherent(record_bigger_than_journal));

  Limits frame = limits;
  frame.max_frame_payload_bytes = frame.max_frame_bytes;
  PF_CHECK(!limits_are_coherent(frame));

  Limits zero_backends = limits;
  zero_backends.max_backends = 0;
  PF_CHECK(!limits_are_coherent(zero_backends));

  Limits zero_explanation = limits;
  zero_explanation.max_explanation_bytes = 0;
  PF_CHECK(!limits_are_coherent(zero_explanation));

  Limits zero_workers = limits;
  zero_workers.max_workers = 0;
  PF_CHECK(!limits_are_coherent(zero_workers));
}

PF_TEST(adversarial, malformed_configuration_is_refused_at_initialization) {
  {
    Limits broken{};
    broken.max_rate_bps = 0;
    HarnessOptions options{};
    options.limits = broken;
    Harness h{{}, 7, options};
    PF_CHECK_STATUS_CODE(h.fabric->initialize(), ErrorCode::InvalidArgument);
    PF_CHECK(!h.fabric->running());
  }
  {
    HarnessOptions options{};
    options.tick_period_ns = 0;
    Harness h{{}, 7, options};
    PF_CHECK_STATUS_CODE(h.fabric->initialize(), ErrorCode::InvalidArgument);
  }
  {
    HarnessOptions options{};
    options.envelope_lifetime_ns = 0;
    Harness h{{}, 7, options};
    PF_CHECK_STATUS_CODE(h.fabric->initialize(), ErrorCode::InvalidArgument);
  }
  {
    Limits limits{};
    const HarnessOptions options{limits, 1, 1'000'000'000ull, 1'000'000ull};
    Harness h{{}, 7, options};
    PF_CHECK_OK(h.fabric->initialize());
  }
}

PF_TEST(adversarial, fabric_rejects_malformed_api_input) {
  Harness h;
  h.start();
  h.install_grant();
  h.bind_flow(h.flow);
  h.publish_policy(1'000'000'000ull);
  h.register_backend();

  // A second initialize is an invalid state, not a silent no-op.
  PF_CHECK_STATUS_CODE(h.fabric->initialize(), ErrorCode::InvalidState);

  auto zero_flow = h.fabric->derive_envelope(FlowId{});
  PF_CHECK_CODE(zero_flow, ErrorCode::NotFound);

  auto zero_envelope = h.fabric->apply_envelope(EnvelopeRef{}, h.backend_id);
  PF_CHECK_CODE(zero_envelope, ErrorCode::InvalidArgument);

  const PacingEnvelope envelope = h.derive(h.flow);
  auto zero_backend = h.fabric->apply_envelope(envelope.ref, pacing::BackendId{});
  PF_CHECK_CODE(zero_backend, ErrorCode::InvalidArgument);

  auto wrong_generation =
      h.fabric->apply_envelope(EnvelopeRef{envelope.ref.id, Generation::from(77)}, h.backend_id);
  PF_CHECK_CODE(wrong_generation, ErrorCode::StaleGeneration);

  auto missing_envelope =
      h.fabric->apply_envelope(EnvelopeRef{EnvelopeId::from(999'999), Generation::from(1)}, h.backend_id);
  PF_CHECK_CODE(missing_envelope, ErrorCode::NotFound);

  PF_CHECK_CODE(h.fabric->attempt(AttemptId::from(999'999)), ErrorCode::NotFound);
  PF_CHECK_CODE(h.fabric->envelope(EnvelopeId::from(999'999)), ErrorCode::NotFound);
  PF_CHECK_CODE(h.fabric->policy(PolicyId::from(999'999)), ErrorCode::NotFound);
  PF_CHECK_STATUS_CODE(h.fabric->binding(FlowId::from(999'999)).status(), ErrorCode::NotFound);
  PF_CHECK_CODE(h.fabric->explain(FlowId{}), ErrorCode::InvalidArgument);
  PF_CHECK_STATUS_CODE(h.fabric->remove_policy(PolicyId::from(999'999)), ErrorCode::NotFound);
  PF_CHECK_STATUS_CODE(h.fabric->unbind_flow(FlowId::from(999'999)), ErrorCode::NotFound);
}

PF_TEST(adversarial, frames_roundtrip_and_reject_oversized_payloads) {
  Limits limits{};
  PF_CHECK_OK(transport_global_init());
  SocketPair pair;
  PF_CHECK(make_socket_pair(pair));

  Frame sent{};
  sent.type = FrameType::Request;
  sent.request_id = 4242;
  sent.payload = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  PF_CHECK_STATUS_CODE(pair.client.send(sent, limits), ErrorCode::Ok);

  Frame received{};
  PF_CHECK_STATUS_CODE(pair.server.receive(received, limits), ErrorCode::Ok);
  PF_CHECK_EQ(received.request_id, 4242ull);
  PF_CHECK_EQ(received.type, FrameType::Request);
  PF_CHECK(received.payload == sent.payload);

  Frame huge{};
  huge.payload.assign(limits.max_frame_payload_bytes + 1, 0);
  PF_CHECK_STATUS_CODE(pair.client.send(huge, limits), ErrorCode::Oversized);

  pair.client.close();
  pair.server.close();
  pair.listener.close();
  transport_global_shutdown();
}

namespace {

// Feeds one hostile frame to a fresh connection and returns the code the
// receiver produced. A fresh connection per case is required because a framing
// failure poisons the stream by design.
ErrorCode feed_hostile_frame(const Limits& limits, const std::vector<u8>& bytes, bool& poisoned) {
  SocketPair pair;
  PF_CHECK(make_socket_pair(pair));
  PF_CHECK(raw_send(pair.client.native_handle(), bytes));
  Frame received{};
  const Status status = pair.server.receive(received, limits);
  poisoned = pair.server.poisoned();
  pair.client.close();
  pair.server.close();
  pair.listener.close();
  return status.code();
}

}  // namespace

PF_TEST(adversarial, transport_rejects_corrupt_magic_and_checksum) {
  Limits limits{};
  PF_CHECK_OK(transport_global_init());

  bool poisoned = false;
  std::vector<u8> bad_magic = build_raw_frame(1, {1, 2, 3});
  bad_magic[0] = 'X';
  PF_CHECK_EQ(feed_hostile_frame(limits, bad_magic, poisoned), ErrorCode::MalformedInput);
  PF_CHECK(poisoned);

  std::vector<u8> bad_crc = build_raw_frame(2, {1, 2, 3});
  bad_crc[kFrameHeaderBytes] ^= 0xFF;  // payload no longer matches the checksum
  PF_CHECK_EQ(feed_hostile_frame(limits, bad_crc, poisoned), ErrorCode::IntegrityFailure);
  PF_CHECK(poisoned);

  std::vector<u8> bad_version = build_raw_frame(3, {1});
  bad_version[4] = 99;
  PF_CHECK_EQ(feed_hostile_frame(limits, bad_version, poisoned), ErrorCode::Unsupported);

  std::vector<u8> bad_type = build_raw_frame(4, {1});
  bad_type[6] = 0;
  PF_CHECK_EQ(feed_hostile_frame(limits, bad_type, poisoned), ErrorCode::MalformedInput);

  std::vector<u8> oversized = build_raw_frame(5, {1});
  put_u32(oversized, 16, limits.max_frame_payload_bytes + 1);
  PF_CHECK_EQ(feed_hostile_frame(limits, oversized, poisoned), ErrorCode::Oversized);

  // A desynchronised stream must never be reinterpreted as fresh frames.
  SocketPair pair;
  PF_CHECK(make_socket_pair(pair));
  std::vector<u8> corrupt = build_raw_frame(6, {1, 2, 3});
  corrupt[kFrameHeaderBytes] ^= 0xFF;
  PF_CHECK(raw_send(pair.client.native_handle(), corrupt));
  Frame received{};
  PF_CHECK_STATUS_CODE(pair.server.receive(received, limits), ErrorCode::IntegrityFailure);
  PF_CHECK(pair.server.poisoned());
  PF_CHECK_STATUS_CODE(pair.server.receive(received, limits), ErrorCode::InvalidState);
  PF_CHECK_STATUS_CODE(pair.server.send(Frame{}, limits), ErrorCode::InvalidState);
  pair.client.close();
  pair.server.close();
  pair.listener.close();

  transport_global_shutdown();
}

PF_TEST(adversarial, transport_rejects_a_truncated_frame) {
  Limits limits{};
  PF_CHECK_OK(transport_global_init());
  SocketPair pair;
  PF_CHECK(make_socket_pair(pair));

  std::vector<u8> partial = build_raw_frame(7, {9, 9, 9, 9, 9, 9});
  partial.resize(partial.size() - 3);  // sever the frame mid-payload
  PF_CHECK(raw_send(pair.client.native_handle(), partial));
  pair.client.close();

  Frame received{};
  PF_CHECK_STATUS_CODE(pair.server.receive(received, limits), ErrorCode::MalformedInput);

  // A severed frame desynchronises the stream, so the connection is poisoned
  // rather than being asked to reinterpret the remaining bytes.
  Frame next{};
  PF_CHECK_STATUS_CODE(pair.server.receive(next, limits), ErrorCode::InvalidState);

  pair.server.close();
  pair.listener.close();
  transport_global_shutdown();
}

PF_TEST(adversarial, a_closed_connection_refuses_further_frames) {
  Limits limits{};
  PF_CHECK_OK(transport_global_init());
  FramedConnection connection;
  const Frame probe{};
  PF_CHECK_STATUS_CODE(connection.send(probe, limits), ErrorCode::BackendUnavailable);
  Frame received{};
  PF_CHECK_STATUS_CODE(connection.receive(received, limits), ErrorCode::BackendUnavailable);
  PF_CHECK(!connection.valid());

  FrameListener listener;
  FramedConnection accepted;
  PF_CHECK_STATUS_CODE(listener.accept(accepted), ErrorCode::InvalidState);

  u16 port = 0;
  (void)port;
  PF_CHECK_OK(listener.listen_loopback(0, port));
  listener.close();
  PF_CHECK_STATUS_CODE(listener.accept(accepted), ErrorCode::InvalidState);
  transport_global_shutdown();
}
