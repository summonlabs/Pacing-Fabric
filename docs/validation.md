# Validation

Everything below was executed against this repository at version 1.0.0. Ten CTest suites,
150 test cases.

## What was run

| Suite | Cases | Coverage |
| --- | --- | --- |
| `test_checked` | 9 | checked arithmetic, 128-bit product and divide identities over 20 000 seeded cases, saturation, narrowing, digests |
| `test_model` | 21 | cadence derivation properties over 20 000 seeded inputs, ceiling and burst bounds, authority vectors and drift classification, policy and binding stores |
| `test_hardening` | 15 | adversarial backend behaviour, authority drift during apply, backend unregistration during apply, ledger and envelope-table exhaustion, bounded journal growth, clock regression, near-maximum instants, huge bursts, many flows, shutdown racing cancellation |
| `test_lifecycle` | 30 | derivation, application, verification, idempotence, expiry, drift invalidation, refusals, service-class exceptions, label propagation |
| `test_governance` | 21 | cancellation, revocation, fencing, epoch advance, revalidation, bounded explanation rendering, re-entrant event sinks, shutdown draining |
| `test_persistence` | 19 | store round-trip, checkpointing, sequence barrier, corrupt snapshot, corrupt journal, torn tail, oversized records, restart semantics, compaction pressure |
| `test_concurrency` | 9 | duplicate apply from eight threads, concurrent derivation, cancellation racing apply, revocation racing apply, epoch advance racing apply, rebinding racing derivation, observation under load, re-entrant sink under load, pending-attempt bound |
| `test_adversarial` | 18 | every truncated prefix of four encodings, trailing bytes, oversized length prefixes, out-of-range discriminators, poisoned writers, forged ceilings, incomplete authority, incoherent limits, malformed configuration and API input, frame round-trip, corrupt magic, corrupt checksum, bad version, bad type, oversized and truncated frames, closed connections |
| `test_randomized` | 3 | six seeded 220-step operation walks with full invariant checks, 20 000 random-byte decode attempts across seven decoders, 40 000 hostile derivation inputs |
| `test_multiproc` | 5 | a real `pacingd` process, real `pacing-agent` processes, and a real framed TCP transport: multiple workers on one coordinator, hard kill and restart with epoch fencing, worker-process death isolation, malformed and corrupt frames, epoch advancement fencing a live worker |

## Commands

    cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/dev
    ctest --test-dir build/dev --output-on-failure

    cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPACING_FABRIC_ENABLE_ASAN=ON
    cmake --build build/asan
    ctest --test-dir build/asan --output-on-failure

## Results

* Release, MSVC 19.44, `/W4 /WX /permissive-`: 10 of 10 suites pass, 150 of 150 cases.
* Debug with `/fsanitize=address`: 10 of 10 suites pass, 150 of 150 cases, no sanitizer
  report.
* Repeat stability: 20 consecutive full-suite runs with zero failures.
* Static analysis, `/analyze /analyze:external:W0`: zero first-party warnings. The only
  remaining diagnostics originate in the Windows SDK header `ws2tcpip.h`.

## Defects found and fixed during hardening

Every item below was found by a test or by the lock and durability review, and every one has
a regression test.

1. **Backend failure and rejection ran a readback.** A failed or rejected apply moved the
   record to FAILED and then asked the backend to read back pacing it never installed,
   reporting a misleading CANCELLED and counting a late completion. Fixed by ending the
   apply lifecycle at the recorded outcome.
2. **Epoch and fence operations left applied effects asserted.** Advancing the epoch or
   fencing below an epoch invalidated envelopes and in-flight attempts but left APPLIED
   records claiming an effect, and did not withdraw anything from a backend. Both now fence
   every record stamped with an older epoch and issue a compensating withdrawal.
3. **Cancelling an in-flight attempt issued no withdrawal.** A backend may install pacing
   before the coordinator observes the completion. Cancellation now always issues a
   compensating withdrawal, which the backend contract requires to be idempotent.
4. **Checkpoint sequence numbers restarted at one.** Compacted records began at sequence 1
   while the snapshot barrier recorded a different value, so an interruption between the
   snapshot rename and the journal truncation could replay superseded records over newer
   state and lose recent work. Compaction now continues the same monotonic sequence.
5. **The truncated journal had a zero format version.** The journal rewrite during
   checkpointing wrote a magic number but no version, making the store unreadable after a
   checkpoint. Fixed and covered by a reopen test.
6. **A coordinator could never be stopped.** The accept loop waited for a listener that
   nobody closed, so a Stop request hung the daemon and the multiprocess test. A Stop request
   now triggers the server's own shutdown.
7. **No authority re-check between backend apply and effect assertion.** A rate grant,
   policy, binding or path that moved while the backend was working still committed an
   effect. Authority is now re-checked immediately before an effect is asserted, and the
   completion is fenced and withdrawn when it has moved.
8. **An effect could commit on an unregistered backend.** Unregistering a backend mid-apply
   left an effect installed on a backend the coordinator no longer governed, so it could
   never be revoked, fenced, or rebound. Such a completion is now discarded and withdrawn.
9. **A backwards clock could resurrect an expired envelope.** Envelope expiry is computed
   from the injected clock, so a clock that moved backwards could re-authorize lapsed
   authority. The coordinator now remembers the highest instant it has observed and refuses
   all further authorization with CLOCK_REGRESSION once time stops being trustworthy.
10. **Recovery counts were inflated by replay.** Restored binding and policy counts counted
    replay records rather than live entities, so a journal that mentioned the same binding
    twice reported two. The report now describes live state.
11. **Atomic snapshot replacement could fail transiently.** A bounded retry now covers a
    transient sharing violation on the destination file.
12. **Static analysis finding.** An array bound in the CRC-32C table construction was not
    provable; the loops were restructured so the bound is provable by construction.

## Lock and re-entrancy review

Reviewed by inspection, not only by tests:

* no read-lock to write-lock re-entry exists, because the coordinator uses a single
  non-recursive mutex and never upgrades it;
* no write lock is held across a callback: backend, rate authority, clock, and event sink
  calls all happen with the state lock released;
* no mutex is re-entered through a callback, and a re-entrant event sink that calls back into
  the fabric is covered by dedicated tests in the governance and concurrency suites;
* events are emitted only after the state lock is released;
* no worker is joined while holding state that worker needs: server connection threads are
  detached and drained through a condition variable that releases the lock while waiting;
* shutdown sets a flag, closes the listener and every live connection, then waits for the
  active-operation count to reach zero, and only then closes durable state;
* lock order is uniform: fabric state lock, then durable store lock, and the durable store
  never calls back into the fabric;
* no progress callback re-enters mutable state; there are no progress callbacks.

## Persistence and restart

Proven by test: a real daemon is hard-killed with `TerminateProcess` mid-life, restarted
against the same durable directory, and the following are asserted - the epoch advanced, the
boot incarnation changed, a request stamped with the pre-restart epoch is fenced, every
persisted envelope is invalidated, every applied effect now reports REQUIRES_REVALIDATION,
no backend is registered after restart, and a fresh derive-and-apply under the new epoch
succeeds.

## Install and downstream consumer

The package is installed to a prefix and consumed by `examples/downstream`, a separate
CMake project that resolves it with `find_package(PacingFabric 1.0 CONFIG REQUIRED)` and
links only `PacingFabric::pacing_fabric`. The consumer derives an envelope, applies it,
verifies the SYNTHETIC_VERIFIED effect, and renders a bounded explanation.

## Benchmark

`bench_pacing` reports completed control-plane work against an in-process SYNTHETIC
backend. Representative Release figures on the development machine, four worker threads,
2 000 flows, three iterations: 6 000 envelopes derived, 6 000 attempts applied, 6 000 effects
verified in 0.0495 s, with zero refusals and zero duplicate applications.

These are control-plane bookkeeping figures for a synthetic population. They are not a
packet-rate, throughput, or latency claim, and no physical mechanism was exercised.

## REAL, SYNTHETIC, UNSUPPORTED

* SYNTHETIC: every backend, every rate authority, and every benchmark figure in this
  repository. Envelopes and explanations carry the synthetic flags so a synthetic result can
  never be presented as a real one.
* REAL: the interface, the label propagation, and the strict acknowledgement-versus-effect
  split are implemented and tested for label fidelity only. No physical pacing integration
  ships here, so no REAL effect is claimed.
* UNSUPPORTED: physical packet pacing, NIC, DPU, switch, RDMA, NVLink, or optical validation,
  and any multi-node or multi-switch claim. Nothing of the sort was fabricated or tested.
  ThreadSanitizer is unavailable on this toolchain, so no race-detector claim is made.

## Known limitations

* The bundled backends are software models. A real deployment must supply an
  `IPacingBackend` that drives an actual mechanism and can read it back.
* Rate authority is whatever the injected `IRateAuthority` reports. The fabric does not
  contact an arbiter itself, and it has no fallback when one is unreachable.
* Race freedom rests on the lock discipline, the dedicated race tests, and the multiprocess
  tests rather than on a race detector.
* The control transport is loopback-only in the bundled tools. It is a real framed stream
  protocol, but no authenticated or encrypted transport ships here.
* Recovery intentionally invalidates everything that was in force. Restoring pacing after a
  restart is an explicit re-derive and re-apply, by design.
