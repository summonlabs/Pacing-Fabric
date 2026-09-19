// Pacing Fabric - deterministic, bounded encoding for durable records and
// control-plane payloads.
//
// Decoders validate every length, count and identity against Limits and refuse
// structurally invalid input instead of trusting a producer. Encoders never
// emit a partial record: a poisoned writer reports failure to the caller.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef PACING_FABRIC_SERIALIZE_HPP
#define PACING_FABRIC_SERIALIZE_HPP

#include "pacing/application.hpp"
#include "pacing/binding.hpp"
#include "pacing/codec.hpp"
#include "pacing/policy.hpp"
#include "pacing/upstream.hpp"

namespace pacing {

inline constexpr std::size_t kEncodeCapacityBytes = 1u << 20;

void encode_binding(ByteWriter& w, const FlowBinding& binding);
Status decode_binding(ByteReader& r, const Limits& limits, FlowBinding& out);

void encode_policy(ByteWriter& w, const PacingPolicy& policy);
Status decode_policy(ByteReader& r, const Limits& limits, PacingPolicy& out);

void encode_envelope(ByteWriter& w, const PacingEnvelope& envelope);
Status decode_envelope(ByteReader& r, const Limits& limits, PacingEnvelope& out);

void encode_attempt(ByteWriter& w, const ApplicationRecord& record);
Status decode_attempt(ByteReader& r, const Limits& limits, ApplicationRecord& out);

void encode_grant(ByteWriter& w, const RateGrant& grant);
Status decode_grant(ByteReader& r, const Limits& limits, RateGrant& out);

void encode_revocation(ByteWriter& w, EnvelopeRef envelope, std::string_view reason);
Status decode_revocation(ByteReader& r, EnvelopeRef& envelope, std::string& reason);

void encode_epoch(ByteWriter& w, Epoch epoch, BootId boot);
Status decode_epoch(ByteReader& r, Epoch& epoch, BootId& boot);

void encode_identity_u64(ByteWriter& w, u64 value);
Status decode_identity_u64(ByteReader& r, u64& value);

void encode_backend_descriptor(ByteWriter& w, const BackendDescriptor& descriptor);
Status decode_backend_descriptor(ByteReader& r, const Limits& limits, BackendDescriptor& out);

void encode_cadence(ByteWriter& w, const Cadence& cadence);
Status decode_cadence(ByteReader& r, Cadence& out);

void encode_authority(ByteWriter& w, const AuthorityVector& authority);
Status decode_authority(ByteReader& r, AuthorityVector& out);

}  // namespace pacing

#endif  // PACING_FABRIC_SERIALIZE_HPP
