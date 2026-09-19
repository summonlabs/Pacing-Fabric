// pacingd - Pacing Fabric coordinator daemon.
//
// Hosts one PacingFabric instance, one SYNTHETIC pacing backend and one locally
// configured SYNTHETIC rate authority, and serves the framed control protocol
// on a loopback port. It never drives a physical device and makes no packet
// rate claim.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pacing/pacing.hpp"

namespace {

struct Options {
  pacing::u16 port{0};
  std::string durable_directory{};
  std::string port_file{};
  std::string name{"pacingd"};
  pacing::u64 instance{1};
  pacing::u64 resource{1};
  pacing::u64 ceiling_bps{10'000'000'000ull};
  pacing::u64 floor_bps{0};
  pacing::u64 grant_generation{1};
  pacing::u64 backend_id{9};
  bool quiet{false};
};

void usage() {
  std::fprintf(stderr,
               "usage: pacingd [--port N] [--port-file PATH] [--durable DIR] [--instance ID]\n"
               "               [--resource ID] [--ceiling-bps N] [--floor-bps N]\n"
               "               [--grant-generation N] [--backend-id N] [--name NAME] [--quiet]\n");
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
    if (arg == "--port") {
      pacing::u64 value = 0;
      if (!parse_u64(next, value) || value > 65535) return false;
      options.port = static_cast<pacing::u16>(value);
      ++i;
    } else if (arg == "--port-file") {
      if (next == nullptr) return false;
      options.port_file = next;
      ++i;
    } else if (arg == "--durable") {
      if (next == nullptr) return false;
      options.durable_directory = next;
      ++i;
    } else if (arg == "--name") {
      if (next == nullptr) return false;
      options.name = std::string(next).substr(0, 64);
      ++i;
    } else if (arg == "--instance") {
      if (!parse_u64(next, options.instance)) return false;
      ++i;
    } else if (arg == "--resource") {
      if (!parse_u64(next, options.resource)) return false;
      ++i;
    } else if (arg == "--ceiling-bps") {
      if (!parse_u64(next, options.ceiling_bps)) return false;
      ++i;
    } else if (arg == "--floor-bps") {
      if (!parse_u64(next, options.floor_bps)) return false;
      ++i;
    } else if (arg == "--grant-generation") {
      if (!parse_u64(next, options.grant_generation)) return false;
      ++i;
    } else if (arg == "--backend-id") {
      if (!parse_u64(next, options.backend_id)) return false;
      ++i;
    } else if (arg == "--quiet") {
      options.quiet = true;
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options{};
  if (!parse_options(argc, argv, options)) {
    usage();
    return 2;
  }

  if (!pacing::transport_global_init().ok()) {
    std::fprintf(stderr, "pacingd: transport initialisation failed\n");
    return 3;
  }

  // The rate ceiling is an input, never an output: this process installs what
  // it was configured with and the fabric can only read it back.
  pacing::StaticRateAuthority authority;
  pacing::RateGrant grant{};
  grant.ref = pacing::GrantRef{pacing::GrantId::from(1), pacing::Generation::from(1)};
  grant.resource = pacing::ResourceId::from(options.resource);
  grant.ceiling_bps = options.ceiling_bps;
  grant.floor_bps = options.floor_bps;
  grant.revision = options.grant_generation;
  grant.authoritative = true;
  authority.install(grant);

  pacing::SteadyClock clock;
  pacing::FabricConfig config{};
  config.durable_directory = options.durable_directory;
  config.enable_durability = !options.durable_directory.empty();
  config.fabric_instance = options.instance;
  config.tick_period_ns = 1'000'000ull;
  config.envelope_lifetime_ns = 2'000'000'000ull;

  pacing::PacingFabric fabric(config, authority, clock);
  const pacing::Status initialized = fabric.initialize();
  if (!initialized.ok()) {
    std::fprintf(stderr, "pacingd: fabric initialisation failed: %s\n", initialized.to_string().c_str());
    return 4;
  }

  auto backend = std::make_shared<pacing::SyntheticPacingBackend>(
      pacing::BackendRef{pacing::BackendId::from(options.backend_id), pacing::Generation::from(1)},
      "synthetic-shaper");
  const pacing::Status registered = fabric.register_backend(backend);
  if (!registered.ok()) {
    std::fprintf(stderr, "pacingd: backend registration failed: %s\n", registered.to_string().c_str());
    return 5;
  }

  pacing::ControlServer::Config server_config{};
  server_config.port = options.port;
  server_config.name = options.name;
  server_config.max_connections = 64;
  pacing::ControlServer server(server_config, fabric);

  pacing::u16 bound_port = 0;
  const pacing::Status started = server.start(bound_port);
  if (!started.ok()) {
    std::fprintf(stderr, "pacingd: listen failed: %s\n", started.to_string().c_str());
    return 6;
  }

  if (!options.port_file.empty()) {
    std::FILE* file = std::fopen(options.port_file.c_str(), "wb");
    if (file == nullptr) {
      std::fprintf(stderr, "pacingd: cannot write port file\n");
      return 7;
    }
    std::fprintf(file, "%u\n", static_cast<unsigned>(bound_port));
    std::fclose(file);
  }

  if (!options.quiet) {
    std::printf("pacingd listening on port %u (epoch %llu, boot %llu)\n",
                static_cast<unsigned>(bound_port),
                static_cast<unsigned long long>(fabric.epoch().value()),
                static_cast<unsigned long long>(fabric.boot().counter));
    std::fflush(stdout);
  }

  const pacing::Status served = server.run();
  (void)fabric.shutdown();
  server.stop();
  pacing::transport_global_shutdown();
  if (!served.ok()) {
    std::fprintf(stderr, "pacingd: serve failed: %s\n", served.to_string().c_str());
    return 8;
  }
  return 0;
}
