# Semantics

## Authority

A decision is only meaningful relative to the exact evidence that justified it. That
evidence is an `AuthorityVector`:

    flow@generation
    resource@generation
    policy@generation
    rate_grant@generation
    path@generation
    coordinator epoch
    coordinator boot (counter and nonce)

`compare_authority` reports a specific drift rather than a generic staleness flag, and an
incomplete vector is reported as `Incomplete` before any field comparison. Every envelope,
attempt record, and durable record carries the vector that produced it.

## Derivation

Derivation is a pure function of a policy, a rate grant, and the declared flow bounds.

1. The input is bounded and shape-checked. A non-zero ceiling is required; an unknown
   ceiling is UNKNOWN, not zero.
2. A policy floor above the authorized ceiling is refused with `EntitlementMint`. The
   fabric does not manufacture bandwidth.
3. The target rate is the absolute request, or a parts-per-million share of the ceiling.
   A request above the ceiling is clamped down and flagged `clamped_to_ceiling`.
4. The interval is a ceiling division of `quantum * 8 * 1e9 / rate`, so the realized rate is
   at or below the target by construction. A windowed shape hint may only make the cadence
   sparser.
5. Window accounting is a floor division, so `bytes_per_window` can never over-authorize.
6. A zero burst allowance means one quantum in flight; an allowance below one quantum is
   rejected; a token-bucket depth above what the authorized rate can accumulate over the
   configured horizon is rejected.
7. The assembled cadence is re-validated, and the realized rate is compared against the
   ceiling one final time.

All arithmetic is integer-only with checked 128-bit multiply and divide. No floating point
participates anywhere in a pacing decision.

## Lifecycle

    UNKNOWN
      -> PLANNED -> RESERVED -> SUBMITTED -> ACKNOWLEDGED -> APPLIED
                                                          -> APPLIED_UNVERIFIED
                                                          -> MISMATCHED
    terminal without effect: CANCELLED, REVOKED, FENCED, STALE, FAILED, AMBIGUOUS
    REQUIRES_REVALIDATION: durable history that is not current fact

RESERVED is the durable state of a newly created attempt. SUBMITTED means the request has
been handed to a backend in this process; it is not durable, because a crash in that window
is recovered as AMBIGUOUS, which is exactly the truth about an attempt whose backend outcome
was never observed.

The effect label is a pure function of the state:

| State | Permitted effect label |
| --- | --- |
| APPLIED | VERIFIED (REAL backend) or SYNTHETIC_VERIFIED (SYNTHETIC backend) |
| APPLIED_UNVERIFIED | UNVERIFIED |
| MISMATCHED | MISMATCHED |
| anything else | NONE |

`requires_revalidation` is orthogonal: while it is set, `claims_effect` is false
regardless of state.

## Readback matching

Evidence matches only when it reproduces the authorized cadence exactly: attempt identity,
backend identity, rate, quantum, burst, and interval. A backend pacing below authority is
safe but is still not what was authorized, and reporting it as a match would hide a real
divergence. Evidence whose integrity check fails is treated as absent. When the readback
rate exceeds the authorized rate, the record becomes MISMATCHED with `CeilingExceeded` and
the fabric withdraws the over-rate effect.

## Cancellation, revocation and fencing

Cancellation is terminal. If a backend completion arrives after an attempt was closed, the
completion is discarded, the record keeps its closed state, and a compensating withdrawal is
issued. Cancelling an attempt that already asserted an effect moves it to REVOKED; cancelling
one that had not moves it to CANCELLED. Both paths issue the withdrawal, because the backend
may have installed pacing without the fabric having observed it yet.

Revoking an envelope withdraws every attempt on it and blocks new attempts. Fencing below an
epoch invalidates every envelope and every attempt stamped with an older epoch - including
attempts that were applied - and issues a withdrawal for anything that claimed an effect.
Advancing the coordinator epoch does the same and additionally instructs every registered
backend to discard its older-epoch state.

## Durability

    snapshot.pfs : "PFSN" | version | flags | record count | sequence barrier | body CRC-32C
    journal.pfj  : "PFJ1" | version, then self-describing records

Each durable record is `type | flags | payload length | sequence | timestamp | payload
CRC-32C`. A record is never acknowledged before it is flushed to stable storage.

The snapshot's sequence barrier is the highest journal sequence it already includes. Compacted
records continue the same monotonic sequence as journal records, so an interruption between
the snapshot rename and the journal truncation can only leave superseded bytes behind, which
the barrier discards on replay. Recovery reports a torn tail explicitly and never invents
state for bytes it could not validate. A corrupt snapshot is a hard failure: the coordinator
refuses to start rather than starting empty.

## Recovery

On restart the coordinator advances both its epoch and its boot incarnation, then:

* every persisted envelope is invalidated;
* an attempt that never reached a terminal state becomes AMBIGUOUS;
* an applied, acknowledged, or mismatched attempt becomes REQUIRES_REVALIDATION and asserts
  no effect;
* no backend handle, liveness, or effect is restored. Backend descriptors are restored as
  historical lineage only.

`revalidate` is the deliberate path back: it re-derives live authority and requires a
positive, matching backend readback. Without that evidence the record stays
REQUIRES_REVALIDATION. Across a restart, the authority vector has necessarily moved, so
revalidation reports the drift and the sanctioned recovery is to derive fresh authority and
apply again under the new epoch.

## Explanation

`explain` returns a structured `Explanation` plus bounded text and JSON renderings. It
reports the cadence, the envelope, upstream authority, the burst budget, desired versus
applied status, backend evidence, and the stale or revoke reason. Renderings are truncated
at the configured bound with an explicit marker, so a truncated explanation is never mistaken
for a complete one.
