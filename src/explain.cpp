// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "pacing/explain.hpp"

#include <string>

namespace pacing {
namespace {

constexpr const char* kTruncationMarker = "\n[truncated: explanation exceeded its configured bound]";

void append_field(std::string& out, const char* key, const std::string& value) {
  out += "  ";
  out += key;
  out += ": ";
  out += value;
  out += "\n";
}

void append_field(std::string& out, const char* key, u64 value) {
  append_field(out, key, std::to_string(value));
}

void append_field(std::string& out, const char* key, bool value) {
  append_field(out, key, std::string(value ? "true" : "false"));
}

std::string json_escape(const std::string& in, std::size_t limit) {
  std::string out;
  out.reserve(in.size() + 8);
  std::size_t emitted = 0;
  for (const char ch : in) {
    if (emitted >= limit) {
      out += "...";
      break;
    }
    const unsigned char uc = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (uc < 0x20) {
          static const char* kHex = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(uc >> 4) & 0xF];
          out += kHex[uc & 0xF];
        } else {
          out += ch;
        }
        break;
    }
    emitted += 1;
  }
  return out;
}

void json_member(std::string& out, const char* key, const std::string& value, bool& first) {
  if (!first) out += ",";
  first = false;
  out += "\"";
  out += key;
  out += "\":\"";
  out += json_escape(value, 256);
  out += "\"";
}

void json_number(std::string& out, const char* key, u64 value, bool& first) {
  if (!first) out += ",";
  first = false;
  out += "\"";
  out += key;
  out += "\":";
  out += std::to_string(value);
}

void json_flag(std::string& out, const char* key, bool value, bool& first) {
  if (!first) out += ",";
  first = false;
  out += "\"";
  out += key;
  out += "\":";
  out += value ? "true" : "false";
}

}  // namespace

std::string bound_explanation(std::string text, std::size_t max_bytes) {
  if (max_bytes == 0) return std::string{};
  if (text.size() <= max_bytes) return text;
  const std::size_t marker_len = std::char_traits<char>::length(kTruncationMarker);
  if (max_bytes <= marker_len) return text.substr(0, max_bytes);
  std::string out = text.substr(0, max_bytes - marker_len);
  out += kTruncationMarker;
  return out;
}

std::string Explanation::to_text(std::size_t max_bytes) const {
  std::string out;
  out.reserve(1024);
  out += "pacing explanation\n";
  append_field(out, "flow", flow.value());
  append_field(out, "flow_generation", flow_generation.value());
  append_field(out, "resource", resource.value());
  append_field(out, "binding_present", has_binding);
  append_field(out, "envelope_present", has_envelope);
  append_field(out, "attempt_present", has_attempt);
  append_field(out, "grant_present", has_grant);
  append_field(out, "refusal", std::string(pacing::to_string(refusal)));
  if (!refusal_detail.empty()) append_field(out, "refusal_detail", refusal_detail);
  append_field(out, "authority_drift", std::string(pacing::to_string(drift)));
  if (!stale_or_revoke_reason.empty()) {
    append_field(out, "stale_or_revoke_reason", stale_or_revoke_reason);
  }
  append_field(out, "asserted_effect", std::string(pacing::to_string(effect)));

  if (has_grant) {
    out += "upstream_rate_authority\n";
    append_field(out, "grant", to_string(grant.ref));
    append_field(out, "ceiling_bps", grant.ceiling_bps);
    append_field(out, "floor_bps", grant.floor_bps);
    append_field(out, "upstream_revision", grant.revision);
    append_field(out, "authoritative", grant.authoritative);
    append_field(out, "valid_until_ns", grant.valid_until.is_set() ? grant.valid_until.instant().ns() : 0);
  }

  if (has_envelope) {
    out += "pacing_envelope\n";
    append_field(out, "envelope", to_string(envelope.ref));
    append_field(out, "requested_rate_bps", envelope.requested_rate_bps);
    append_field(out, "authorized_rate_bps", envelope.cadence.rate_bps);
    append_field(out, "ceiling_bps", envelope.ceiling_bps);
    append_field(out, "floor_bps", envelope.floor_bps);
    append_field(out, "clamped_to_ceiling", envelope.clamped_to_ceiling);
    append_field(out, "clamped_to_floor", envelope.clamped_to_floor);
    append_field(out, "shape", std::string(pacing::to_string(envelope.cadence.shape)));
    append_field(out, "quantum_bytes", envelope.cadence.quantum_bytes);
    append_field(out, "interval_ns", envelope.cadence.interval_ns);
    append_field(out, "window_ns", envelope.cadence.window_ns);
    append_field(out, "grants_per_window", envelope.cadence.grants_per_window);
    append_field(out, "bytes_per_window", envelope.cadence.bytes_per_window);
    append_field(out, "burst_budget_bytes", burst_budget_bytes != 0 ? burst_budget_bytes
                                                                    : envelope.cadence.burst_bytes);
    append_field(out, "burst_budget_packets",
                 burst_budget_packets != 0 ? burst_budget_packets : envelope.cadence.burst_packets);
    append_field(out, "upstream_synthetic", envelope.upstream_synthetic);
    append_field(out, "policy_synthetic", envelope.policy_synthetic);
    append_field(out, "authority", pacing::to_string(envelope.authority));
    append_field(out, "authority_digest", envelope.authority.digest());
    append_field(out, "valid_until_ns",
                 envelope.valid_until.is_set() ? envelope.valid_until.instant().ns() : 0);
  }

  if (has_attempt) {
    out += "application_state\n";
    append_field(out, "attempt", attempt.attempt.value());
    append_field(out, "state", std::string(pacing::to_string(attempt.state)));
    append_field(out, "last_known_state", std::string(pacing::to_string(attempt.last_known_state)));
    append_field(out, "effect", std::string(pacing::to_string(attempt.effect)));
    append_field(out, "evidence_kind", std::string(pacing::to_string(attempt.evidence_kind)));
    append_field(out, "backend", to_string(attempt.backend));
    append_field(out, "backend_synthetic", attempt.backend_synthetic);
    append_field(out, "observed_rate_bps", attempt.observed_rate_bps);
    append_field(out, "observed_quantum_bytes", attempt.observed_quantum_bytes);
    append_field(out, "observed_burst_bytes", attempt.observed_burst_bytes);
    append_field(out, "backend_seq", attempt.backend_seq);
    append_field(out, "evidence_integrity_ok", attempt.evidence_integrity_ok);
    append_field(out, "reason", std::string(pacing::to_string(attempt.reason)));
    append_field(out, "requires_revalidation", attempt.requires_revalidation);
    append_field(out, "revision", attempt.revision);
    if (!attempt.detail.empty()) append_field(out, "detail", attempt.detail);
  }

  return bound_explanation(std::move(out), max_bytes);
}

std::string Explanation::to_json(std::size_t max_bytes) const {
  std::string out;
  out.reserve(1024);
  out += "{";
  bool first = true;
  json_number(out, "flow", flow.value(), first);
  json_number(out, "flow_generation", flow_generation.value(), first);
  json_number(out, "resource", resource.value(), first);
  json_flag(out, "binding_present", has_binding, first);
  json_flag(out, "envelope_present", has_envelope, first);
  json_flag(out, "attempt_present", has_attempt, first);
  json_flag(out, "grant_present", has_grant, first);
  json_member(out, "refusal", std::string(pacing::to_string(refusal)), first);
  json_member(out, "refusal_detail", refusal_detail, first);
  json_member(out, "authority_drift", std::string(pacing::to_string(drift)), first);
  json_member(out, "stale_or_revoke_reason", stale_or_revoke_reason, first);
  json_member(out, "asserted_effect", std::string(pacing::to_string(effect)), first);

  if (has_grant) {
    json_number(out, "ceiling_bps", grant.ceiling_bps, first);
    json_number(out, "floor_bps", grant.floor_bps, first);
    json_number(out, "upstream_revision", grant.revision, first);
    json_flag(out, "authoritative", grant.authoritative, first);
  }
  if (has_envelope) {
    json_number(out, "envelope_id", envelope.ref.id.value(), first);
    json_number(out, "envelope_generation", envelope.ref.generation.value(), first);
    json_number(out, "requested_rate_bps", envelope.requested_rate_bps, first);
    json_number(out, "authorized_rate_bps", envelope.cadence.rate_bps, first);
    json_flag(out, "clamped_to_ceiling", envelope.clamped_to_ceiling, first);
    json_flag(out, "clamped_to_floor", envelope.clamped_to_floor, first);
    json_member(out, "shape", std::string(pacing::to_string(envelope.cadence.shape)), first);
    json_number(out, "quantum_bytes", envelope.cadence.quantum_bytes, first);
    json_number(out, "interval_ns", envelope.cadence.interval_ns, first);
    json_number(out, "window_ns", envelope.cadence.window_ns, first);
    json_number(out, "grants_per_window", envelope.cadence.grants_per_window, first);
    json_number(out, "bytes_per_window", envelope.cadence.bytes_per_window, first);
    json_number(out, "burst_budget_bytes",
                burst_budget_bytes != 0 ? burst_budget_bytes : envelope.cadence.burst_bytes, first);
    json_number(out, "burst_budget_packets",
                burst_budget_packets != 0 ? burst_budget_packets : envelope.cadence.burst_packets, first);
    json_flag(out, "upstream_synthetic", envelope.upstream_synthetic, first);
    json_flag(out, "policy_synthetic", envelope.policy_synthetic, first);
    json_number(out, "authority_digest", envelope.authority.digest(), first);
  }
  if (has_attempt) {
    json_number(out, "attempt_id", attempt.attempt.value(), first);
    json_member(out, "state", std::string(pacing::to_string(attempt.state)), first);
    json_member(out, "last_known_state", std::string(pacing::to_string(attempt.last_known_state)), first);
    json_member(out, "effect", std::string(pacing::to_string(attempt.effect)), first);
    json_member(out, "evidence_kind", std::string(pacing::to_string(attempt.evidence_kind)), first);
    json_member(out, "backend", to_string(attempt.backend), first);
    json_flag(out, "backend_synthetic", attempt.backend_synthetic, first);
    json_number(out, "observed_rate_bps", attempt.observed_rate_bps, first);
    json_number(out, "observed_quantum_bytes", attempt.observed_quantum_bytes, first);
    json_number(out, "observed_burst_bytes", attempt.observed_burst_bytes, first);
    json_number(out, "backend_seq", attempt.backend_seq, first);
    json_flag(out, "evidence_integrity_ok", attempt.evidence_integrity_ok, first);
    json_member(out, "reason", std::string(pacing::to_string(attempt.reason)), first);
    json_flag(out, "requires_revalidation", attempt.requires_revalidation, first);
    json_number(out, "revision", attempt.revision, first);
    json_member(out, "detail", attempt.detail, first);
  }
  out += "}";
  return bound_explanation(std::move(out), max_bytes);
}

}  // namespace pacing
