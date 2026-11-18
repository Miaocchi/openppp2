#define BOOST_TEST_MODULE multipath_scheduler_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/multipath/Scheduler.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace mp = ppp::app::multipath;
using Scheduler = mp::PathScheduler;
using Candidate = Scheduler::Candidate;

namespace {
bool IsStatus(Scheduler::Status actual, Scheduler::Status expected) {
    return static_cast<unsigned>(actual) == static_cast<unsigned>(expected);
}

Candidate Measured(std::uint16_t id, std::uint64_t rtt_ms,
                   std::uint64_t throughput, std::uint64_t now_ms) {
    Candidate c;
    c.id = id;
    c.eligible = true;
    c.send_credit_bytes = 1024 * 1024;
    c.rtt_ms = rtt_ms;
    c.throughput_bytes_per_second = throughput;
    c.last_active_ms = now_ms;
    c.metrics_updated_ms = now_ms;
    return c;
}
}

BOOST_AUTO_TEST_CASE(adaptive_prefers_lower_arrival_time_across_heterogeneous_rtts) {
    Scheduler scheduler;
    std::vector<Candidate> candidates{
        Measured(1, 5, 10 * 1024 * 1024, 1000),
        Measured(2, 80, 10 * 1024 * 1024, 1000),
        Measured(3, 200, 10 * 1024 * 1024, 1000),
    };
    const auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1200);
    BOOST_TEST(IsStatus(selected.status, Scheduler::Status::Selected));
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
}

BOOST_AUTO_TEST_CASE(adaptive_accounts_for_rate_instead_of_rtt_alone) {
    Scheduler scheduler;
    std::vector<Candidate> candidates{
        Measured(1, 5, 32 * 1024, 2000),
        Measured(2, 80, 1024 * 1024, 2000),
    };
    const auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 128 * 1024);
    BOOST_TEST(selected.carrier_id == 2);
}

BOOST_AUTO_TEST_CASE(unknown_and_stale_measurements_get_conservative_nonzero_costs) {
    Scheduler scheduler;
    Candidate unknown;
    unknown.id = 1; unknown.eligible = true; unknown.send_credit_bytes = 32 * 1024 * 1024;
    Candidate fresh = Measured(2, 200, 1024 * 1024, 10000);
    std::vector<Candidate> candidates{unknown, fresh};
    auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 10000, 4096);
    BOOST_TEST(selected.carrier_id == 2);

    scheduler.Reset();
    candidates[0] = Measured(1, 1, 1024 * 1024, 1000);
    candidates[1] = Measured(2, 200, 1024 * 1024, 10000);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 10000, 4096);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.estimated_arrival_ms > 0);
}

BOOST_AUTO_TEST_CASE(adaptive_load_arithmetic_saturates_without_wrapping) {
    Scheduler scheduler;
    Candidate overloaded = Measured(1, 1, std::numeric_limits<std::uint64_t>::max(), 1000);
    overloaded.queue_bytes = std::numeric_limits<std::uint64_t>::max();
    overloaded.unacked_bytes = std::numeric_limits<std::uint64_t>::max();
    Candidate clear = Measured(2, 100, 1024 * 1024, 1000);
    std::vector<Candidate> candidates{overloaded, clear};
    const auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1);
    BOOST_TEST(selected.carrier_id == 2);
}

BOOST_AUTO_TEST_CASE(failover_keeps_preferred_under_backlog_and_blocks_on_its_credit) {
    Scheduler scheduler;
    Candidate preferred = Measured(9, 5, 10 * 1024 * 1024, 5000);
    preferred.preferred = true;
    preferred.queue_bytes = 8 * 1024 * 1024;
    Candidate backup = Measured(2, 80, 10 * 1024 * 1024, 5000);
    backup.queue_bytes = 0;
    std::vector<Candidate> candidates{backup, preferred};
    auto selected = scheduler.Select(Scheduler::Policy::Failover, candidates, 5000, 1200);
    BOOST_TEST(IsStatus(selected.status, Scheduler::Status::Selected));
    BOOST_TEST(selected.carrier_id == 9);

    preferred.send_credit_bytes.reset();
    candidates[1] = preferred;
    selected = scheduler.Select(Scheduler::Policy::Failover, candidates, 5001, 1200);
    BOOST_TEST(IsStatus(selected.status, Scheduler::Status::Backpressure));

    preferred.eligible = false;
    candidates[1] = preferred;
    selected = scheduler.Select(Scheduler::Policy::Failover, candidates, 5002, 1200);
    BOOST_TEST(IsStatus(selected.status, Scheduler::Status::Selected));
    BOOST_TEST(selected.carrier_id == 2);
}

BOOST_AUTO_TEST_CASE(no_known_credit_is_backpressure_not_an_assumed_send_budget) {
    Scheduler scheduler;
    Candidate candidate = Measured(1, 5, 1024 * 1024, 1000);
    candidate.send_credit_bytes.reset();
    const auto selected = scheduler.Select(Scheduler::Policy::Adaptive,
        std::vector<Candidate>{candidate}, 1000, 1);
    BOOST_TEST(IsStatus(selected.status, Scheduler::Status::Backpressure));
}

BOOST_AUTO_TEST_CASE(adaptive_reserves_low_rate_probes_at_a_bounded_interval) {
    Scheduler scheduler(1000);
    std::vector<Candidate> candidates{
        Measured(1, 5, 10 * 1024 * 1024, 1000),
        Measured(2, 200, 10 * 1024 * 1024, 1000),
    };
    auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1200);
    BOOST_TEST(selected.carrier_id == 1);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1999, 1200);
    BOOST_TEST(selected.carrier_id == 1);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1200);
    BOOST_TEST(IsStatus(selected.status, Scheduler::Status::Selected));
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2500, 1200);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
}

BOOST_AUTO_TEST_CASE(candidate_count_and_duplicate_ids_are_rejected) {
    Scheduler scheduler;
    Candidate item = Measured(1, 5, 1000, 1);
    BOOST_TEST(IsStatus(scheduler.Select(Scheduler::Policy::Adaptive,
        std::vector<Candidate>(Scheduler::MaxCandidates + 1, item), 1, 1).status,
        Scheduler::Status::InvalidInput));
    BOOST_TEST(IsStatus(scheduler.Select(Scheduler::Policy::Adaptive,
        std::vector<Candidate>{item, item}, 1, 1).status, Scheduler::Status::InvalidInput));
}

BOOST_AUTO_TEST_CASE(failover_without_preference_does_not_overflow_to_a_better_credited_path) {
    Scheduler scheduler;
    Candidate first = Measured(3, 5, 10 * 1024 * 1024, 7000);
    first.send_credit_bytes = 0;
    Candidate second = Measured(7, 80, 10 * 1024 * 1024, 7000);
    std::vector<Candidate> candidates{second, first};
    auto selected = scheduler.Select(Scheduler::Policy::Failover, candidates, 7000, 1200);
    BOOST_TEST(IsStatus(selected.status, Scheduler::Status::Backpressure));
    first.eligible = false;
    candidates[1] = first;
    selected = scheduler.Select(Scheduler::Policy::Failover, candidates, 7001, 1200);
    BOOST_TEST(IsStatus(selected.status, Scheduler::Status::Selected));
    BOOST_TEST(selected.carrier_id == 7);

    first.eligible = true;
    first.send_credit_bytes = 1200;
    candidates[1] = first;
    selected = scheduler.Select(Scheduler::Policy::Failover, candidates, 7002, 1200);
    BOOST_TEST(IsStatus(selected.status, Scheduler::Status::Selected));
    BOOST_TEST(selected.carrier_id == 3);
}

BOOST_AUTO_TEST_CASE(adaptive_tracks_dynamic_carriers_and_reinitializes_reused_ids) {
    Scheduler scheduler(1000);
    Candidate fast = Measured(1, 5, 10 * 1024 * 1024, 10000);
    Candidate slow = Measured(2, 200, 10 * 1024 * 1024, 10000);
    auto selected = scheduler.Select(Scheduler::Policy::Adaptive,
        std::vector<Candidate>{fast, slow}, 10000, 1000);
    BOOST_TEST(selected.carrier_id == 1);

    Candidate added = Measured(3, 250, 10 * 1024 * 1024, 10999);
    selected = scheduler.Select(Scheduler::Policy::Adaptive,
        std::vector<Candidate>{fast, slow, added}, 10999, 1000);
    BOOST_TEST(selected.carrier_id == 1);
    selected = scheduler.Select(Scheduler::Policy::Adaptive,
        std::vector<Candidate>{fast, slow, added}, 11000, 1000);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);

    selected = scheduler.Select(Scheduler::Policy::Adaptive,
        std::vector<Candidate>{fast}, 12000, 1000);
    BOOST_TEST(selected.carrier_id == 1);
    Candidate reused = Measured(2, 1200, 10 * 1024 * 1024, 13000);
    selected = scheduler.Select(Scheduler::Policy::Adaptive,
        std::vector<Candidate>{fast, reused}, 13000, 1000);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
    selected = scheduler.Select(Scheduler::Policy::Adaptive,
        std::vector<Candidate>{fast, reused}, 14000, 1000);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);
}

BOOST_AUTO_TEST_CASE(probe_respects_state_and_explicit_budget) {
    Scheduler scheduler(1000);
    Candidate fast = Measured(1, 5, 10 * 1024 * 1024, 1000);
    Candidate degraded = Measured(2, 200, 10 * 1024 * 1024, 1000);
    degraded.state = Scheduler::CandidateState::Degraded;
    degraded.probe_eligible = true;
    std::vector<Candidate> candidates{fast, degraded};

    auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1200);
    BOOST_TEST(selected.carrier_id == 1);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1200);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);

    degraded.state = Scheduler::CandidateState::Warming;
    degraded.probe_budget_bytes = 1000;
    candidates[1] = degraded;
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 3000, 1200);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);

    degraded.probe_budget_bytes = 1200;
    candidates[1] = degraded;
    scheduler.Reset();
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 3000, 1200);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 4000, 1200);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);
}

BOOST_AUTO_TEST_CASE(draining_candidate_is_not_selected_but_legacy_active_default_is) {
    Scheduler scheduler;
    Candidate draining = Measured(1, 5, 10 * 1024 * 1024, 1000);
    draining.state = Scheduler::CandidateState::Draining;
    Candidate fallback = Measured(2, 50, 10 * 1024 * 1024, 1000);
    auto selected = scheduler.Select(Scheduler::Policy::Failover,
        std::vector<Candidate>{draining, fallback}, 1000, 1200);
    BOOST_TEST(IsStatus(selected.status, Scheduler::Status::Selected));
    BOOST_TEST(selected.carrier_id == 2);

    Candidate legacy = Measured(3, 5, 10 * 1024 * 1024, 1000);
    selected = scheduler.Select(Scheduler::Policy::Failover,
        std::vector<Candidate>{legacy}, 1000, 1200);
    BOOST_TEST(selected.carrier_id == 3);
}

BOOST_AUTO_TEST_CASE(snapshot_projection_preserves_state_and_capability_gates) {
    mp::PathControlSnapshot snapshot;
    snapshot.state = mp::PathState::Active;
    snapshot.telemetry.has_sample = true;
    snapshot.telemetry.updated_ms = 5000;
    snapshot.telemetry.confidence = 80;
    snapshot.telemetry.min_rtt_ms = 12;
    snapshot.telemetry.current_rate_bytes_per_second = 9000;
    snapshot.telemetry.local_queue_bytes = 17;
    snapshot.telemetry.wire_inflight_bytes = 23;
    snapshot.capabilities.active_probe = true;
    snapshot.capabilities.payload_delivery_rate = true;

    const Candidate candidate = Scheduler::ProjectCandidate(7, snapshot, 5001, 4096, true);
    BOOST_TEST(candidate.id == 7);
    BOOST_TEST(candidate.eligible);
    BOOST_TEST(candidate.preferred);
    BOOST_TEST(static_cast<unsigned>(candidate.state) ==
        static_cast<unsigned>(Scheduler::CandidateState::Active));
    BOOST_TEST(candidate.probe_eligible);
    BOOST_TEST(*candidate.rtt_ms == 12);
    BOOST_TEST(*candidate.throughput_bytes_per_second == 9000);
    BOOST_TEST(candidate.queue_bytes == 17);
    BOOST_TEST(candidate.unacked_bytes == 23);
}

BOOST_AUTO_TEST_CASE(snapshot_projection_discards_stale_or_low_confidence_metrics) {
    mp::PathControlSnapshot snapshot;
    snapshot.state = mp::PathState::Degraded;
    snapshot.telemetry.has_sample = true;
    snapshot.telemetry.updated_ms = 1;
    snapshot.telemetry.confidence = 100;
    snapshot.telemetry.min_rtt_ms = 1;
    snapshot.telemetry.current_rate_bytes_per_second = 1000000;
    snapshot.capabilities.payload_delivery_rate = true;
    snapshot.capabilities.active_probe = false;

    auto candidate = Scheduler::CandidateFromSnapshot(3, snapshot, 10000);
    BOOST_TEST(candidate.eligible);
    BOOST_TEST(static_cast<unsigned>(candidate.state) ==
        static_cast<unsigned>(Scheduler::CandidateState::Degraded));
    BOOST_TEST(!candidate.rtt_ms);
    BOOST_TEST(!candidate.throughput_bytes_per_second);
    BOOST_TEST(!candidate.probe_eligible);

    snapshot.telemetry.updated_ms = 10000;
    snapshot.telemetry.confidence = 0;
    candidate = Scheduler::ProjectCandidate(3, snapshot, 10000);
    BOOST_TEST(!candidate.rtt_ms);
}

BOOST_AUTO_TEST_CASE(snapshot_projection_blocks_non_running_states) {
    mp::PathControlSnapshot snapshot;
    snapshot.state = mp::PathState::Backoff;
    snapshot.capabilities.active_probe = true;
    const auto candidate = Scheduler::ProjectCandidate(4, snapshot, 100);
    BOOST_TEST(!candidate.eligible);
    BOOST_TEST(static_cast<unsigned>(candidate.state) ==
        static_cast<unsigned>(Scheduler::CandidateState::Unavailable));
    BOOST_TEST(!candidate.probe_eligible);
}
