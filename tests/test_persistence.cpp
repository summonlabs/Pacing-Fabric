// Durability and restart semantics: versioned, integrity-checked, crash-safe
// state; recovery that never restores liveness or effect as current.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "framework.hpp"
#include "harness.hpp"
#include "pacing/serialize.hpp"

using namespace pacing;
using pf_test::Harness;
using pf_test::HarnessOptions;
using pf_test::LoopbackBackend;
using pf_test::TempDir;

namespace {

void make_ready(Harness& h, std::uint64_t ceiling = 10'000'000'000ull,
                std::uint64_t rate = 1'000'000'000ull, LoopbackBackend::Options options = {}) {
  h.start();
  h.install_grant(ceiling);
  h.bind_flow(h.flow);
  h.publish_policy(rate);
  h.register_backend(std::move(options));
}

std::vector<u8> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<u8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const std::vector<u8>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

DurableRecord make_record(DurableRecordType type, u64 sequence, std::vector<u8> payload) {
  DurableRecord record{};
  record.type = type;
  record.sequence = sequence;
  record.timestamp_ns = sequence * 1000;
  record.payload = std::move(payload);
  return record;
}

}  // namespace

PF_TEST(durability, roundtrip_preserves_records) {
  TempDir dir("roundtrip");
  Limits limits{};
  {
    DurableStore store(dir.path(), limits);
    PF_CHECK_OK(store.open());
    for (u64 i = 1; i <= 8; ++i) {
      PF_CHECK_OK(store.append(make_record(DurableRecordType::Meta, i, {static_cast<u8>(i)})));
    }
    PF_CHECK_EQ(store.journal_records(), 8ull);
    PF_CHECK(store.journal_bytes() > 0);
    PF_CHECK_OK(store.close());
  }
  {
    DurableStore store(dir.path(), limits);
    PF_CHECK_OK(store.open());
    PF_CHECK_EQ(store.recovered().size(), std::size_t{8});
    PF_CHECK_EQ(store.recovered()[3].sequence, 4ull);
    PF_CHECK_EQ(store.recovered()[3].payload.size(), std::size_t{1});
    PF_CHECK(!store.needs_checkpoint());
    PF_CHECK_OK(store.close());
  }
}

PF_TEST(durability, reopen_appends_after_existing_records) {
  TempDir dir("append");
  Limits limits{};
  DurableStore store(dir.path(), limits);
  PF_CHECK_OK(store.open());
  PF_CHECK_OK(store.append(make_record(DurableRecordType::Meta, 1, {1, 2, 3})));
  PF_CHECK_OK(store.close());
  PF_CHECK_OK(store.open());
  PF_CHECK_OK(store.append(make_record(DurableRecordType::Meta, 2, {4, 5})));
  PF_CHECK_OK(store.close());

  DurableStore reader(dir.path(), limits);
  PF_CHECK_OK(reader.open());
  PF_CHECK_EQ(reader.recovered().size(), std::size_t{2});
  PF_CHECK_EQ(reader.recovered()[1].sequence, 2ull);
}

PF_TEST(durability, checkpoint_replaces_snapshot_and_truncates_journal) {
  TempDir dir("checkpoint");
  Limits limits{};
  DurableStore store(dir.path(), limits);
  PF_CHECK_OK(store.open());
  for (u64 i = 1; i <= 4; ++i) {
    PF_CHECK_OK(store.append(make_record(DurableRecordType::Envelope, i, {static_cast<u8>(i)})));
  }
  std::vector<DurableRecord> compacted;
  compacted.push_back(make_record(DurableRecordType::Meta, 1, {9, 9}));
  compacted.push_back(make_record(DurableRecordType::Policy, 2, {7}));
  PF_CHECK_OK(store.checkpoint(compacted));
  PF_CHECK_EQ(store.journal_records(), 0ull);
  PF_CHECK_OK(store.append(make_record(DurableRecordType::Attempt, 3, {1})));
  PF_CHECK_OK(store.close());

  DurableStore reader(dir.path(), limits);
  PF_CHECK_OK(reader.open());
  PF_CHECK_EQ(reader.recovered().size(), std::size_t{3});
  PF_CHECK(reader.report().snapshot_present);
  PF_CHECK_EQ(reader.recovered()[0].payload.size(), std::size_t{2});
  PF_CHECK_EQ(reader.recovered()[2].type, DurableRecordType::Attempt);
}

PF_TEST(durability, journal_replay_skips_records_at_or_below_the_snapshot_barrier) {
  TempDir dir("barrier");
  Limits limits{};
  DurableStore store(dir.path(), limits);
  PF_CHECK_OK(store.open());
  PF_CHECK_OK(store.append(make_record(DurableRecordType::Meta, 5, {5})));
  std::vector<DurableRecord> compacted;
  compacted.push_back(make_record(DurableRecordType::Meta, 5, {5}));
  PF_CHECK_OK(store.checkpoint(compacted));
  PF_CHECK_OK(store.close());

  // Simulate a crash between the snapshot rename and the journal truncation:
  // the stale journal records survive and must not be replayed over newer state.
  {
    std::FILE* f = std::fopen(store.journal_path().c_str(), "wb");
    PF_CHECK(f != nullptr);
    const u8 magic[4] = {'P', 'F', 'J', '1'};
    const u8 version[4] = {1, 0, 0, 0};
    const u8 zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    PF_CHECK_EQ(std::fwrite(magic, 1, 4, f), std::size_t{4});
    PF_CHECK_EQ(std::fwrite(version, 1, 4, f), std::size_t{4});
    PF_CHECK_EQ(std::fwrite(zero, 1, 8, f), std::size_t{8});
    std::fclose(f);
  }
  // Re-append the same sequence number through a raw store handle.
  {
    DurableStore writer(dir.path(), limits);
    PF_CHECK_OK(writer.open());
    PF_CHECK_OK(writer.append(make_record(DurableRecordType::Meta, 5, {5})));
    PF_CHECK_OK(writer.append(make_record(DurableRecordType::Meta, 6, {6})));
    PF_CHECK_OK(writer.close());
  }

  DurableStore reader(dir.path(), limits);
  PF_CHECK_OK(reader.open());
  PF_CHECK_EQ(reader.recovered().size(), std::size_t{2});
  PF_CHECK_EQ(reader.recovered()[0].sequence, 5ull);
  PF_CHECK_EQ(reader.recovered()[1].sequence, 6ull);
  PF_CHECK(reader.report().ignored_records >= 1);
}

PF_TEST(durability, corrupt_snapshot_is_refused_not_silently_dropped) {
  TempDir dir("corrupt_snapshot");
  Limits limits{};
  {
    DurableStore store(dir.path(), limits);
    PF_CHECK_OK(store.open());
    PF_CHECK_OK(store.append(make_record(DurableRecordType::Meta, 1, {1})));
    std::vector<DurableRecord> compacted;
    compacted.push_back(make_record(DurableRecordType::Meta, 1, {1, 2, 3, 4}));
    PF_CHECK_OK(store.checkpoint(compacted));
    PF_CHECK_OK(store.close());
  }
  std::string snapshot = (std::filesystem::path(dir.path()) / "snapshot.pfs").string();
  std::vector<u8> bytes = read_file(snapshot);
  PF_CHECK(bytes.size() > DurableStore::kSnapshotHeaderBytes);
  bytes[DurableStore::kSnapshotHeaderBytes + 2] ^= 0xFF;
  write_file(snapshot, bytes);

  DurableStore store(dir.path(), limits);
  PF_CHECK_STATUS_CODE(store.open(), ErrorCode::IntegrityFailure);
  PF_CHECK(!store.is_open());
}

PF_TEST(durability, snapshot_magic_and_version_are_enforced) {
  TempDir magic_dir("bad_magic");
  Limits limits{};
  {
    DurableStore store(magic_dir.path(), limits);
    PF_CHECK_OK(store.open());
    std::vector<DurableRecord> compacted;
    compacted.push_back(make_record(DurableRecordType::Meta, 1, {1}));
    PF_CHECK_OK(store.checkpoint(compacted));
    PF_CHECK_OK(store.close());
  }
  std::string snapshot = (std::filesystem::path(magic_dir.path()) / "snapshot.pfs").string();
  std::vector<u8> bytes = read_file(snapshot);
  bytes[0] = 'X';
  write_file(snapshot, bytes);
  DurableStore reader(magic_dir.path(), limits);
  PF_CHECK_STATUS_CODE(reader.open(), ErrorCode::IntegrityFailure);

  TempDir version_dir("bad_version");
  {
    DurableStore store(version_dir.path(), limits);
    PF_CHECK_OK(store.open());
    std::vector<DurableRecord> compacted;
    compacted.push_back(make_record(DurableRecordType::Meta, 1, {1}));
    PF_CHECK_OK(store.checkpoint(compacted));
    PF_CHECK_OK(store.close());
  }
  std::string versioned = (std::filesystem::path(version_dir.path()) / "snapshot.pfs").string();
  std::vector<u8> vbytes = read_file(versioned);
  vbytes[4] = 99;
  write_file(versioned, vbytes);
  DurableStore vreader(version_dir.path(), limits);
  PF_CHECK_STATUS_CODE(vreader.open(), ErrorCode::Unsupported);
}

PF_TEST(durability, torn_journal_tail_replays_only_the_valid_prefix) {
  TempDir dir("torn");
  Limits limits{};
  {
    DurableStore store(dir.path(), limits);
    PF_CHECK_OK(store.open());
    for (u64 i = 1; i <= 5; ++i) {
      PF_CHECK_OK(store.append(make_record(DurableRecordType::Attempt, i, {1, 2, 3, 4})));
    }
    PF_CHECK_OK(store.close());
  }
  const std::string journal = (std::filesystem::path(dir.path()) / "journal.pfj").string();
  std::vector<u8> bytes = read_file(journal);
  PF_CHECK(bytes.size() > 40);
  bytes.resize(bytes.size() - 17);  // sever the final record mid-payload
  write_file(journal, bytes);

  DurableStore store(dir.path(), limits);
  PF_CHECK_OK(store.open());
  PF_CHECK_EQ(store.recovered().size(), std::size_t{4});
  PF_CHECK(store.report().torn_tail_bytes > 0);
  PF_CHECK_EQ(store.report().corrupt_records, 0ull);
}

PF_TEST(durability, corrupt_journal_record_stops_replay_at_the_defect) {
  TempDir dir("corrupt_journal");
  Limits limits{};
  {
    DurableStore store(dir.path(), limits);
    PF_CHECK_OK(store.open());
    for (u64 i = 1; i <= 3; ++i) {
      PF_CHECK_OK(store.append(make_record(DurableRecordType::Attempt, i, {7, 7, 7, 7})));
    }
    PF_CHECK_OK(store.close());
  }
  const std::string journal = (std::filesystem::path(dir.path()) / "journal.pfj").string();
  std::vector<u8> bytes = read_file(journal);
  // Flip a byte inside the second record's payload, leaving the first intact.
  const std::size_t record_stride = DurableStore::kRecordHeaderBytes + 4;
  bytes[DurableStore::kJournalHeaderBytes + record_stride + DurableStore::kRecordHeaderBytes + 1] ^= 0x5A;
  write_file(journal, bytes);

  DurableStore store(dir.path(), limits);
  PF_CHECK_OK(store.open());
  PF_CHECK_EQ(store.recovered().size(), std::size_t{1});
  PF_CHECK_EQ(store.report().corrupt_records, 1ull);
  PF_CHECK(store.report().torn_tail_bytes > 0);
}

PF_TEST(durability, oversized_records_are_refused) {
  TempDir dir("oversized");
  Limits limits{};
  limits.max_durable_record_bytes = 64;
  limits.max_journal_bytes = 1u << 20;
  limits.max_journal_records = 1u << 20;
  DurableStore store(dir.path(), limits);
  PF_CHECK_OK(store.open());
  std::vector<u8> big(65, 0);
  PF_CHECK_STATUS_CODE(store.append(make_record(DurableRecordType::Meta, 1, big)), ErrorCode::Oversized);
  PF_CHECK_OK(store.append(make_record(DurableRecordType::Meta, 2, std::vector<u8>(64, 0))));
  PF_CHECK_OK(store.close());
}

PF_TEST(durability, journal_growth_is_bounded_and_forces_a_checkpoint) {
  TempDir dir("bounded");
  Limits limits{};
  limits.max_journal_records = 32;
  Harness h{dir.path(), 7, HarnessOptions{limits}};
  make_ready(h);
  for (int i = 0; i < 80; ++i) {
    const PacingEnvelope envelope = h.derive(h.flow);
    (void)h.apply(envelope);
  }
  const FabricStats stats = h.fabric->stats();
  PF_CHECK(stats.checkpoints >= 1);
  const DurabilityReport report = h.fabric->durability_report();
  PF_CHECK(report.journal_records < limits.max_journal_records);
  PF_CHECK(h.fabric->running());
}

PF_TEST(restart, recovery_invalidates_every_prior_effect) {
  TempDir dir("restart");
  AttemptId attempt{};
  EnvelopeId envelope_id{};
  Epoch epoch_before{};
  BootId boot_before{};
  {
    Harness h(dir.path());
    make_ready(h);
    const PacingEnvelope envelope = h.derive(h.flow);
    envelope_id = envelope.ref.id;
    attempt = h.apply(envelope);
    PF_CHECK_EQ(h.record(attempt).state, ApplicationState::Applied);
    epoch_before = h.fabric->epoch();
    boot_before = h.fabric->boot();
    PF_CHECK_OK(h.fabric->shutdown());
  }

  Harness restored(dir.path());
  restored.start();
  const RecoveryReport report = restored.fabric->recovery_report();
  PF_CHECK(report.performed);
  PF_CHECK(report.durable_directory_used);
  PF_CHECK_EQ(report.code, ErrorCode::Ok);
  PF_CHECK_EQ(report.records_rejected, 0ull);
  PF_CHECK(report.envelopes_invalidated >= 1);
  PF_CHECK(report.attempts_requiring_revalidation >= 1);
  PF_CHECK(report.epoch_after > epoch_before);
  PF_CHECK_NE(report.boot_after.nonce, boot_before.nonce);

  // The effect is history, never current fact.
  const ApplicationRecord record = restored.record(attempt);
  PF_CHECK_EQ(record.state, ApplicationState::RequiresRevalidation);
  PF_CHECK_EQ(record.last_known_state, ApplicationState::Applied);
  PF_CHECK_EQ(record.effect, EffectLabel::None);
  PF_CHECK(record.requires_revalidation);
  PF_CHECK(!record.claims_effect());

  // The old envelope is fenced and can never be applied again.
  auto reuse = restored.fabric->apply_envelope(EnvelopeRef{envelope_id, Generation::from(1)},
                                               restored.backend_id);
  PF_CHECK_CODE(reuse, ErrorCode::Revoked);

  // No backend liveness survives the restart.
  PF_CHECK(restored.fabric->list_backends().empty());
  PF_CHECK(report.backends_requiring_registration >= 1);
}

PF_TEST(restart, configuration_is_restored_and_new_work_succeeds) {
  TempDir dir("restart_config");
  {
    Harness h(dir.path());
    make_ready(h);
    (void)h.apply(h.derive(h.flow));
    PF_CHECK_OK(h.fabric->shutdown());
  }
  Harness restored(dir.path());
  restored.start();
  PF_CHECK_EQ(restored.fabric->recovery_report().bindings_restored, 1ull);
  PF_CHECK_EQ(restored.fabric->recovery_report().policies_restored, 1ull);
  PF_CHECK(restored.fabric->list_backends().empty());

  // Register a fresh backend and a fresh grant: the restored policy and binding
  // are configuration and are safe to reuse, but the epoch has moved.
  restored.install_grant();
  restored.register_backend();
  const PacingEnvelope envelope = restored.derive(restored.flow);
  PF_CHECK_EQ(envelope.authority.epoch.value(), restored.fabric->epoch().value());
  PF_CHECK_NE(envelope.authority.epoch.value(), 1ull);
  const AttemptId id = restored.apply(envelope);
  PF_CHECK_EQ(restored.record(id).state, ApplicationState::Applied);
}

PF_TEST(restart, unverified_attempts_stay_unproven) {
  TempDir dir("restart_unverified");
  AttemptId attempt{};
  {
    LoopbackBackend::Options options{};
    options.supports_readback = false;
    Harness h(dir.path());
    make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);
    attempt = h.apply(h.derive(h.flow));
    PF_CHECK_EQ(h.record(attempt).state, ApplicationState::AppliedUnverified);
    PF_CHECK_OK(h.fabric->shutdown());
  }
  Harness restored(dir.path());
  restored.start();
  const ApplicationRecord record = restored.record(attempt);
  PF_CHECK_EQ(record.state, ApplicationState::RequiresRevalidation);
  PF_CHECK_EQ(record.effect, EffectLabel::None);
  PF_CHECK(record.requires_revalidation);
}

PF_TEST(restart, revalidation_after_restart_reports_authority_drift) {
  TempDir dir("restart_revalidate");
  AttemptId attempt{};
  {
    Harness h(dir.path());
    make_ready(h);
    attempt = h.apply(h.derive(h.flow));
    PF_CHECK_OK(h.fabric->shutdown());
  }
  Harness restored(dir.path());
  restored.start();
  restored.install_grant();

  // A restart advances the coordinator epoch, so the recorded authority that
  // justified the old effect no longer exists. Revalidation reports the drift
  // instead of quietly re-asserting the effect.
  PF_CHECK_STATUS_CODE(restored.fabric->revalidate(attempt), ErrorCode::StaleEpoch);
  PF_CHECK_EQ(restored.record(attempt).state, ApplicationState::Stale);
  PF_CHECK(!restored.record(attempt).claims_effect());

  // Registering a live backend cannot resurrect authority that has moved.
  restored.register_backend();
  PF_CHECK_STATUS_CODE(restored.fabric->revalidate(attempt), ErrorCode::StaleEpoch);

  // The sanctioned recovery path is to derive fresh authority and apply again.
  const PacingEnvelope envelope = restored.derive(restored.flow);
  const AttemptId renewed = restored.apply(envelope);
  PF_CHECK_EQ(restored.record(renewed).state, ApplicationState::Applied);
  PF_CHECK_EQ(restored.record(renewed).effect, EffectLabel::SyntheticVerified);
}

PF_TEST(restart, revalidation_requires_a_registered_backend) {
  Harness h;
  make_ready(h);
  const AttemptId id = h.apply(h.derive(h.flow));
  PF_CHECK_OK(h.fabric->unregister_backend(h.backend_id));
  PF_CHECK_STATUS_CODE(h.fabric->revalidate(id), ErrorCode::BackendUnavailable);
}

PF_TEST(restart, unfinished_attempts_are_recovered_as_ambiguous) {
  TempDir dir("restart_ambiguous");
  AttemptId attempt{};
  {
    Harness h(dir.path());
    make_ready(h);
    attempt = h.apply(h.derive(h.flow));
    PF_CHECK_OK(h.fabric->shutdown());
  }

  // Simulate a coordinator that died while an attempt was handed to a backend:
  // a durable record exists with no terminal outcome.
  Limits limits{};
  AttemptId unfinished{};
  {
    DurableStore store(dir.path(), limits);
    PF_CHECK_OK(store.open());
    const auto& recovered = store.recovered();
    const ApplicationRecord* source = nullptr;
    for (const auto& record : recovered) {
      if (record.type != DurableRecordType::Attempt) continue;
      ByteReader reader(std::span<const u8>(record.payload.data(), record.payload.size()));
      ApplicationRecord decoded{};
      PF_CHECK_OK(decode_attempt(reader, limits, decoded));
      if (decoded.attempt.value() == attempt.value()) source = nullptr;
      if (decoded.state == ApplicationState::Applied) {
        static ApplicationRecord held{};
        held = decoded;
        source = &held;
      }
    }
    PF_CHECK(source != nullptr);
    ApplicationRecord pending = *source;
    pending.attempt = AttemptId::from(9001);
    pending.state = ApplicationState::Submitted;
    pending.last_known_state = ApplicationState::Reserved;
    unfinished = pending.attempt;

    ByteWriter writer(kEncodeCapacityBytes);
    encode_attempt(writer, pending);
    PF_CHECK(writer.ok());
    DurableRecord record{};
    record.type = DurableRecordType::Attempt;
    record.sequence = recovered.back().sequence + 1;
    record.timestamp_ns = 1;
    record.payload = writer.take();
    PF_CHECK_OK(store.append(std::move(record)));
    PF_CHECK_OK(store.close());
  }

  Harness restored(dir.path());
  restored.start();
  const ApplicationRecord pending = restored.record(unfinished);
  PF_CHECK_EQ(pending.state, ApplicationState::Ambiguous);
  PF_CHECK_EQ(pending.reason, ErrorCode::Ambiguous);
  PF_CHECK(pending.requires_revalidation);
  PF_CHECK(!pending.claims_effect());
  PF_CHECK(restored.fabric->recovery_report().attempts_ambiguous >= 1);
}

PF_TEST(restart, compaction_does_not_lose_recent_work) {
  TempDir dir("restart_compaction");
  Limits limits{};
  limits.max_journal_records = 24;
  std::vector<AttemptId> attempts;
  std::vector<EnvelopeId> envelopes;
  {
    Harness h{dir.path(), 7, HarnessOptions{limits}};
    make_ready(h);
    for (int i = 0; i < 40; ++i) {
      const PacingEnvelope envelope = h.derive(h.flow);
      envelopes.push_back(envelope.ref.id);
      attempts.push_back(h.apply(envelope));
    }
    PF_CHECK(h.fabric->stats().checkpoints >= 2);
    PF_CHECK_OK(h.fabric->shutdown());
  }
  Harness restored(dir.path());
  restored.start();
  // Every attempt -- including those written after the last compaction -- must
  // survive the restart. A sequence-barrier mistake would silently drop them.
  const auto recovered = restored.fabric->attempts_for_flow(restored.flow);
  PF_CHECK_EQ(recovered.size(), attempts.size());
  for (const AttemptId id : attempts) {
    const ApplicationRecord record = restored.record(id);
    PF_CHECK_EQ(record.state, ApplicationState::RequiresRevalidation);
    PF_CHECK(!record.claims_effect());
  }
  PF_CHECK_EQ(restored.fabric->envelopes_for_flow(restored.flow).size(), envelopes.size());
}

PF_TEST(durability, stale_journal_after_checkpoint_cannot_regress_state) {
  TempDir dir("crash_window");
  Limits limits{};
  DurableStore store(dir.path(), limits);
  PF_CHECK_OK(store.open());
  for (u64 i = 1; i <= 3; ++i) {
    PF_CHECK_OK(store.append(make_record(DurableRecordType::Meta, i, {static_cast<u8>(i)})));
  }
  const std::string journal = (std::filesystem::path(dir.path()) / "journal.pfj").string();
  const std::vector<u8> stale_journal = read_file(journal);

  // The store assigns the snapshot records the next free sequence numbers, so a
  // simulated crash before the journal truncation leaves only superseded bytes.
  std::vector<DurableRecord> compacted;
  compacted.push_back(make_record(DurableRecordType::Meta, 4, {4}));
  compacted.push_back(make_record(DurableRecordType::Meta, 5, {5}));
  PF_CHECK_OK(store.checkpoint(compacted));
  PF_CHECK_OK(store.close());

  write_file(journal, stale_journal);

  DurableStore reader(dir.path(), limits);
  PF_CHECK_OK(reader.open());
  PF_CHECK_EQ(reader.recovered().size(), std::size_t{2});
  PF_CHECK_EQ(reader.recovered()[0].sequence, 4ull);
  PF_CHECK_EQ(reader.recovered()[1].sequence, 5ull);
  PF_CHECK(reader.report().ignored_records >= 3);
}

PF_TEST(durability, no_directory_means_no_durability) {
  Harness h;  // empty durable directory
  h.start();
  PF_CHECK(!h.fabric->recovery_report().performed);
  PF_CHECK(!h.fabric->recovery_report().durable_directory_used);
  const DurabilityReport report = h.fabric->durability_report();
  PF_CHECK(!report.snapshot_present);
  PF_CHECK(!report.journal_present);
}
