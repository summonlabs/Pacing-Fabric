# Architecture

## Runtime boundary

Pacing Fabric is a control-plane runtime. It owns:

* pacing-policy authority - which cadence is authorized, for which exact evidence;
* pacing application-state governance - what was intended, acknowledged, verified, and
  withdrawn, and what is no longer current.

It deliberately does not own bandwidth arbitration, rate entitlement, traffic admission,
path placement, packet queue scheduling, congestion-state synthesis, backpressure, or
device drivers. Two interfaces are the entire seam between the fabric and everything else:

    IRateAuthority   read-only upstream rate/grant evidence
    IPacingBackend   the only path from intent to effect, with readback

## Components

    pacing::PacingFabric        the coordinator: bindings, policies, envelopes, ledger,
                                governance, durability, explanation
    pacing::BindingRegistry     generation-bound flow/resource/path bindings
    pacing::PolicyStore         generation-bound pacing policies with service-class exceptions
    pacing::derive_cadence      pure derivation: policy + grant + declared bounds -> Cadence
    pacing::DurableStore        versioned snapshot + append-only CRC-32C journal
    pacing::SyntheticPacingBackend  bundled SYNTHETIC shaper model
    pacing::StaticRateAuthority     bundled SYNTHETIC (locally configured) arbiter adapter
    pacing::ControlServer/Client    framed loopback control protocol
    pacing::FramedConnection/Listener  real stream-socket transport with fail-closed framing

The coordinator is the only stateful component. Everything it decides is a function of its
inputs plus the injected clock, which makes derivation deterministic and replayable.

## Interfaces

### IRateAuthority

    StatusOr<RateGrant> fetch(ResourceId) const;
    const char* name() const noexcept;
    bool synthetic() const noexcept;

A grant carries identity, generation, an authorized ceiling, an authorized floor, an
upstream revision, an observation instant, an optional expiry, provenance, and an
`authoritative` flag. A grant that is absent, expired, or not authoritative is treated as
UNKNOWN: derivation is refused rather than approximated. The fabric never writes a grant.

### IPacingBackend

    BackendDescriptor describe() const;
    Status apply(const ApplyRequest&, ApplyAck&);
    Status readback(const ApplyRequest&, ReadbackEvidence&);
    Status revoke(const ApplyRequest&, std::string_view reason);
    Status rebind_epoch(Epoch, u64& discarded);
    BackendStats stats() const;

An accepted acknowledgement means the backend recorded the request. It does not mean an
effect exists. Only `readback` produces evidence, and evidence is compared against the
authorized cadence field by field. A backend that cannot read back reports
`ReadbackUnavailable`, and the record stops at APPLIED_UNVERIFIED.

`rebind_epoch` is destruction of stale authority, never resurrection of liveness.

## Concurrency

* Every public coordinator method is thread-safe.
* No external code - backend, rate authority, clock, event sink - is ever invoked while an
  internal lock is held. Backend and authority calls happen outside the state lock, and
  their results are folded back in with a re-validation step that discards late, stale, or
  cancelled work.
* Events are emitted after the state lock is released, so a sink may call back into the
  fabric. This is exercised directly by a re-entrant sink test.
* Lock order is fabric state lock then durable store lock. The durable store never calls
  back into the fabric.
* Shutdown stops accepting work, then waits for in-flight coordinator operations to drain
  while releasing the lock, so an operation between its backend call and its commit can
  still finish. Only afterwards is durable state closed.

## Control transport

    frame header: magic | version | type | request_id | payload_len | crc32c
    request body: operation | caller stamp (epoch, boot, worker) | fields
    reply body:   error code | bounded detail | fields

The checksum covers the header fields and the payload. Payload length is validated against
the configured bound before any allocation. A framing failure - bad magic, bad version, bad
type, bad checksum, truncated body, oversized body - poisons the connection permanently:
a stream whose framing is no longer trustworthy is never reinterpreted as fresh frames.
Requests stamped with a superseded epoch or boot incarnation are refused, not reinterpreted.

## Bounds

Every externally influenced quantity is checked against `Limits`: rate, quantum, burst
bytes and packets, window and interval, envelope lifetime, policy count, flow count, envelope
count, attempt count, backend count, journal records and bytes, durable record size,
compaction batch, frame size, connection count, explanation size, name length, retries, and
worker count. The limit set is validated for internal coherence at initialization; an
incoherent set is refused rather than silently corrected.

## Idempotence and identity

Identities are minted from monotonic per-domain counters. Attempt records are indexed by
attempt id and by envelope, so a repeated `apply_envelope` returns the existing attempt and
performs no second backend mutation. Attempt identifiers supplied by a caller are honoured
and conflict-checked: reusing an id for different work is a `Duplicate` error, never a
silent overwrite.
