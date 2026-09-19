// pacingctl - operator command line for a running Pacing Fabric coordinator.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pacing/pacing.hpp"

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: pacingctl --port N <command> [options]\n"
               "commands:\n"
               "  epoch\n"
               "  stats\n"
               "  backends\n"
               "  recover\n"
               "  advance-epoch\n"
               "  stop\n"
               "  bind-flow    --flow N --resource N [--resource-gen N] [--path N] [--path-gen N]\n"
               "               [--service-class N] [--burst-bytes N] [--burst-packets N]\n"
               "  publish-policy --policy N --resource N [--path N] --quantum N --window-ns N\n"
               "               [--rate-bps N] [--rate-share-ppm N] [--min-rate-bps N] [--burst-bytes N]\n"
               "  derive       --flow N\n"
               "  apply        --envelope N --backend N [--attempt N]\n"
               "  verify       --attempt N\n"
               "  cancel       --attempt N [--reason TEXT]\n"
               "  revoke       --envelope N [--reason TEXT]\n"
               "  revalidate   --attempt N\n"
               "  explain      --flow N\n");
}

bool parse_u64(const char* text, pacing::u64& out) {
  if (text == nullptr || *text == '\0') return false;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (end == nullptr || *end != '\0') return false;
  out = static_cast<pacing::u64>(value);
  return true;
}

int fail(const char* message) {
  std::fprintf(stderr, "pacingctl: %s\n", message);
  return 2;
}

void print_response(const pacing::ControlResponse& response, bool ok_expected) {
  if (response.code != pacing::ErrorCode::Ok) {
    std::printf("%s: %s\n", std::string(pacing::to_string(response.code)).c_str(),
                response.detail.c_str());
    return;
  }
  if (!response.payload.empty()) {
    std::printf("%s\n", std::string(reinterpret_cast<const char*>(response.payload.data()),
                                     response.payload.size())
                             .c_str());
  } else if (ok_expected) {
    std::printf("ok\n");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    usage();
    return 2;
  }

  pacing::u16 port = 0;
  std::string command;
  std::vector<std::pair<std::string, std::string>> args;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (arg == "--port") {
      pacing::u64 value = 0;
      if (!parse_u64(next, value) || value == 0 || value > 65535) return fail("bad --port");
      port = static_cast<pacing::u16>(value);
      ++i;
    } else if (arg.rfind("--", 0) == 0) {
      if (next == nullptr) return fail("option requires a value");
      args.emplace_back(arg.substr(2), next);
      ++i;
    } else if (command.empty()) {
      command = arg;
    } else {
      return fail("unexpected argument");
    }
  }
  if (command.empty() || port == 0) {
    usage();
    return 2;
  }

  auto value_of = [&args](const std::string& key, pacing::u64 fallback) {
    for (const auto& entry : args) {
      if (entry.first == key) {
        pacing::u64 parsed = 0;
        if (parse_u64(entry.second.c_str(), parsed)) return parsed;
      }
    }
    return fallback;
  };
  auto text_of = [&args](const std::string& key, const std::string& fallback) {
    for (const auto& entry : args) {
      if (entry.first == key) return entry.second;
    }
    return fallback;
  };
  auto has = [&args](const std::string& key) {
    for (const auto& entry : args) {
      if (entry.first == key) return true;
    }
    return false;
  };

  if (!pacing::transport_global_init().ok()) return fail("transport initialisation failed");
  pacing::ControlClient client;
  const pacing::Status connected = client.connect_loopback(port, 400, 25);
  if (!connected.ok()) {
    std::fprintf(stderr, "pacingctl: connect failed: %s\n", connected.to_string().c_str());
    return 3;
  }
  const pacing::Status handshake = client.handshake("pacingctl");
  if (!handshake.ok()) {
    std::fprintf(stderr, "pacingctl: handshake failed: %s\n", handshake.to_string().c_str());
    return 3;
  }

  const pacing::Limits limits{};
  pacing::ControlResponse response{};
  bool ok_expected = true;

  if (command == "epoch" || command == "stats" || command == "backends" || command == "recover" ||
      command == "stop") {
    if (command == "epoch") response = client.epoch();
    else if (command == "stats") response = client.stats();
    else if (command == "backends") response = client.backends();
    else if (command == "recover") response = client.recover();
    else response = client.stop();
    if (command == "epoch" && response.code == pacing::ErrorCode::Ok) {
      pacing::ByteReader reader(std::span<const pacing::u8>(response.payload.data(), response.payload.size()));
      pacing::Epoch epoch{};
      pacing::BootId boot{};
      bool running = false;
      if (pacing::decode_epoch_payload(reader, epoch, boot, running).ok()) {
        std::printf("epoch=%llu boot_counter=%llu boot_nonce=%llu running=%s\n",
                    static_cast<unsigned long long>(epoch.value()),
                    static_cast<unsigned long long>(boot.counter),
                    static_cast<unsigned long long>(boot.nonce), running ? "true" : "false");
        return 0;
      }
    }
    print_response(response, ok_expected);
    return response.code == pacing::ErrorCode::Ok ? 0 : 1;
  }

  if (command == "advance-epoch") {
    response = client.advance_epoch();
    print_response(response, true);
    return response.code == pacing::ErrorCode::Ok ? 0 : 1;
  }

  if (command == "bind-flow") {
    pacing::FlowBinding binding{};
    binding.flow = pacing::FlowId::from(value_of("flow", 0));
    binding.resource = pacing::ResourceRef{pacing::ResourceId::from(value_of("resource", 0)),
                                           pacing::Generation::from(value_of("resource-gen", 1))};
    binding.path = pacing::PathRef{pacing::PathId::from(value_of("path", 1)),
                                   pacing::Generation::from(value_of("path-gen", 1))};
    binding.service_class = pacing::ServiceClassId::from(value_of("service-class", 0));
    binding.declared_max_burst_bytes = value_of("burst-bytes", 0);
    binding.declared_max_burst_packets = value_of("burst-packets", 0);
    response = client.bind_flow(binding);
    print_response(response, true);
    return response.code == pacing::ErrorCode::Ok ? 0 : 1;
  }

  if (command == "publish-policy") {
    pacing::PacingPolicy policy{};
    policy.ref.id = pacing::PolicyId::from(value_of("policy", 0));
    policy.ref.generation = pacing::Generation::unknown();
    policy.layer = pacing::PacingLayer::Resource;
    policy.resource = pacing::ResourceId::from(value_of("resource", 0));
    policy.path = pacing::PathRef{pacing::PathId::from(value_of("path", 1)),
                                  pacing::Generation::from(value_of("path-gen", 1))};
    policy.rate_bps = value_of("rate-bps", 0);
    policy.rate_share_ppm = value_of("rate-share-ppm", 1'000'000ull);
    policy.min_rate_bps = value_of("min-rate-bps", 0);
    policy.burst_bytes = value_of("burst-bytes", 0);
    policy.burst_packets = value_of("burst-packets", 0);
    policy.quantum_bytes = value_of("quantum", 1500);
    policy.window_ns = value_of("window-ns", 1'000'000ull);
    if (has("shape")) {
      pacing::CadenceShape shape{};
      if (!pacing::parse_cadence_shape(text_of("shape", "uniform"), shape)) return fail("bad --shape");
      policy.shape = shape;
    }
    response = client.publish_policy(policy);
    print_response(response, true);
    return response.code == pacing::ErrorCode::Ok ? 0 : 1;
  }

  if (command == "derive") {
    response = client.derive_envelope(pacing::FlowId::from(value_of("flow", 0)));
    if (response.code != pacing::ErrorCode::Ok) {
      print_response(response, false);
      return 1;
    }
    pacing::ByteReader reader(std::span<const pacing::u8>(response.payload.data(), response.payload.size()));
    pacing::PacingEnvelope envelope{};
    if (!pacing::decode_envelope_payload(reader, limits, envelope).ok()) return fail("bad envelope payload");
    std::printf("%s\n", envelope.to_string().c_str());
    std::printf("authority %s\n", pacing::to_string(envelope.authority).c_str());
    return 0;
  }

  if (command == "apply") {
    response = client.apply_envelope(
        pacing::EnvelopeRef{pacing::EnvelopeId::from(value_of("envelope", 0)), pacing::Generation::from(1)},
        pacing::BackendId::from(value_of("backend", 0)),
        pacing::AttemptId::from(value_of("attempt", 0)));
    print_response(response, true);
    return response.code == pacing::ErrorCode::Ok ? 0 : 1;
  }

  if (command == "verify" || command == "revalidate" || command == "cancel") {
    const pacing::AttemptId attempt = pacing::AttemptId::from(value_of("attempt", 0));
    if (command == "verify") response = client.verify_attempt(attempt);
    else if (command == "revalidate") response = client.revalidate(attempt);
    else response = client.cancel_attempt(attempt, text_of("reason", "operator cancellation"));
    if (response.code == pacing::ErrorCode::Ok && !response.payload.empty()) {
      pacing::ByteReader reader(std::span<const pacing::u8>(response.payload.data(), response.payload.size()));
      pacing::ApplicationRecord record{};
      if (pacing::decode_attempt_payload(reader, limits, record).ok()) {
        std::printf("attempt=%llu state=%s effect=%s reason=%s\n",
                    static_cast<unsigned long long>(record.attempt.value()),
                    std::string(pacing::to_string(record.state)).c_str(),
                    std::string(pacing::to_string(record.effect)).c_str(),
                    std::string(pacing::to_string(record.reason)).c_str());
        return 0;
      }
    }
    print_response(response, true);
    return response.code == pacing::ErrorCode::Ok ? 0 : 1;
  }

  if (command == "revoke") {
    response = client.revoke_envelope(
        pacing::EnvelopeRef{pacing::EnvelopeId::from(value_of("envelope", 0)), pacing::Generation::from(1)},
        text_of("reason", "operator revocation"));
    print_response(response, true);
    return response.code == pacing::ErrorCode::Ok ? 0 : 1;
  }

  if (command == "explain") {
    response = client.explain(pacing::FlowId::from(value_of("flow", 0)));
    if (response.code != pacing::ErrorCode::Ok) {
      print_response(response, false);
      return 1;
    }
    pacing::ByteReader reader(std::span<const pacing::u8>(response.payload.data(), response.payload.size()));
    const std::string json = reader.str(limits.max_explanation_bytes);
    if (!reader.ok()) return fail("bad explain payload");
    std::printf("%s\n", json.c_str());
    return 0;
  }

  usage();
  return 2;
}
