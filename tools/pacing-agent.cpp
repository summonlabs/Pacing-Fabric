// pacing-agent - a real worker process that drives a coordinator over the
// framed control protocol.
//
// It is intentionally a separate operating-system process: the coordinator's
// fencing, restart and liveness behaviour is only meaningful when the caller
// can actually die.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "pacing/pacing.hpp"

namespace {

struct Options {
  pacing::u16 port{0};
  pacing::u64 flow{100};
  pacing::u64 resource{1};
  pacing::u64 path{1};
  pacing::u64 backend{9};
  pacing::u64 policy{1};
  pacing::u64 quantum{1500};
  pacing::u64 window_ns{1'000'000};
  pacing::u64 rate_bps{1'000'000'000ull};
  pacing::u64 iterations{1};
  pacing::u64 pause_ms{0};
  bool configure{false};
  bool verbose{false};
  std::string name{"pacing-agent"};
  std::string ready_file{};
};

void usage() {
  std::fprintf(stderr,
               "usage: pacing-agent --port N [--flow N] [--resource N] [--backend N]\n"
               "                    [--iterations N] [--pause-ms N] [--configure] [--verbose]\n");
}

bool parse_u64(const char* text, pacing::u64& out) {
  if (text == nullptr || *text == '\0') return false;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (end == nullptr || *end != '\0') return false;
  out = static_cast<pacing::u64>(value);
  return true;
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (arg == "--configure") {
      options.configure = true;
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--ready-file") {
      if (next == nullptr) return false;
      options.ready_file = next;
      ++i;
    } else if (arg == "--port" || arg == "--flow" || arg == "--resource" || arg == "--path" ||
               arg == "--backend" || arg == "--policy" || arg == "--quantum" || arg == "--window-ns" ||
               arg == "--rate-bps" || arg == "--iterations" || arg == "--pause-ms") {
      pacing::u64 value = 0;
      if (!parse_u64(next, value)) return false;
      if (arg == "--port") {
        if (value == 0 || value > 65535) return false;
        options.port = static_cast<pacing::u16>(value);
      } else if (arg == "--flow") options.flow = value;
      else if (arg == "--resource") options.resource = value;
      else if (arg == "--path") options.path = value;
      else if (arg == "--backend") options.backend = value;
      else if (arg == "--policy") options.policy = value;
      else if (arg == "--quantum") options.quantum = value;
      else if (arg == "--window-ns") options.window_ns = value;
      else if (arg == "--rate-bps") options.rate_bps = value;
      else if (arg == "--iterations") options.iterations = value;
      else options.pause_ms = value;
      ++i;
    } else {
      return false;
    }
  }
  return options.port != 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options{};
  if (!parse_options(argc, argv, options)) {
    usage();
    return 2;
  }
  if (!pacing::transport_global_init().ok()) {
    std::fprintf(stderr, "pacing-agent: transport initialisation failed\n");
    return 3;
  }

  pacing::ControlClient client;
  const pacing::Status connected = client.connect_loopback(options.port, 40, 25);
  if (!connected.ok()) {
    std::fprintf(stderr, "pacing-agent: connect failed: %s\n", connected.to_string().c_str());
    return 3;
  }
  const pacing::Status handshake = client.handshake(options.name);
  if (!handshake.ok()) {
    std::fprintf(stderr, "pacing-agent: handshake failed: %s\n", handshake.to_string().c_str());
    return 3;
  }
  std::printf("agent stamp epoch=%llu boot=%llu\n",
              static_cast<unsigned long long>(client.stamp().epoch),
              static_cast<unsigned long long>(client.stamp().boot_nonce));
  std::fflush(stdout);

  if (!options.ready_file.empty()) {
    std::FILE* marker = std::fopen(options.ready_file.c_str(), "wb");
    if (marker == nullptr) {
      std::fprintf(stderr, "pacing-agent: cannot write ready file\n");
      return 4;
    }
    std::fprintf(marker, "ready\n");
    std::fclose(marker);
  }

  if (options.configure) {
    pacing::FlowBinding binding{};
    binding.flow = pacing::FlowId::from(options.flow);
    binding.resource = pacing::ResourceRef{pacing::ResourceId::from(options.resource),
                                           pacing::Generation::from(1)};
    binding.path = pacing::PathRef{pacing::PathId::from(options.path), pacing::Generation::from(1)};
    const pacing::ControlResponse bound = client.bind_flow(binding);
    if (bound.code != pacing::ErrorCode::Ok) {
      std::fprintf(stderr, "pacing-agent: bind failed: %s\n", bound.detail.c_str());
      return 4;
    }
    pacing::PacingPolicy policy{};
    policy.ref.id = pacing::PolicyId::from(options.policy);
    policy.ref.generation = pacing::Generation::unknown();
    policy.layer = pacing::PacingLayer::Resource;
    policy.resource = pacing::ResourceId::from(options.resource);
    policy.path = pacing::PathRef{pacing::PathId::from(options.path), pacing::Generation::from(1)};
    policy.rate_bps = options.rate_bps;
    policy.quantum_bytes = options.quantum;
    policy.window_ns = options.window_ns;
    const pacing::ControlResponse published = client.publish_policy(policy);
    if (published.code != pacing::ErrorCode::Ok) {
      std::fprintf(stderr, "pacing-agent: publish failed: %s\n", published.detail.c_str());
      return 4;
    }
    std::printf("agent configured flow=%llu policy=%llu\n",
                static_cast<unsigned long long>(options.flow),
                static_cast<unsigned long long>(options.policy));
    std::fflush(stdout);
  }

  const pacing::Limits limits{};
  for (pacing::u64 iteration = 0; iteration < options.iterations; ++iteration) {
    const pacing::ControlResponse derived = client.derive_envelope(pacing::FlowId::from(options.flow));
    if (derived.code != pacing::ErrorCode::Ok) {
      std::printf("iteration %llu derive refused: %s: %s\n",
                  static_cast<unsigned long long>(iteration),
                  std::string(pacing::to_string(derived.code)).c_str(), derived.detail.c_str());
      std::fflush(stdout);
    } else {
      pacing::ByteReader reader(
          std::span<const pacing::u8>(derived.payload.data(), derived.payload.size()));
      pacing::PacingEnvelope envelope{};
      if (!pacing::decode_envelope_payload(reader, limits, envelope).ok()) {
        std::fprintf(stderr, "pacing-agent: malformed envelope reply\n");
        return 5;
      }
      const pacing::ControlResponse applied =
          client.apply_envelope(envelope.ref, pacing::BackendId::from(options.backend));
      pacing::u64 attempt_value = 0;
      if (applied.code == pacing::ErrorCode::Ok) {
        pacing::ByteReader attempt_reader(
            std::span<const pacing::u8>(applied.payload.data(), applied.payload.size()));
        attempt_value = attempt_reader.u64v();
      }
      std::printf("iteration %llu envelope=%llu attempt=%llu apply=%s\n",
                  static_cast<unsigned long long>(iteration),
                  static_cast<unsigned long long>(envelope.ref.id.value()),
                  static_cast<unsigned long long>(attempt_value),
                  std::string(pacing::to_string(applied.code)).c_str());
      std::fflush(stdout);
    }
    if (options.pause_ms != 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(options.pause_ms));
    }
  }

  client.close();
  pacing::transport_global_shutdown();
  return 0;
}
