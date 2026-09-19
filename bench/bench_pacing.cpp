// Pacing Fabric benchmark - SYNTHETIC control-plane only.
//
// This measures completed control-plane work: envelope derivation, durable
// application bookkeeping and readback verification, all against an in-process
// SYNTHETIC backend. It is not a packet-rate, throughput or latency claim, and
// nothing here exercises a physical pacing mechanism. Enqueue or submission
// latency is deliberately not reported; only completed work is counted.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "pacing/pacing.hpp"

namespace {

struct Options {
  pacing::u64 flows{1000};
  pacing::u64 iterations{2};
  pacing::u64 threads{4};
  bool durable{false};
  bool quiet{false};
};

bool parse_u64(const char* text, pacing::u64& out) {
  if (text == nullptr || *text == '\0') return false;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (end == nullptr || *end != '\0') return false;
  out = static_cast<pacing::u64>(value);
  return true;
}

struct Totals {
  pacing::u64 derived{0};
  pacing::u64 applied{0};
  pacing::u64 verified{0};
  pacing::u64 refused{0};
  pacing::u64 nanos{0};
};

}  // namespace

int main(int argc, char** argv) {
  Options options{};
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (arg == "--flows") {
      if (!parse_u64(next, options.flows)) return 2;
      ++i;
    } else if (arg == "--iterations") {
      if (!parse_u64(next, options.iterations)) return 2;
      ++i;
    } else if (arg == "--threads") {
      if (!parse_u64(next, options.threads)) return 2;
      ++i;
    } else if (arg == "--durable") {
      options.durable = true;
    } else if (arg == "--quiet") {
      options.quiet = true;
    } else {
      std::fprintf(stderr, "usage: bench_pacing [--flows N] [--iterations N] [--threads N] [--durable]\n");
      return 2;
    }
  }
  if (options.flows == 0 || options.iterations == 0 || options.threads == 0) return 2;

  const std::string workspace =
      (std::filesystem::temp_directory_path() / "pacing_fabric_bench").string();
  if (options.durable) {
    std::error_code ec;
    std::filesystem::remove_all(workspace, ec);
    std::filesystem::create_directories(workspace, ec);
  }

  pacing::StaticRateAuthority authority;
  pacing::RateGrant grant{};
  grant.ref = pacing::GrantRef{pacing::GrantId::from(1), pacing::Generation::from(1)};
  grant.resource = pacing::ResourceId::from(1);
  grant.ceiling_bps = 40'000'000'000ull;
  grant.authoritative = true;
  authority.install(grant);

  pacing::SteadyClock clock;
  pacing::FabricConfig config{};
  config.fabric_instance = 1;
  config.enable_durability = options.durable;
  config.durable_directory = options.durable ? workspace : std::string{};
  // The lifetime must stay inside the configured bound; the benchmark derives
  // and applies immediately, so a short lifetime is sufficient.
  config.envelope_lifetime_ns = 4'000'000'000ull;

  pacing::PacingFabric fabric(config, authority, clock);
  const pacing::Status initialized = fabric.initialize();
  if (!initialized.ok()) {
    std::fprintf(stderr, "bench: fabric initialisation failed: %s\n",
                 initialized.to_string().c_str());
    return 3;
  }

  auto backend = std::make_shared<pacing::SyntheticPacingBackend>(
      pacing::BackendRef{pacing::BackendId::from(9), pacing::Generation::from(1)}, "bench-synthetic");
  if (!fabric.register_backend(backend).ok()) {
    std::fprintf(stderr, "bench: backend registration failed\n");
    return 3;
  }

  pacing::PacingPolicy policy{};
  policy.ref.id = pacing::PolicyId::from(1);
  policy.ref.generation = pacing::Generation::unknown();
  policy.layer = pacing::PacingLayer::Resource;
  policy.resource = pacing::ResourceId::from(1);
  policy.path = pacing::PathRef{pacing::PathId::from(1), pacing::Generation::from(1)};
  policy.rate_share_ppm = 500'000;
  policy.quantum_bytes = 1500;
  policy.window_ns = 1'000'000;
  if (!fabric.publish_policy(policy).ok()) {
    std::fprintf(stderr, "bench: policy publication failed\n");
    return 3;
  }

  std::vector<pacing::FlowId> flows;
  flows.reserve(options.flows);
  for (pacing::u64 i = 0; i < options.flows; ++i) {
    pacing::FlowBinding binding{};
    binding.flow = pacing::FlowId::from(i + 1);
    binding.resource = pacing::ResourceRef{pacing::ResourceId::from(1), pacing::Generation::from(1)};
    binding.path = pacing::PathRef{pacing::PathId::from(1), pacing::Generation::from(1)};
    if (!fabric.bind_flow(binding).ok()) {
      std::fprintf(stderr, "bench: flow binding failed at %llu\n",
                   static_cast<unsigned long long>(i));
      return 3;
    }
    flows.push_back(binding.flow);
  }

  const pacing::u64 worker_count = options.threads;
  std::vector<Totals> per_worker(worker_count);
  std::vector<std::jthread> workers;
  const auto started = std::chrono::steady_clock::now();
  for (pacing::u64 worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&, worker] {
      Totals totals{};
      for (pacing::u64 iteration = 0; iteration < options.iterations; ++iteration) {
        for (pacing::u64 index = worker; index < flows.size(); index += worker_count) {
          auto envelope = fabric.derive_envelope(flows[index]);
          if (!envelope) {
            ++totals.refused;
            continue;
          }
          ++totals.derived;
          auto attempt = fabric.apply_envelope(envelope.value().ref,
                                               pacing::BackendId::from(9));
          if (!attempt) {
            ++totals.refused;
            continue;
          }
          ++totals.applied;
          // Count only work that reached a terminal, evidence-backed outcome.
          auto record = fabric.attempt(attempt.value());
          if (record && record.value().state == pacing::ApplicationState::Applied) ++totals.verified;
        }
      }
      per_worker[worker] = totals;
    });
  }
  workers.clear();
  const auto finished = std::chrono::steady_clock::now();
  const auto elapsed_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();

  Totals total{};
  for (const auto& worker : per_worker) {
    total.derived += worker.derived;
    total.applied += worker.applied;
    total.verified += worker.verified;
    total.refused += worker.refused;
  }
  total.nanos = static_cast<pacing::u64>(elapsed_ns);

  const double seconds = static_cast<double>(total.nanos) / 1e9;
  const auto rate = [seconds](pacing::u64 count) {
    return seconds > 0.0 ? static_cast<double>(count) / seconds : 0.0;
  };

  std::printf("Pacing Fabric benchmark (SYNTHETIC control plane only)\n");
  std::printf("  scope              : envelope derivation, application bookkeeping, readback verification\n");
  std::printf("  physical validation: NONE - no packet-rate or device claim is made\n");
  std::printf("  durability         : %s\n", options.durable ? "enabled" : "disabled");
  std::printf("  flows              : %llu\n", static_cast<unsigned long long>(options.flows));
  std::printf("  iterations         : %llu\n", static_cast<unsigned long long>(options.iterations));
  std::printf("  worker threads     : %llu\n", static_cast<unsigned long long>(worker_count));
  std::printf("  elapsed            : %.4f s\n", seconds);
  std::printf("  envelopes derived  : %llu (%.0f/s)\n",
              static_cast<unsigned long long>(total.derived), rate(total.derived));
  std::printf("  attempts applied   : %llu (%.0f/s)\n",
              static_cast<unsigned long long>(total.applied), rate(total.applied));
  std::printf("  effects verified   : %llu (%.0f/s)\n",
              static_cast<unsigned long long>(total.verified), rate(total.verified));
  std::printf("  refusals           : %llu\n", static_cast<unsigned long long>(total.refused));
  const pacing::FabricStats stats = fabric.stats();
  std::printf("  duplicate applies  : %llu\n", static_cast<unsigned long long>(stats.duplicate_applies));
  std::printf("  journal appends    : %llu\n", static_cast<unsigned long long>(stats.journal_appends));
  std::printf("  checkpoints        : %llu\n", static_cast<unsigned long long>(stats.checkpoints));
  std::printf("SYNTHETIC: these figures describe control-plane bookkeeping in this process.\n");
  if (!options.quiet) std::fflush(stdout);

  if (!fabric.shutdown().ok()) {
    std::fprintf(stderr, "bench: shutdown failed\n");
    return 4;
  }
  if (total.applied == 0 || total.verified != total.applied) {
    std::fprintf(stderr, "bench: incomplete work (applied=%llu verified=%llu)\n",
                 static_cast<unsigned long long>(total.applied),
                 static_cast<unsigned long long>(total.verified));
    return 5;
  }
  return 0;
}
