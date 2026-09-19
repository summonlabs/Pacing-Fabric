# Pacing Fabric

Open-source, vendor-neutral C++20 runtime for generation-bound packet and flow pacing
policy, pacing envelopes, burst smoothing, lifecycle, authority, and verified application
state.

Pacing Fabric answers one question and refuses to answer any other:

> Given an authoritative flow/resource binding, pacing policy, service obligations, current
> rate/grant state, burst constraints, and exact generations, **what pacing envelope is
> authorized now, what cadence is intended, what effect is actually applied, and when must
> pacing be changed, revoked, fenced, or revalidated?**

## Boundary

Pacing Fabric owns **pacing-policy authority** and **pacing application-state governance**.

It does not own bandwidth arbitration, generic rate entitlement, traffic admission, path
placement, packet queue scheduling, congestion-state synthesis, backpressure, or physical
device implementation. Those enter only through two narrow interfaces:

* `IRateAuthority` - read-only upstream rate/grant evidence. The fabric observes an
  authorized ceiling and can never produce, extend, repair, or write one back.
* `IPacingBackend` - the only thing that can turn intent into an effect, with an explicit
  readback path and an explicit REAL/SYNTHETIC label.

## Separations the implementation enforces

| Separation | How it is enforced |
| --- | --- |
| Rate authority is not pacing authority | `RateGrant` is read-only input; the fabric has no API that produces a ceiling. A missing, expired, or non-authoritative grant yields UNKNOWN, never a synthesized rate. |
| Pacing intent is not application | `PacingEnvelope` is derived intent bound to an authority vector. It asserts nothing about what is in force. |
| Backend acknowledgement is not applied effect | An accepted `apply` moves the record to ACKNOWLEDGED only, with `EffectLabel::NONE`. APPLIED requires a readback that reproduces the authorized cadence exactly. Without readback the record stops at APPLIED_UNVERIFIED. |
| Pacing never creates bandwidth entitlement | A policy floor above the upstream ceiling is refused with `EntitlementMint` rather than clamped; a request above the ceiling is clamped down and flagged. |
| Stale generations invalidate pacing | Every envelope, attempt, and durable record carries the exact `AuthorityVector` that justified it. Any drift in flow, resource, policy, rate-grant, path, epoch, or coordinator boot invalidates it. |
| Cancelled or revoked work cannot commit | Cancellation is terminal; a backend completion that arrives afterwards is discarded and a compensating withdrawal is issued. |
| Duplicate application is idempotent | Attempts are keyed by (envelope, backend) and by explicit attempt id; a repeat returns the existing attempt and performs no second backend mutation. |
| Burst accounting is exact and bounded | All rate, quantum, interval, window, and burst arithmetic is integer-only, with 128-bit checked multiply and divide. Burst above the configured bound is rejected, never silently clamped. |

## Model

Strong identities are distinct types: `FlowId`, `ResourceId`, `PolicyId`, `EnvelopeId`,
`BackendId`, `AttemptId`, `GrantId`, `PathId`, `ProvenanceId`. Mixing identity domains
does not compile. Zero is never a valid identity.

Every reference is a `GenRef<Tag>` - an identity plus a monotonic `Generation`. Generation 0
means UNKNOWN and authorizes nothing. `Epoch` advances whenever a coordinator incarnation takes
ownership of durable state. `BootId` is a process incarnation that is never restored from disk
as current.

The exact evidence set that justified a decision is an `AuthorityVector`:

    flow@gen  resource@gen  policy@gen  rate_grant@gen  path@gen  epoch  coordinator_boot(nonce)

Two vectors compare field by field, and a mismatch is reported as a specific
`AuthorityDrift` (Flow, Resource, Policy, RateGrant, Path, Epoch, CoordinatorBoot,
Incomplete) rather than a generic staleness flag.

## Lifecycle

    UNKNOWN -> PLANNED -> RESERVED -> SUBMITTED -> ACKNOWLEDGED -> APPLIED
                                                                -> APPLIED_UNVERIFIED
                                                                -> MISMATCHED
    terminal without effect: CANCELLED, REVOKED, FENCED, STALE, FAILED, AMBIGUOUS
    REQUIRES_REVALIDATION: durable history that is no longer current fact

The effect label is a pure function of the state: only APPLIED may claim VERIFIED (REAL
backend readback) or SYNTHETIC_VERIFIED (SYNTHETIC backend readback); APPLIED_UNVERIFIED
claims UNVERIFIED; MISMATCHED claims MISMATCHED; every other state claims nothing.

## Repository layout

    include/pacing/       public headers (the whole installed surface)
    src/                  implementation
    tools/                pacingd (coordinator daemon), pacingctl (operator CLI), pacing-agent (worker)
    tests/                unit, property, seeded randomized, adversarial, concurrency, durability
    tests/multiproc/      real-OS-process integration tests
    bench/                synthetic control-plane benchmark
    examples/downstream/  independent find_package consumer
    docs/                 architecture, semantics and validation notes

## Building

Requires CMake 3.25 or newer and a C++20 compiler. MSVC is built with /W4 /WX /permissive-.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

Options: `PACING_FABRIC_BUILD_TESTS`, `PACING_FABRIC_BUILD_TOOLS`,
`PACING_FABRIC_BUILD_BENCH`, `PACING_FABRIC_WARNINGS_AS_ERRORS`,
`PACING_FABRIC_ENABLE_ASAN`.

## Installing and consuming

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/path/to/prefix
    cmake --build build
    cmake --install build

A downstream project then uses the exported package only:

    find_package(PacingFabric 1.0 CONFIG REQUIRED)
    target_link_libraries(app PRIVATE PacingFabric::pacing_fabric)

`examples/downstream` is exactly such a project; it derives an envelope, applies it to a
synthetic backend, and asserts the verified state without touching the Pacing Fabric source
tree.

## Minimal use

    #include <pacing/pacing.hpp>

    LocalRateAuthority authority(10'000'000'000ull);   // your arbiter adapter
    pacing::SteadyClock clock;
    pacing::FabricConfig config;
    pacing::PacingFabric fabric(config, authority, clock);
    fabric.initialize();

    auto backend = std::make_shared<pacing::SyntheticPacingBackend>(ref, "synthetic-shaper");
    fabric.register_backend(backend);
    fabric.bind_flow(binding);
    fabric.publish_policy(policy);

    auto envelope = fabric.derive_envelope(flow);       // authorized intent + exact authority
    auto attempt  = fabric.apply_envelope(envelope.value().ref, backend_id);
    auto record   = fabric.attempt(attempt.value());    // APPLIED only with matching evidence
    auto why      = fabric.explain(flow);               // bounded, deterministic explanation

## Control plane

`pacingd` hosts one fabric instance, one SYNTHETIC backend and one locally configured
SYNTHETIC rate authority, and serves the framed protocol on a loopback port.

    pacingd --port 0 --port-file port.txt --durable /var/lib/pacingd --resource 1 --ceiling-bps 10000000000 --backend-id 9

    pacingctl --port 9411 bind-flow --flow 100 --resource 1
    pacingctl --port 9411 publish-policy --policy 1 --resource 1 --quantum 1500 --window-ns 1000000
    pacingctl --port 9411 derive --flow 100
    pacingctl --port 9411 apply --envelope 1 --backend 9
    pacingctl --port 9411 verify --attempt 1
    pacingctl --port 9411 explain --flow 100
    pacingctl --port 9411 stats
    pacingctl --port 9411 stop

Frames are magic | version | type | request_id | payload_len | crc32c, with the checksum
covering the header and the payload. Every request carries the caller's view of the
coordinator epoch and boot incarnation; a superseded stamp is refused with StaleEpoch or
StaleBoot and is never reinterpreted against current authority. A framing failure poisons
the connection: a byte stream whose framing is no longer trustworthy is not reinterpreted.

## Durability and recovery

State is a versioned snapshot plus an append-only journal, both CRC-32C protected per record
and per body. A record is never acknowledged before it is on stable storage. Recovery
replays a valid prefix, reports a torn tail explicitly, and refuses to start on a corrupt
snapshot rather than starting empty.

On restart the coordinator advances its epoch and boot incarnation, and then:

* every persisted envelope is invalidated;
* every in-flight attempt becomes AMBIGUOUS - its backend outcome was never observed;
* every applied or acknowledged attempt becomes REQUIRES_REVALIDATION and asserts no effect;
* no backend handle, liveness, or effect is restored: the operator must register a live
  backend, and re-derive and re-apply under the new epoch.

## REAL vs SYNTHETIC vs UNSUPPORTED

* **SYNTHETIC** - everything in this repository. `SyntheticPacingBackend` and
  `StaticRateAuthority` perform control-plane bookkeeping in-process. Every envelope and
  every explanation carries the synthetic flags. The benchmark measures control-plane work
  only and makes no packet-rate claim.
* **REAL** - a vendor backend that drives an actual pacing mechanism and returns readback
  evidence from it. The interface, the label propagation, and the strict
  acknowledgement-versus-effect split are implemented and tested for label fidelity, but no
  physical integration ships here.
* **UNSUPPORTED** - physical packet pacing, NIC/DPU/switch integration, RDMA, NVLink, optical
  transport, and multi-node or multi-switch validation. None of this is claimed, tested, or
  fabricated.

## Testing

Ten suites and 150 cases run under CTest: checked arithmetic and 128-bit division identities,
model-level derivation properties, a hardening suite that attacks authority mid-apply,
end-to-end lifecycle, governance (cancel, revoke, fence, revalidate), durability and restart,
concurrency and races, adversarial input and framing, seeded randomized walks and fuzzing,
and a multiprocess suite that starts a real `pacingd`, real `pacing-agent` processes and
a real framed TCP transport, then hard-kills them.

The multiprocess suite proves hard process kill and restart, coordinator epoch advancement,
stale epoch and boot rejection, worker-death isolation, and that malformed or corrupt frames
do not take the coordinator down.

## Benchmark

`bench_pacing` reports completed control-plane work - envelopes derived, attempts applied,
effects verified - against an in-process SYNTHETIC backend. It deliberately does not report
enqueue or submission latency, and it is not a packet-rate measurement.

## Documentation

* `docs/architecture.md` - components, interfaces, and the runtime boundary
* `docs/semantics.md` - authority, generations, lifecycle, durability and recovery rules
* `docs/validation.md` - what was validated, how, and what remains unproven

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
