// Concurrency and race tests. Every assertion inside a worker thread is
// collected rather than thrown, so a failure is reported instead of terminating.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "framework.hpp"
#include "harness.hpp"

using namespace pacing;
using pf_test::Harness;
using pf_test::HarnessOptions;
using pf_test::LoopbackBackend;

namespace {

class Failures {
 public:
  void note(const std::string& message) {
    if (count_.fetch_add(1) == 0) {
      std::lock_guard<std::mutex> lock(mu_);
      first_ = message;
    }
  }
  [[nodiscard]] int count() const noexcept { return count_.load(); }
  [[nodiscard]] std::string first() const {
    std::lock_guard<std::mutex> lock(mu_);
    return first_;
  }

 private:
  std::atomic<int> count_{0};
  mutable std::mutex mu_;
  std::string first_;
};

void make_ready(Harness& h, std::uint64_t ceiling = 10'000'000'000ull,
                std::uint64_t rate = 1'000'000'000ull, LoopbackBackend::Options options = {}) {
  h.start();
  h.install_grant(ceiling);
  h.bind_flow(h.flow);
  h.publish_policy(rate);
  h.register_backend(std::move(options));
}

}  // namespace

#define PF_THREAD_CHECK(counter, cond)                                                       \
  do {                                                                                       \
    if (!(cond)) {                                                                           \
      (counter).note(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #cond);    \
    }                                                                                        \
  } while (false)

PF_TEST(concurrency, duplicate_apply_from_many_threads_calls_the_backend_once) {
  Harness h;
  make_ready(h);
  const PacingEnvelope envelope = h.derive(h.flow);
  Failures failures;
  std::vector<AttemptId> ids(8);
  {
    std::vector<std::jthread> threads;
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&, t] {
        auto result = h.fabric->apply_envelope(envelope.ref, h.backend_id);
        if (!result) {
          failures.note(std::string("apply failed: ") + std::string(to_string(result.code())));
          return;
        }
        ids[static_cast<std::size_t>(t)] = result.value();
      });
    }
  }
  PF_CHECK_EQ(failures.count(), 0);
  for (std::size_t i = 1; i < ids.size(); ++i) PF_CHECK_EQ(ids[i].value(), ids[0].value());
  PF_CHECK_EQ(h.backend->apply_entries(), 1ull);
  PF_CHECK_EQ(h.fabric->stats().applies_reserved, 1ull);
  PF_CHECK_EQ(h.fabric->stats().applies_committed, 1ull);
}

PF_TEST(concurrency, concurrent_derivation_produces_distinct_envelopes) {
  Harness h;
  make_ready(h);
  Failures failures;
  std::mutex ids_mu;
  std::vector<EnvelopeId> ids;
  {
    std::vector<std::jthread> threads;
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&] {
        for (int i = 0; i < 25; ++i) {
          auto envelope = h.fabric->derive_envelope(h.flow);
          if (!envelope) {
            failures.note(std::string("derive failed: ") + std::string(to_string(envelope.code())));
            return;
          }
          std::lock_guard<std::mutex> lock(ids_mu);
          ids.push_back(envelope.value().ref.id);
        }
      });
    }
  }
  PF_CHECK_EQ(failures.count(), 0);
  PF_CHECK_EQ(ids.size(), std::size_t{200});
  for (std::size_t i = 0; i < ids.size(); ++i) {
    for (std::size_t j = i + 1; j < ids.size(); ++j) PF_CHECK_NE(ids[i].value(), ids[j].value());
  }
  PF_CHECK_EQ(h.fabric->stats().envelopes_derived, 200ull);
}

PF_TEST(concurrency, cancellation_racing_apply_never_commits_a_cancelled_attempt) {
  Harness h;
  make_ready(h);
  Failures failures;
  std::vector<StatusOr<AttemptId>> results(32, Status::of(ErrorCode::Internal, "not run"));
  std::vector<std::jthread> threads;
  for (int t = 0; t < 32; ++t) {
    threads.emplace_back([&, t] {
      const PacingEnvelope envelope = h.derive(h.flow);
      results[static_cast<std::size_t>(t)] = h.fabric->apply_envelope(envelope.ref, h.backend_id);
      const std::size_t index = static_cast<std::size_t>(t);
      if (results[index] && (index % 2) == 0) {
        (void)h.fabric->cancel_attempt(results[index].value(), "racing cancellation");
      }
    });
  }
  threads.clear();
  PF_CHECK_EQ(failures.count(), 0);

  const auto records = h.fabric->attempts_for_flow(h.flow);
  PF_CHECK_EQ(records.size(), std::size_t{32});
  std::size_t cancelled = 0;
  std::size_t applied = 0;
  for (const auto& record : records) {
    if (record.state == ApplicationState::Cancelled || record.state == ApplicationState::Revoked) {
      ++cancelled;
      // A cancelled attempt must not still assert an effect.
      PF_CHECK(!record.claims_effect());
      PF_CHECK(!h.backend->installed(record.attempt));
    } else if (record.state == ApplicationState::Applied) {
      ++applied;
      // Applied implies matching evidence, never an assumption.
      PF_CHECK(record.evidence_integrity_ok);
      PF_CHECK_EQ(record.evidence_kind, EvidenceKind::SyntheticReadback);
    }
  }
  PF_CHECK(cancelled > 0);
  PF_CHECK(applied > 0);
}

PF_TEST(concurrency, revocation_racing_apply_leaves_no_live_effect) {
  Harness h;
  make_ready(h);
  for (int iteration = 0; iteration < 12; ++iteration) {
    const PacingEnvelope envelope = h.derive(h.flow);
    StatusOr<AttemptId> applied = Status::of(ErrorCode::Internal, "not run");
    {
      std::jthread worker([&] { applied = h.fabric->apply_envelope(envelope.ref, h.backend_id); });
      (void)h.fabric->revoke_envelope(envelope.ref, "racing revocation");
    }
    if (applied) {
      const ApplicationRecord record = h.record(applied.value());
      PF_CHECK(record.state != ApplicationState::Applied);
      PF_CHECK(!record.claims_effect());
      PF_CHECK(!h.backend->installed(applied.value()));
    }
  }
}

PF_TEST(concurrency, epoch_advance_racing_apply_leaves_no_stale_effect) {
  Harness h;
  make_ready(h);
  std::vector<StatusOr<AttemptId>> results(16, Status::of(ErrorCode::Internal, "not run"));
  {
    std::vector<std::jthread> threads;
    for (int t = 0; t < 16; ++t) {
      threads.emplace_back([&, t] {
        const PacingEnvelope envelope = h.derive(h.flow);
        results[static_cast<std::size_t>(t)] = h.fabric->apply_envelope(envelope.ref, h.backend_id);
      });
    }
    std::jthread advancer([&] {
      for (int i = 0; i < 4; ++i) (void)h.fabric->advance_epoch();
    });
  }
  const Epoch current = h.fabric->epoch();
  for (const auto& record : h.fabric->attempts_for_flow(h.flow)) {
    PF_CHECK(!record.claims_effect() || record.authority.epoch == current);
  }
  // Nothing that was applied under a fenced epoch may still be installed.
  for (const auto& result : results) {
    if (!result) continue;
    const ApplicationRecord record = h.record(result.value());
    if (record.authority.epoch != current) {
      PF_CHECK(record.state == ApplicationState::Fenced ||
               record.state == ApplicationState::Revoked);
      PF_CHECK(!h.backend->installed(result.value()));
    }
  }
}

PF_TEST(concurrency, rebinding_racing_derivation_never_yields_torn_authority) {
  Harness h;
  make_ready(h);
  Failures failures;
  std::atomic<bool> stop{false};
  {
    std::jthread rebinder([&] {
      while (!stop.load()) h.rebind();
    });
    std::vector<std::jthread> workers;
    for (int t = 0; t < 6; ++t) {
      workers.emplace_back([&] {
        for (int i = 0; i < 40; ++i) {
          const PacingEnvelope envelope = h.derive(h.flow);
          PF_THREAD_CHECK(failures, envelope.authority.complete());
          PF_THREAD_CHECK(failures, envelope.authority.rate_grant.known());
          const u64 observed = envelope.authority.flow.generation.value();
          PF_THREAD_CHECK(failures, observed >= 1);
        }
      });
    }
    workers.clear();
    stop.store(true);
  }
  PF_CHECK_EQ(failures.count(), 0);
  PF_CHECK(h.fabric->stats().envelopes_derived > 0);
}

PF_TEST(concurrency, observation_is_safe_while_work_proceeds) {
  Harness h;
  make_ready(h);
  Failures failures;
  std::atomic<bool> stop{false};
  {
    std::jthread observer([&] {
      while (!stop.load()) {
        auto explanation = h.fabric->explain(h.flow);
        PF_THREAD_CHECK(failures, explanation.ok());
        if (explanation) {
          const std::string text = explanation.value().to_text(512);
          PF_THREAD_CHECK(failures, text.size() <= 512);
        }
        (void)h.fabric->stats();
        (void)h.fabric->list_backends();
      }
    });
    std::vector<std::jthread> workers;
    for (int t = 0; t < 4; ++t) {
      workers.emplace_back([&] {
        for (int i = 0; i < 30; ++i) {
          auto envelope = h.fabric->derive_envelope(h.flow);
          if (!envelope) continue;
          (void)h.fabric->apply_envelope(envelope.value().ref, h.backend_id);
        }
      });
    }
    workers.clear();
    stop.store(true);
  }
  PF_CHECK_EQ(failures.count(), 0);
}

PF_TEST(concurrency, reentrant_sink_is_safe_under_load) {
  Harness h;
  make_ready(h);
  class Sink final : public IEventSink {
   public:
    explicit Sink(PacingFabric* fabric) : fabric_(fabric) {}
    void on_event(const FabricEvent&) noexcept override {
      ++calls_;
      (void)fabric_->stats();
    }
    std::atomic<int> calls_{0};

   private:
    PacingFabric* fabric_;
  };
  Sink sink(h.fabric.get());
  h.fabric->set_event_sink(&sink);
  {
    std::vector<std::jthread> threads;
    for (int t = 0; t < 6; ++t) {
      threads.emplace_back([&] {
        for (int i = 0; i < 20; ++i) {
          auto envelope = h.fabric->derive_envelope(h.flow);
          if (!envelope) continue;
          (void)h.fabric->apply_envelope(envelope.value().ref, h.backend_id);
        }
      });
    }
  }
  h.fabric->set_event_sink(nullptr);
  PF_CHECK(sink.calls_.load() >= 120);
}

PF_TEST(concurrency, pending_attempt_bound_is_enforced) {
  LoopbackBackend::Options options{};
  options.stall_apply = true;
  HarnessOptions harness_options{};
  harness_options.max_pending_attempts = 2;
  Harness h{{}, 7, harness_options};
  make_ready(h, 10'000'000'000ull, 1'000'000'000ull, options);

  const PacingEnvelope first = h.derive(h.flow);
  const PacingEnvelope second = h.derive(h.flow);
  const PacingEnvelope third = h.derive(h.flow);
  StatusOr<AttemptId> a = Status::of(ErrorCode::Internal, "not run");
  StatusOr<AttemptId> b = Status::of(ErrorCode::Internal, "not run");
  {
    // Both pending slots are occupied by attempts parked in the backend.
    std::jthread t1([&] { a = h.fabric->apply_envelope(first.ref, h.backend_id); });
    std::jthread t2([&] { b = h.fabric->apply_envelope(second.ref, h.backend_id); });
    h.backend->wait_until_stalled(2);

    auto rejected = h.fabric->apply_envelope(third.ref, h.backend_id);
    PF_CHECK_CODE(rejected, ErrorCode::ResourceExhausted);
    PF_CHECK_EQ(h.backend->apply_entries(), 2ull);

    h.backend->release_stall();
  }
  PF_CHECK_STATUS_OK(a);
  PF_CHECK_STATUS_OK(b);
  PF_CHECK_EQ(h.fabric->attempts_for_flow(h.flow).size(), std::size_t{2});
}
