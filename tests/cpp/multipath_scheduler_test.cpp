#define BOOST_TEST_MODULE multipath_scheduler_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/multipath/Scheduler.h>
#include <ppp/app/multipath/RuntimeState.h>

#include <algorithm>
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

BOOST_AUTO_TEST_CASE(credit_fallback_is_counted_when_all_preferred_carriers_are_exhausted) {
    Scheduler scheduler;
    mp::IntentRuntimeStats stats;
    std::vector<Candidate> candidates{
        Measured(1, 50, 1024 * 1024, 1000),
        Measured(2, 50, 1024 * 1024, 1000),
        Measured(3, 155, 1024 * 1024, 1000),
    };
    candidates[0].send_credit_bytes = 0;
    candidates[1].send_credit_bytes = 0;
    candidates[2].eligible = false;
    const auto preferred = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1200);
    BOOST_TEST(IsStatus(preferred.status, Scheduler::Status::Backpressure));
    BOOST_TEST(!stats.RecordCreditFallback(true, false));
    BOOST_TEST(stats.credit_fallback_count == 0u);

    candidates[0].eligible = false;
    candidates[1].eligible = false;
    candidates[2].eligible = true;
    const auto alternate = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1200);
    BOOST_TEST(IsStatus(alternate.status, Scheduler::Status::Selected));
    BOOST_TEST(alternate.carrier_id == 3u);
    BOOST_TEST(stats.RecordCreditFallback(
        preferred.status == Scheduler::Status::Backpressure,
        alternate.status == Scheduler::Status::Selected));
    BOOST_TEST(!stats.RecordCreditFallback(false, true));
    BOOST_TEST(stats.credit_fallback_count == 1u);
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

BOOST_AUTO_TEST_CASE(adaptive_flight_credit_bootstraps_and_bounds_each_carrier) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t frame = 65535;
    constexpr std::uint64_t high_rtt_bdp = 6750000;
    constexpr std::uint64_t share = window / 3;
    // A known BDP targets the BDP plus derived headroom, clamped by the share.
    constexpr std::uint64_t expected_target =
        std::min(share, high_rtt_bdp + high_rtt_bdp / Scheduler::FlightHeadroomDivisor);
    // An unknown BDP targets the startup window. Weighting alone would grant
    // half of it, but the startup floor holds the full window.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, 0, 0, 50, startup, frame) ==
        startup);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, high_rtt_bdp, 0, 100, startup, frame) ==
        expected_target);
    // The derived headroom is a discovery floor: a flight already at the BDP
    // still gets the headroom bytes, under full or reduced weight.
    const auto bdp_flight_credit = expected_target - high_rtt_bdp;
    BOOST_TEST(bdp_flight_credit > 0u);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, high_rtt_bdp, high_rtt_bdp, 100, startup, frame) ==
        bdp_flight_credit);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, high_rtt_bdp, high_rtt_bdp, 25, startup, frame) ==
        bdp_flight_credit);
    // At or beyond the target there is no spare credit.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, high_rtt_bdp, expected_target, 100, startup, frame) ==
        0u);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, high_rtt_bdp, share, 100, startup, frame) ==
        0u);
}

BOOST_AUTO_TEST_CASE(adaptive_flight_credit_never_targets_below_known_bdp) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t frame = 65535;
    constexpr std::uint64_t bdp = 2 * 1024 * 1024;
    constexpr std::uint64_t share = window / 3;
    constexpr std::uint64_t expected_target =
        std::min(share, bdp + bdp / Scheduler::FlightHeadroomDivisor);
    // Weighting may share a carrier down, but the target never falls below the
    // BDP plus derived headroom, so an already-full flight retains the headroom
    // as spare credit.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, 0, 25, startup, frame) ==
        expected_target);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, bdp, 25, startup, frame) ==
        expected_target - bdp);
    // Full weight grants the same target, and a BDP above the carrier's session
    // share is still clamped to that share.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, 0, 100, startup, frame) ==
        expected_target);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, bdp, 100, startup, frame) ==
        expected_target - bdp);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, window, 0, 100, startup, frame) ==
        window / 3);
    // A known BDP smaller than the startup window is floored at the startup
    // window: the weighted target alone would grant only half of it.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, 1024, 0, 50, startup, frame) ==
        startup);
    // An unknown BDP floors at the startup window, not the minimum frame
    // window, even at minimal weight.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, 0, 0, 1, startup, frame) == startup);
    // Saturating headroom cannot wrap, and the session share still bounds it.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3,
        std::numeric_limits<std::uint64_t>::max(), 0, 100, startup, frame) == window / 3);
}

BOOST_AUTO_TEST_CASE(adaptive_flight_credit_tiny_known_bdp_still_permits_multiple_frames) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t frame = 65535;
    constexpr std::uint64_t share = window / 3;
    // A low-capacity carrier measures roughly one 64 KiB frame of BDP (r9
    // path1: ~0.30 MB/s at the path RTT). The startup floor must still grant
    // several frames; at one frame of credit the delivery sample can never
    // exceed the current estimate, so the estimator locks low forever.
    constexpr std::uint64_t tiny_bdp = frame;
    constexpr std::uint64_t floor_credit = startup;
    static_assert(floor_credit / frame >= 3, "startup floor must permit multiple frames");
    static_assert(floor_credit > tiny_bdp + tiny_bdp / Scheduler::FlightHeadroomDivisor,
        "startup floor must exceed the tiny BDP plus headroom");
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, tiny_bdp, 0, 100, startup, frame) ==
        floor_credit);
    // Weighted sharing may not shrink the floor; the old floor (BDP plus
    // headroom) left only ~one frame here.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, tiny_bdp, 0, 25, startup, frame) ==
        floor_credit);
    // One frame already in flight still leaves three more frames of credit.
    const auto inflight_credit = Scheduler::AdaptiveFlightCredit(
        window, 3, tiny_bdp, tiny_bdp, 25, startup, frame);
    BOOST_TEST(inflight_credit == floor_credit - tiny_bdp);
    BOOST_TEST(inflight_credit / frame >= 3);
    // The floor is also the target: no over-credit at or beyond it.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, tiny_bdp, floor_credit, 100,
        startup, frame) == 0u);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, tiny_bdp, share, 100,
        startup, frame) == 0u);
    // The session share still clamps a startup window larger than the share.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, tiny_bdp, 0, 100, share * 2, frame) ==
        share);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, tiny_bdp, 0, 100, window, frame) ==
        share);
}

BOOST_AUTO_TEST_CASE(adaptive_flight_credit_high_bdp_is_independent_of_startup_floor) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t frame = 65535;
    constexpr std::uint64_t share = window / 3;
    constexpr std::uint64_t high_rtt_bdp = 6750000;
    constexpr std::uint64_t expected_target =
        std::min(share, high_rtt_bdp + high_rtt_bdp / Scheduler::FlightHeadroomDivisor);
    // Above the startup window the floor is unchanged: BDP plus headroom, and
    // the startup window no longer influences the credit at all.
    constexpr std::uint64_t startup_windows[] = {0, 8 * 1024, 256 * 1024, 3 * 1024 * 1024};
    for (const auto startup : startup_windows) {
        BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, high_rtt_bdp, 0, 100,
            startup, frame) == expected_target);
        BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, high_rtt_bdp, high_rtt_bdp, 25,
            startup, frame) == expected_target - high_rtt_bdp);
    }
    // A mid-size BDP is dominated by its own headroom as well.
    constexpr std::uint64_t bdp = 2 * 1024 * 1024;
    constexpr std::uint64_t bdp_target =
        std::min(share, bdp + bdp / Scheduler::FlightHeadroomDivisor);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, 0, 100, 3 * 1024 * 1024, frame) ==
        bdp_target);
    // Only a startup window larger than BDP plus headroom raises the target,
    // and the session share still clamps it.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, 0, 100, 8 * 1024 * 1024, frame) ==
        8 * 1024 * 1024);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, 0, 100, window, frame) == share);
}

BOOST_AUTO_TEST_CASE(adaptive_flight_credit_discovery_allowance_stays_probe_and_share_bounded) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t share = window / 3;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t frame = 65535;
    constexpr std::uint64_t bdp = 2 * 1024 * 1024;
    constexpr std::uint64_t probe_budget = 16 * 1024 * 1024;
    constexpr std::uint64_t normal_target = bdp + bdp / Scheduler::FlightHeadroomDivisor;
    static_assert(normal_target < share, "test needs the share to be the discovery ceiling");
    // A mature path at its target still gets no credit.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, normal_target, 100,
        startup, frame) == 0u);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, normal_target, 100,
        startup, frame, 0) == 0u);
    // The probe allowance extends the target to the carrier's session share, so
    // a capacity probe is not capped by the provisional BDP it is testing.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, 0, 100, startup, frame,
        probe_budget) == share);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, normal_target, 100,
        startup, frame, probe_budget) == share - normal_target);
    // A smaller live remainder only lifts the ceiling by that remainder, so the
    // burst spending down also withdraws the extra flight.
    constexpr std::uint64_t live_remainder = 1024 * 1024;
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, normal_target, 100,
        startup, frame, live_remainder) == live_remainder);
    // The allowance is never granted beyond the session share.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, share, 100,
        startup, frame, probe_budget) == 0u);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, 0, 100, startup, frame,
        std::numeric_limits<std::uint64_t>::max()) == share);
    // An unknown BDP has no provisional target to escape; the startup floor and
    // the session share keep the legacy behavior even when a caller passes a
    // probe allowance.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, 0, 0, 50, startup, frame,
        probe_budget) == startup);
    // A tiny known BDP keeps the startup floor and then adds the allowance.
    constexpr std::uint64_t tiny_bdp = frame;
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, tiny_bdp, 0, 100,
        startup, frame, probe_budget) == share);
}

namespace {
using CreditAllocation = Scheduler::FlightCreditAllocation;
using CreditRequest = Scheduler::FlightCreditRequest;

CreditRequest KnownCreditRequest(std::uint16_t id, std::uint64_t bdp,
                                 std::uint64_t inflight = 0,
                                 std::uint64_t discovery = 0) {
    CreditRequest request;
    request.id = id;
    request.eligible = true;
    request.capacity_known = true;
    request.bdp_bytes = bdp;
    request.inflight_bytes = inflight;
    request.discovery_budget_bytes = discovery;
    return request;
}

const CreditAllocation* FindAllocation(
    const std::vector<CreditAllocation>& allocations, std::uint16_t id) {
    const auto found = std::find_if(allocations.begin(), allocations.end(),
        [id](const CreditAllocation& allocation) {
            return allocation.id == id;
        });
    return found == allocations.end() ? nullptr : &*found;
}
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_returns_idle_share_to_high_demand_carrier) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t minimum = 65535;
    constexpr std::uint64_t low_bdp = 512 * 1024;
    constexpr std::uint64_t high_bdp = 16 * 1024 * 1024;
    const auto low_demand = Scheduler::FlightDemandBytes(
        low_bdp, true, startup, minimum);
    const auto high_demand = Scheduler::FlightDemandBytes(
        high_bdp, true, startup, minimum);
    // No measured RTT pair means one BDP of headroom on top of the BDP.
    BOOST_TEST(low_demand == 2 * low_bdp);
    BOOST_TEST(high_demand == 2 * high_bdp);
    BOOST_TEST(high_demand == window);

    const std::vector<CreditRequest> requests{
        KnownCreditRequest(1, low_bdp),
        KnownCreditRequest(2, low_bdp),
        KnownCreditRequest(3, high_bdp),
    };
    std::vector<CreditAllocation> allocations(requests.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) == requests.size());

    const auto* low1 = FindAllocation(allocations, 1);
    const auto* low2 = FindAllocation(allocations, 2);
    const auto* high = FindAllocation(allocations, 3);
    BOOST_REQUIRE(low1 && low2 && high);

    // r49 path3 was capped at window / 3 == ~11.14 MiB even though path1 and
    // path2 could not fill their equal shares. The borrow plan must return
    // that idle capacity to the only carrier with demand for it.
    BOOST_TEST(high->target_bytes > window / 3);
    BOOST_TEST(high->target_bytes <= high->demand_bytes);
    BOOST_TEST(high->target_bytes == allocations[2].target_bytes);
    BOOST_TEST(high->demand_bytes == high_demand);
    // The low-BDP paths still receive capacity, but never more than their
    // measured demand.
    BOOST_TEST(low1->target_bytes > 0);
    BOOST_TEST(low1->target_bytes <= low1->demand_bytes);
    BOOST_TEST(low2->target_bytes <= low2->demand_bytes);
    BOOST_TEST(low1->target_bytes == low2->target_bytes);
    const auto total = low1->target_bytes + low2->target_bytes +
        high->target_bytes;
    BOOST_TEST(total <= window);
    // With no flight outstanding the full target is sendable credit.
    BOOST_TEST(high->credit_bytes == high->target_bytes);
    BOOST_TEST(high->discovery_bytes == 0u);
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_path3_borrows_only_idle_peer_share) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t minimum = 65535;
    constexpr std::uint64_t low_bdp = 1024 * 1024;
    constexpr std::uint64_t high_bdp = 16 * 1024 * 1024;
    const auto share = window / 3;
    const auto low_demand = Scheduler::FlightDemandBytes(
        low_bdp, true, startup, minimum);
    const auto high_demand = Scheduler::FlightDemandBytes(
        high_bdp, true, startup, minimum);
    BOOST_TEST(low_demand == 2 * low_bdp);
    BOOST_TEST(high_demand == 2 * high_bdp);
    BOOST_TEST(high_demand == window);

    std::vector<CreditRequest> requests{
        KnownCreditRequest(1, low_bdp),
        KnownCreditRequest(2, low_bdp),
        KnownCreditRequest(3, high_bdp),
    };
    std::vector<CreditAllocation> allocations(requests.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) == requests.size());
    const auto* low1 = FindAllocation(allocations, 1);
    const auto* low2 = FindAllocation(allocations, 2);
    const auto* high = FindAllocation(allocations, 3);
    BOOST_REQUIRE(low1 && low2 && high);

    // The high-demand carrier starts at the equal share and borrows only the
    // idle parts of its peers; the borrowed amount is capped by the remaining
    // session window (the low-demand peers keep their measured targets).
    BOOST_TEST(high->share_bytes == share);
    BOOST_TEST(high->target_bytes == share + high->borrow_bytes);
    BOOST_TEST(high->borrow_bytes > 0u);
    BOOST_TEST(high->borrow_bytes <= 2 * share);
    BOOST_TEST(high->target_bytes <= high_demand);
    BOOST_TEST(low1->borrow_bytes == 0u);
    BOOST_TEST(low2->borrow_bytes == 0u);
    BOOST_TEST(low1->target_bytes == low_demand);
    BOOST_TEST(low2->target_bytes == low_demand);
    BOOST_TEST(low1->target_bytes + low2->target_bytes +
        high->target_bytes <= window);

    // Once a peer fills its share with outstanding flight, that share is no
    // longer idle and cannot be lent. Only the other peer's share remains.
    requests[1].inflight_bytes = share;
    allocations.assign(requests.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) == requests.size());
    high = FindAllocation(allocations, 3);
    BOOST_REQUIRE(high);
    // The full peer still holds more flight than its reduced target. Reserve
    // that overhang as well as the idle peer's target before granting a borrow.
    const auto borrow_with_overhang = window - 2 * share - low_demand;
    BOOST_TEST(high->borrow_bytes == borrow_with_overhang);
    BOOST_TEST(high->target_bytes == share + borrow_with_overhang);
    BOOST_TEST(requests[1].inflight_bytes + low_demand +
        high->target_bytes == window);

    // With both peers at their share there is no unused share to borrow, even
    // though the high-demand carrier still wants more flight.
    requests[0].inflight_bytes = share;
    allocations.assign(requests.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) == requests.size());
    high = FindAllocation(allocations, 3);
    BOOST_REQUIRE(high);
    BOOST_TEST(high->borrow_bytes == 0u);
    BOOST_TEST(high->target_bytes == share);

    // The production gate marks a carrier that is still filling its
    // provisional BDP (or has no local credit) as not borrow-eligible. Its
    // measured demand must not reserve idle peer share it would never apply.
    requests[0].inflight_bytes = 0;
    requests[1].inflight_bytes = 0;
    requests[2].borrow_eligible = false;
    allocations.assign(requests.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) == requests.size());
    high = FindAllocation(allocations, 3);
    BOOST_REQUIRE(high);
    BOOST_TEST(high->borrow_bytes == 0u);
    BOOST_TEST(high->target_bytes == share);
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_reserves_overhang_before_discovery_and_borrow) {
    constexpr std::uint64_t mib = 1024 * 1024;
    constexpr std::uint64_t window = 32 * mib;
    std::vector<CreditRequest> requests{
        KnownCreditRequest(1, mib, 12 * mib),
        KnownCreditRequest(2, mib, 0, mib),
        KnownCreditRequest(3, 16 * mib),
    };
    std::vector<CreditAllocation> allocations(requests.size());
    const auto build = [&]() {
        BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
            requests.size(), 256 * 1024, 65535, allocations.data()) == requests.size());
    };
    const auto resident_and_credit = [&]() {
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            total += requests[i].inflight_bytes + allocations[i].credit_bytes;
        }
        return total;
    };
    build();
    BOOST_TEST(allocations[0].target_bytes == 2 * mib);
    BOOST_TEST(allocations[0].credit_bytes == 0u);
    BOOST_TEST(allocations[1].discovery_bytes == mib);
    BOOST_TEST(allocations[2].target_bytes == 17 * mib);
    // Reserving targets alone would advertise 42 MiB of resident+sendable
    // flight; the session gate would then park flows despite carrier credit.
    BOOST_TEST(resident_and_credit() == window);

    // ACKs, not a capacity estimate decrease, return the overhang. Each newly
    // released byte can now become borrower credit without crossing the cap.
    requests[0].inflight_bytes -= mib;
    build();
    BOOST_TEST(allocations[2].target_bytes == 18 * mib);
    BOOST_TEST(resident_and_credit() == window);
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_reserves_ineligible_carrier_flight) {
    constexpr std::uint64_t mib = 1024 * 1024;
    constexpr std::uint64_t window = 32 * mib;
    auto draining = KnownCreditRequest(3, 64 * mib, 8 * mib, mib);
    draining.eligible = false;
    std::vector<CreditRequest> requests{
        KnownCreditRequest(1, mib),
        KnownCreditRequest(2, 16 * mib),
        draining,
    };
    std::vector<CreditAllocation> allocations(requests.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), 256 * 1024, 65535, allocations.data()) == requests.size());
    BOOST_TEST(allocations[2].target_bytes == 0u);
    BOOST_TEST(allocations[2].discovery_bytes == 0u);
    BOOST_TEST(allocations[2].credit_bytes == 0u);
    BOOST_TEST(allocations[1].target_bytes == 22 * mib);
    BOOST_TEST(requests[2].inflight_bytes + allocations[0].credit_bytes +
        allocations[1].credit_bytes == window);

    // An already exhausted session cannot fund discovery or borrowed flight,
    // even when an ineligible carrier reports an extreme outstanding count.
    requests[2].inflight_bytes = std::numeric_limits<std::uint64_t>::max();
    requests[0].discovery_budget_bytes = mib;
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), 256 * 1024, 65535, allocations.data()) == requests.size());
    BOOST_TEST(allocations[0].discovery_bytes == 0u);
    BOOST_TEST(allocations[1].borrow_bytes == 0u);
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_total_never_exceeds_session_window) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t minimum = 65535;
    constexpr std::uint64_t low_bdp = 512 * 1024;
    constexpr std::uint64_t high_bdp = 8 * 1024 * 1024;
    const auto share = window / 3;
    const auto low_demand = Scheduler::FlightDemandBytes(
        low_bdp, true, startup, minimum);
    const auto high_demand = Scheduler::FlightDemandBytes(
        high_bdp, true, startup, minimum);

    // Two high-demand carriers compete for the same idle share. The first
    // borrower is funded to its full demand and the second takes the window
    // remainder, but the plan total still lands exactly on the cap.
    std::vector<CreditRequest> requests{
        KnownCreditRequest(1, low_bdp),
        KnownCreditRequest(2, high_bdp),
        KnownCreditRequest(3, high_bdp),
    };
    std::vector<CreditAllocation> allocations(requests.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) == requests.size());
    const auto* low = FindAllocation(allocations, 1);
    const auto* first = FindAllocation(allocations, 2);
    const auto* second = FindAllocation(allocations, 3);
    BOOST_REQUIRE(low && first && second);
    BOOST_TEST(low->target_bytes == low_demand);
    BOOST_TEST(first->target_bytes == high_demand);
    BOOST_TEST(second->target_bytes > share);
    BOOST_TEST(second->target_bytes < high_demand);
    BOOST_TEST(low->target_bytes + first->target_bytes +
        second->target_bytes <= window);
    BOOST_TEST(low->target_bytes + first->target_bytes +
        second->target_bytes == window);
    std::uint64_t total_borrow = 0;
    for (const auto& allocation : allocations) {
        BOOST_TEST(allocation.target_bytes <= allocation.demand_bytes);
        total_borrow += allocation.borrow_bytes;
    }
    // Lenders expose exactly the idle part of their shares; the borrowers can
    // never claim more than that pool even when their combined demand is huge.
    BOOST_TEST(total_borrow <= 3 * share);

    // When every carrier's demand exceeds the session window, the equal-share
    // base already consumes the cap. Borrowing is therefore limited to the
    // integer remainder of the window division (window % live), not forbidden:
    // the allocator hands that leftover window capacity to a borrower, so the
    // plan still lands exactly on the cap.
    const std::vector<CreditRequest> saturated{
        KnownCreditRequest(1, 64 * 1024 * 1024),
        KnownCreditRequest(2, 64 * 1024 * 1024),
        KnownCreditRequest(3, 64 * 1024 * 1024),
    };
    allocations.assign(saturated.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, saturated.data(),
        saturated.size(), startup, minimum, allocations.data()) ==
        saturated.size());
    const auto saturated_remainder = window % saturated.size();
    std::uint64_t saturated_total = 0;
    std::uint64_t saturated_borrow = 0;
    for (const auto& allocation : allocations) {
        BOOST_TEST(allocation.target_bytes <= allocation.demand_bytes);
        saturated_total += allocation.target_bytes;
        saturated_borrow += allocation.borrow_bytes;
    }
    BOOST_TEST(saturated_total <= window);
    BOOST_TEST(saturated_total >= window - saturated_remainder);
    // Every carrier is idle, so the whole equal share of each is lendable.
    // Borrowing may never exceed that idle-share pool, and because the base
    // already covers live * share only the division remainder can be moved.
    BOOST_TEST(saturated_borrow <= 3 * share);
    BOOST_TEST(saturated_borrow <= saturated_remainder);
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_unknown_or_stale_capacity_never_borrows) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t minimum = 65535;
    constexpr std::uint64_t low_bdp = 1024 * 1024;
    const auto share = window / 3;
    const auto low_demand = Scheduler::FlightDemandBytes(
        low_bdp, true, startup, minimum);

    CreditRequest unknown;
    unknown.id = 2;
    unknown.eligible = true;
    // A caller may leave a stale BDP populated; only capacity_known may
    // authorize demand above the equal share.
    unknown.capacity_known = false;
    unknown.bdp_bytes = 64 * 1024 * 1024;
    std::vector<CreditRequest> requests{
        KnownCreditRequest(1, low_bdp),
        unknown,
        KnownCreditRequest(3, 16 * 1024 * 1024),
    };
    std::vector<CreditAllocation> allocations(requests.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) == requests.size());
    const auto* unmeasured = FindAllocation(allocations, 2);
    const auto* high = FindAllocation(allocations, 3);
    const auto* low = FindAllocation(allocations, 1);
    BOOST_REQUIRE(unmeasured && high && low);
    BOOST_TEST(!unmeasured->capacity_known);
    BOOST_TEST(unmeasured->demand_bytes == share);
    BOOST_TEST(unmeasured->target_bytes == share);
    BOOST_TEST(unmeasured->borrow_bytes == 0u);
    // The unknown carrier still contributes its idle equal share to the
    // borrower pool; it just cannot demand more for itself.
    BOOST_TEST(high->borrow_bytes > 0u);
    BOOST_TEST(low->target_bytes == low_demand);

    // An expired capacity sample reaches the same fallback through the
    // production adapter: no BDP, equal demand, no borrow.
    constexpr std::uint64_t now = 10000;
    mp::PathTelemetry telemetry;
    telemetry.has_sample = true;
    telemetry.capacity_updated_ms = now - Scheduler::MetricsMaxAgeMs - 1;
    telemetry.capacity_rate_bytes_per_second = 8 * 1024 * 1024;
    telemetry.bdp_bytes = 64 * 1024 * 1024;
    telemetry.wire_inflight_bytes = 0;
    const auto stale = Scheduler::MakeFlightCreditRequest(2, telemetry, true, now);
    BOOST_TEST(!stale.capacity_known);
    const std::vector<CreditRequest> stale_requests{stale};
    allocations.assign(stale_requests.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, stale_requests.data(),
        stale_requests.size(), startup, minimum, allocations.data()) ==
        stale_requests.size());
    BOOST_TEST(allocations.front().demand_bytes == window);
    BOOST_TEST(allocations.front().borrow_bytes == 0u);
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_recomputes_on_carrier_join_and_leave) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t minimum = 65535;
    constexpr std::uint64_t low_bdp = 1024 * 1024;
    constexpr std::uint64_t high_bdp = 16 * 1024 * 1024;
    const auto high_demand = Scheduler::FlightDemandBytes(
        high_bdp, true, startup, minimum);

    // Alone the high-demand carrier already owns the whole session window and
    // has no peer share to borrow.
    std::vector<CreditRequest> alone{KnownCreditRequest(3, high_bdp)};
    std::vector<CreditAllocation> allocations(alone.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, alone.data(),
        alone.size(), startup, minimum, allocations.data()) == alone.size());
    BOOST_TEST(allocations.front().share_bytes == window);
    BOOST_TEST(allocations.front().borrow_bytes == 0u);
    BOOST_TEST(allocations.front().target_bytes == window);
    BOOST_TEST(allocations.front().target_bytes <= high_demand);

    // Joining a low-demand carrier lowers the equal share and makes that
    // carrier's idle share borrowable; joining a second one lowers it again.
    std::vector<CreditRequest> joined = alone;
    joined.insert(joined.begin(), KnownCreditRequest(1, low_bdp));
    allocations.assign(joined.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, joined.data(),
        joined.size(), startup, minimum, allocations.data()) == joined.size());
    auto* high = FindAllocation(allocations, 3);
    BOOST_REQUIRE(high);
    BOOST_TEST(high->share_bytes == window / 2);
    BOOST_TEST(high->borrow_bytes > 0u);
    BOOST_TEST(high->target_bytes == high->share_bytes + high->borrow_bytes);
    BOOST_TEST(high->target_bytes <= high_demand);

    joined.insert(joined.begin() + 1, KnownCreditRequest(2, low_bdp));
    allocations.assign(joined.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, joined.data(),
        joined.size(), startup, minimum, allocations.data()) == joined.size());
    high = FindAllocation(allocations, 3);
    BOOST_REQUIRE(high);
    BOOST_TEST(high->share_bytes == window / 3);
    std::uint64_t total = 0;
    for (const auto& allocation : allocations) {
        BOOST_TEST(allocation.target_bytes <= allocation.demand_bytes);
        total += allocation.target_bytes;
    }
    BOOST_TEST(total <= window);

    // Leaving again is stateless: the survivor returns to a two-carrier share
    // and the departed carrier's borrow disappears with it.
    std::vector<CreditRequest> left{joined.front(), joined.back()};
    allocations.assign(left.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, left.data(),
        left.size(), startup, minimum, allocations.data()) == left.size());
    high = FindAllocation(allocations, 3);
    BOOST_REQUIRE(high);
    BOOST_TEST(high->share_bytes == window / 2);
    BOOST_TEST(!FindAllocation(allocations, 2));
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_never_exceeds_the_session_window) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t minimum = 65535;
    constexpr std::uint64_t huge_bdp = 64 * 1024 * 1024;
    const std::vector<CreditRequest> requests{
        KnownCreditRequest(1, huge_bdp),
        KnownCreditRequest(2, huge_bdp),
        KnownCreditRequest(3, huge_bdp),
    };
    std::vector<CreditAllocation> allocations(requests.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) == requests.size());

    std::uint64_t total = 0;
    for (const auto& allocation : allocations) {
        BOOST_TEST(allocation.eligible);
        BOOST_TEST(allocation.target_bytes > 0);
        BOOST_TEST(allocation.target_bytes <= allocation.demand_bytes);
        total += allocation.target_bytes;
    }
    // Three equal huge demands split the session window evenly; the integer
    // floor may leave a couple of bytes unassigned but can never overrun.
    BOOST_TEST(total <= window);
    BOOST_TEST(total >= window - 2);

    // Outstanding flight is subtracted from the allocated target, never from
    // another carrier's share.
    auto with_flight = requests;
    with_flight[1].inflight_bytes = 4 * 1024 * 1024;
    allocations.assign(requests.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, with_flight.data(),
        with_flight.size(), startup, minimum, allocations.data()) ==
        with_flight.size());
    const auto* shared = FindAllocation(allocations, 2);
    BOOST_REQUIRE(shared);
    BOOST_TEST(shared->credit_bytes + 4 * 1024 * 1024 == shared->target_bytes +
        shared->discovery_bytes);
    const auto shared_target = shared->target_bytes;
    // Once flight reaches the target, credit is exactly zero, not negative
    // and not borrowed from a peer.
    with_flight[1].inflight_bytes = shared_target;
    allocations.assign(requests.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, with_flight.data(),
        with_flight.size(), startup, minimum, allocations.data()) ==
        with_flight.size());
    shared = FindAllocation(allocations, 2);
    BOOST_REQUIRE(shared);
    BOOST_TEST(shared->credit_bytes == 0u);
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_unknown_or_stale_capacity_uses_equal_share) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t minimum = 65535;
    // The demand helper only has a floor; the plan supplies the equal-share
    // fallback for a carrier with no usable capacity estimate.
    BOOST_TEST(Scheduler::FlightDemandBytes(
        0, false, startup, minimum) == startup);
    BOOST_TEST(Scheduler::FlightDemandBytes(
        64 * 1024 * 1024, false, startup, minimum) == startup);
    BOOST_TEST(Scheduler::FlightDemandBytes(
        0, true, startup, minimum) == startup);
    BOOST_TEST(Scheduler::FlightDemandBytes(
        1024, true, startup, minimum) == startup);
    // A minimum larger than startup still floors the demand.
    BOOST_TEST(Scheduler::FlightDemandBytes(
        0, false, startup, 2 * startup) == 2 * startup);

    std::vector<CreditRequest> requests(3);
    for (std::size_t i = 0; i < requests.size(); ++i) {
        requests[i].id = static_cast<std::uint16_t>(i + 1);
        requests[i].eligible = true;
    }
    // A stale BDP must not be trusted as demand even if a caller leaves the
    // number populated; only the capacity_known flag unlocks BDP weighting.
    requests[2].bdp_bytes = 64 * 1024 * 1024;
    std::vector<CreditAllocation> allocations(requests.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) ==
        requests.size());
    const auto equal_share = window / requests.size();
    for (const auto& allocation : allocations) {
        BOOST_TEST(allocation.eligible);
        BOOST_TEST(!allocation.capacity_known);
        BOOST_TEST(allocation.demand_bytes == equal_share);
        // Three equal-share demands fit the session window.
        BOOST_TEST(allocation.target_bytes == equal_share);
        BOOST_TEST(allocation.credit_bytes == equal_share);
    }

    // When the session window is smaller than the summed fallback demands,
    // the proportional split still caps the total and leaves every carrier a
    // nonzero share.
    constexpr std::uint64_t small_window = 512 * 1024;
    allocations.assign(requests.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(small_window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) ==
        requests.size());
    std::uint64_t total = 0;
    for (const auto& allocation : allocations) {
        BOOST_TEST(allocation.demand_bytes == startup);
        BOOST_TEST(allocation.target_bytes > 0);
        BOOST_TEST(allocation.target_bytes <= startup);
        total += allocation.target_bytes;
    }
    BOOST_TEST(total <= small_window);
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_excludes_draining_and_avoided_carriers) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t minimum = 65535;
    constexpr std::uint64_t bdp = 8 * 1024 * 1024;
    const auto demand = Scheduler::FlightDemandBytes(bdp, true, startup, minimum);

    auto draining = KnownCreditRequest(3, 64 * 1024 * 1024, 0,
        std::numeric_limits<std::uint64_t>::max());
    draining.eligible = false;
    const std::vector<CreditRequest> requests{
        KnownCreditRequest(1, bdp),
        KnownCreditRequest(2, bdp),
        draining,
    };
    std::vector<CreditAllocation> allocations(requests.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) ==
        requests.size());
    const auto* avoided = FindAllocation(allocations, 3);
    BOOST_REQUIRE(avoided);
    // An avoided carrier is not a live demand: it gets nothing and cannot
    // dilute the two eligible carriers' split.
    BOOST_TEST(!avoided->eligible);
    BOOST_TEST(avoided->demand_bytes == 0u);
    BOOST_TEST(avoided->target_bytes == 0u);
    BOOST_TEST(avoided->discovery_bytes == 0u);
    BOOST_TEST(avoided->credit_bytes == 0u);
    const auto* first = FindAllocation(allocations, 1);
    const auto* second = FindAllocation(allocations, 2);
    BOOST_REQUIRE(first && second);
    BOOST_TEST(first->target_bytes == demand);
    BOOST_TEST(second->target_bytes == demand);

    // Join: one more live carrier re-splits the same window immediately
    // because the plan holds no per-carrier tracking state.
    std::vector<CreditRequest> joined = requests;
    joined.push_back(KnownCreditRequest(4, 512 * 1024));
    allocations.assign(joined.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, joined.data(),
        joined.size(), startup, minimum, allocations.data()) == joined.size());
    std::uint64_t total = 0;
    for (const auto& allocation : allocations) {
        if (!allocation.eligible) continue;
        BOOST_TEST(allocation.target_bytes <= allocation.demand_bytes);
        total += allocation.target_bytes;
    }
    BOOST_TEST(total <= window);

    // Leave: with only one live carrier, the whole demand is funded and the
    // session cap still applies if that demand were larger than the window.
    std::vector<CreditRequest> alone{KnownCreditRequest(4, 512 * 1024)};
    allocations.assign(alone.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, alone.data(),
        alone.size(), startup, minimum, allocations.data()) == alone.size());
    BOOST_TEST(allocations.front().target_bytes ==
        Scheduler::FlightDemandBytes(512 * 1024, true, startup, minimum));
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_discovery_is_separately_bounded) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t minimum = 65535;
    constexpr std::uint64_t low_bdp = 1024 * 1024;
    constexpr std::uint64_t high_bdp = 8 * 1024 * 1024;
    const auto low_demand = Scheduler::FlightDemandBytes(
        low_bdp, true, startup, minimum);
    const auto high_demand = Scheduler::FlightDemandBytes(
        high_bdp, true, startup, minimum);

    // Only the capacity no demand claimed may fund discovery, and only up to
    // the caller's probe budget.
    std::vector<CreditRequest> requests{
        KnownCreditRequest(1, low_bdp, 0, 1024 * 1024),
        KnownCreditRequest(2, high_bdp),
    };
    std::vector<CreditAllocation> allocations(requests.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) ==
        requests.size());
    const auto* probe = FindAllocation(allocations, 1);
    const auto* steady = FindAllocation(allocations, 2);
    BOOST_REQUIRE(probe && steady);
    BOOST_TEST(probe->target_bytes == low_demand);
    BOOST_TEST(steady->target_bytes == high_demand);
    BOOST_TEST(probe->discovery_bytes == 1024 * 1024);
    BOOST_TEST(probe->discovery_bytes <= requests[0].discovery_budget_bytes);
    BOOST_TEST(steady->discovery_bytes == 0u);
    BOOST_TEST(probe->target_bytes + probe->discovery_bytes +
        steady->target_bytes <= window);

    // A larger budget can use all remaining unclaimed capacity, but never
    // pushes the plan over the session window.
    requests[0].discovery_budget_bytes = 64 * 1024 * 1024;
    allocations.assign(requests.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) ==
        requests.size());
    probe = FindAllocation(allocations, 1);
    steady = FindAllocation(allocations, 2);
    BOOST_REQUIRE(probe && steady);
    BOOST_TEST(probe->discovery_bytes == window - low_demand - high_demand);
    BOOST_TEST(probe->target_bytes + probe->discovery_bytes +
        steady->target_bytes == window);

    // Existing probe flight above the base target must be reserved first.
    // Only one byte remains unclaimed; it cannot re-grant the old probe flight
    // as if those bytes had already been acknowledged.
    requests[0].inflight_bytes = probe->target_bytes + probe->discovery_bytes - 1;
    allocations.assign(requests.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, requests.data(),
        requests.size(), startup, minimum, allocations.data()) ==
        requests.size());
    probe = FindAllocation(allocations, 1);
    BOOST_REQUIRE(probe);
    BOOST_TEST(probe->discovery_bytes == 1u);
    BOOST_TEST(probe->credit_bytes == 0u);

    // When demand already exhausts the session window there is no discovery
    // capacity to hand out, even for an unlimited probe budget.
    const std::vector<CreditRequest> saturated{
        KnownCreditRequest(1, 64 * 1024 * 1024, 0,
            std::numeric_limits<std::uint64_t>::max()),
        KnownCreditRequest(2, 64 * 1024 * 1024),
    };
    allocations.assign(saturated.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, saturated.data(),
        saturated.size(), startup, minimum, allocations.data()) ==
        saturated.size());
    for (const auto& allocation : allocations) {
        BOOST_TEST(allocation.discovery_bytes == 0u);
        BOOST_TEST(allocation.target_bytes <= allocation.demand_bytes);
    }

    // Discovery also remains available to a carrier whose capacity is still
    // unknown: its target is the equal-share fallback, and the separately
    // bounded probe grant is the only way to gather evidence.
    const std::vector<CreditRequest> unknown_probe{
        CreditRequest{1, true, false, 0, 0, {}, {}, 64 * 1024 * 1024},
        KnownCreditRequest(2, 512 * 1024),
    };
    allocations.assign(unknown_probe.size(), CreditAllocation{});
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, unknown_probe.data(),
        unknown_probe.size(), startup, minimum, allocations.data()) ==
        unknown_probe.size());
    const auto* unmeasured = FindAllocation(allocations, 1);
    BOOST_REQUIRE(unmeasured);
    const auto known_low_demand = Scheduler::FlightDemandBytes(
        512 * 1024, true, startup, minimum);
    BOOST_TEST(unmeasured->demand_bytes == window / unknown_probe.size());
    BOOST_TEST(unmeasured->discovery_bytes ==
        window - unmeasured->demand_bytes - known_low_demand);
    BOOST_TEST(unmeasured->target_bytes + unmeasured->discovery_bytes +
        FindAllocation(allocations, 2)->target_bytes == window);
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_saturates_extreme_demands_without_wrapping) {
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t minimum = 65535;
    BOOST_TEST(Scheduler::FlightDemandBytes(maximum, true, startup, minimum) ==
        maximum);
    BOOST_TEST(Scheduler::FlightDemandBytes(maximum, true, startup, minimum,
        1, maximum) == maximum);

    std::vector<CreditRequest> requests{
        KnownCreditRequest(1, maximum, maximum),
        KnownCreditRequest(2, maximum, 0),
    };
    std::vector<CreditAllocation> allocations(requests.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(maximum, requests.data(),
        requests.size(), startup, minimum, allocations.data()) ==
        requests.size());
    std::uint64_t total = 0;
    for (const auto& allocation : allocations) {
        BOOST_TEST(allocation.target_bytes <= allocation.demand_bytes);
        const auto target = allocation.target_bytes >
            std::numeric_limits<std::uint64_t>::max() - total
            ? std::numeric_limits<std::uint64_t>::max() : total +
                allocation.target_bytes;
        total = target;
    }
    // The saturated sum cannot wrap below the session window.
    BOOST_TEST(total <= maximum);
    const auto* saturated_flight = FindAllocation(allocations, 1);
    BOOST_REQUIRE(saturated_flight);
    BOOST_TEST(saturated_flight->credit_bytes == 0u);
}

BOOST_AUTO_TEST_CASE(standalone_credit_allocator_caps_total_and_falls_back_to_equal_share) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t mib = 1024 * 1024;

    // Zero demand means no capacity estimate: every carrier gets an equal
    // share instead of zero (or an integer-wrap fallback).
    const std::uint64_t unknown_demands[3] = {0, 0, 0};
    std::uint64_t credits[3] = {};
    std::uint64_t allocated = Scheduler::AllocateFlightCredits(
        window, unknown_demands, nullptr, 3, credits);
    BOOST_TEST(allocated <= window);
    BOOST_TEST(allocated == 3 * (window / 3));
    for (const auto credit : credits) {
        BOOST_TEST(credit == window / 3);
    }

    // A high-demand carrier gets the capacity the low-demand carriers cannot
    // use; no single carrier exceeds its demand and the sum stays capped.
    const std::uint64_t demands[3] = {mib, mib, 32 * mib};
    allocated = Scheduler::AllocateFlightCredits(
        window, demands, nullptr, 3, credits);
    BOOST_TEST(allocated <= window);
    BOOST_TEST(credits[2] > window / 3);
    BOOST_TEST(credits[2] <= demands[2]);
    BOOST_TEST(credits[0] <= demands[0]);
    BOOST_TEST(credits[1] <= demands[1]);
    BOOST_TEST(credits[0] + credits[1] + credits[2] <= window);

    // Outstanding flight is subtracted from the carrier's own target.
    const std::uint64_t one_demand[1] = {4 * mib};
    std::uint64_t one_flight[1] = {mib};
    std::uint64_t one_credit[1] = {};
    BOOST_TEST(Scheduler::AllocateFlightCredits(window, one_demand,
        one_flight, 1, one_credit) == 4 * mib);
    BOOST_TEST(one_credit[0] == 3 * mib);
    one_flight[0] = 4 * mib;
    BOOST_TEST(Scheduler::AllocateFlightCredits(window, one_demand,
        one_flight, 1, one_credit) == 4 * mib);
    BOOST_TEST(one_credit[0] == 0u);

    // Invalid shapes report zero so the caller keeps its local credit policy.
    const std::uint64_t one[1] = {1};
    BOOST_TEST(Scheduler::AllocateFlightCredits(window, one, nullptr,
        Scheduler::MaxCandidates + 1, credits) == 0u);
    BOOST_TEST(Scheduler::AllocateFlightCredits(window, nullptr, nullptr,
        1, one_credit) == 0u);
    BOOST_TEST(Scheduler::AllocateFlightCredits(window, one_demand, nullptr,
        1, nullptr) == 0u);
}

BOOST_AUTO_TEST_CASE(borrow_credit_plan_is_order_independent) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t minimum = 65535;
    std::vector<CreditRequest> forward{
        KnownCreditRequest(1, 1024 * 1024, 4096, 256 * 1024),
        KnownCreditRequest(2, 8 * 1024 * 1024, 1024 * 1024),
        KnownCreditRequest(3, 2 * 1024 * 1024),
    };
    std::vector<CreditRequest> reverse(forward.rbegin(), forward.rend());
    std::vector<CreditAllocation> forward_plan(forward.size());
    std::vector<CreditAllocation> reverse_plan(reverse.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, forward.data(),
        forward.size(), startup, minimum, forward_plan.data()) ==
        forward.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, reverse.data(),
        reverse.size(), startup, minimum, reverse_plan.data()) ==
        reverse.size());
    for (const auto& expected : forward_plan) {
        const auto* actual = FindAllocation(reverse_plan, expected.id);
        BOOST_REQUIRE(actual);
        BOOST_TEST(actual->eligible == expected.eligible);
        BOOST_TEST(actual->capacity_known == expected.capacity_known);
        BOOST_TEST(actual->demand_bytes == expected.demand_bytes);
        BOOST_TEST(actual->target_bytes == expected.target_bytes);
        BOOST_TEST(actual->discovery_bytes == expected.discovery_bytes);
        BOOST_TEST(actual->credit_bytes == expected.credit_bytes);
    }

    // A plan that cannot be built reports zero so callers can fall back to
    // their local credit policy instead of sending with a partial plan.
    std::vector<CreditAllocation> one(1);
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, nullptr, 1,
        startup, minimum, one.data()) == 0u);
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, forward.data(),
        forward.size(), startup, minimum, nullptr) == 0u);
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, forward.data(), 0,
        startup, minimum, one.data()) == 0u);
    std::vector<CreditRequest> too_many(Scheduler::MaxCandidates + 1);
    std::vector<CreditAllocation> too_many_plan(too_many.size());
    BOOST_TEST(Scheduler::BuildFlightCreditPlan(window, too_many.data(),
        too_many.size(), startup, minimum, too_many_plan.data()) == 0u);
}

BOOST_AUTO_TEST_CASE(borrow_credit_request_uses_only_fresh_capacity) {
    constexpr std::uint64_t now = 10000;
    mp::PathTelemetry telemetry;
    telemetry.has_sample = true;
    telemetry.updated_ms = now - 100;
    telemetry.capacity_rate_bytes_per_second = 8 * 1024 * 1024;
    telemetry.capacity_updated_ms = now - 100;
    telemetry.min_rtt_ms = 20;
    telemetry.srtt_ms = 30;
    telemetry.bdp_bytes = 4 * 1024 * 1024;
    telemetry.wire_inflight_bytes = 512 * 1024;

    auto request = Scheduler::MakeFlightCreditRequest(7, telemetry, true,
        now, 64 * 1024);
    BOOST_TEST(request.id == 7);
    BOOST_TEST(request.eligible);
    BOOST_TEST(request.capacity_known);
    BOOST_TEST(request.bdp_bytes == telemetry.bdp_bytes);
    BOOST_TEST(request.inflight_bytes == telemetry.wire_inflight_bytes);
    BOOST_TEST(request.discovery_budget_bytes == 64u * 1024u);
    BOOST_REQUIRE(request.min_rtt_ms);
    BOOST_REQUIRE(request.observed_rtt_ms);
    BOOST_TEST(*request.min_rtt_ms == 20u);
    BOOST_TEST(*request.observed_rtt_ms == 30u);

    // The same BDP after its capacity sample expires is stale: the request
    // drops the number so the plan applies its equal-share fallback instead
    // of trusting an expired estimate.
    telemetry.capacity_updated_ms = now - Scheduler::MetricsMaxAgeMs - 1;
    request = Scheduler::MakeFlightCreditRequest(7, telemetry, true, now);
    BOOST_TEST(!request.capacity_known);
    BOOST_TEST(request.bdp_bytes == 0u);
    BOOST_TEST(Scheduler::FlightDemandBytes(request.bdp_bytes,
        request.capacity_known, 256 * 1024, 65535) == 256u * 1024u);

    // An avoided/draining carrier never becomes a live demand.
    request = Scheduler::MakeFlightCreditRequest(7, telemetry, false, now);
    BOOST_TEST(!request.eligible);
    BOOST_TEST(!request.capacity_known);
    BOOST_TEST(request.bdp_bytes == 0u);
}

BOOST_AUTO_TEST_CASE(flight_headroom_bytes_falls_back_to_one_bdp) {
    constexpr std::uint64_t bdp = 4 * 1024 * 1024;
    constexpr std::uint64_t baseline = bdp / Scheduler::FlightHeadroomDivisor;
    BOOST_TEST(Scheduler::FlightHeadroomBytes(0) == 0u);
    // No RTT pair at all, an unknown min RTT, or an unknown observed RTT all
    // keep the previous 2x BDP target.
    BOOST_TEST(Scheduler::FlightHeadroomBytes(bdp) == baseline);
    BOOST_TEST(Scheduler::FlightHeadroomBytes(bdp, 0, 25) == baseline);
    BOOST_TEST(Scheduler::FlightHeadroomBytes(bdp, 10, 0) == baseline);
    // A symmetric path (srtt at or below min_rtt) has no measured reverse-path
    // dilation, so it also keeps the fallback.
    BOOST_TEST(Scheduler::FlightHeadroomBytes(bdp, 20, 20) == baseline);
    BOOST_TEST(Scheduler::FlightHeadroomBytes(bdp, 20, 10) == baseline);
}

BOOST_AUTO_TEST_CASE(flight_headroom_bytes_expands_with_ack_reversal) {
    constexpr std::uint64_t bdp = 4 * 1024 * 1024;
    // r40's reverse ACK path was 2.3-3x slower than min RTT. The headroom
    // tracks the excess over min RTT, so the total target is srtt/min_rtt BDP.
    BOOST_TEST(Scheduler::FlightHeadroomBytes(bdp, 10, 23) == bdp * 1300 / 1000);
    BOOST_TEST(Scheduler::FlightHeadroomBytes(bdp, 10, 25) == bdp * 1500 / 1000);
    BOOST_TEST(Scheduler::FlightHeadroomBytes(bdp, 10, 30) == bdp * 2000 / 1000);
    // MaxAckReversalPermille is a hard ceiling: a 4x RTT sample still
    // targets 3x BDP, never more.
    BOOST_TEST(Scheduler::MaxAckReversalPermille == 3000u);
    BOOST_TEST(Scheduler::FlightHeadroomBytes(bdp, 10, 40) == bdp * 2000 / 1000);
    BOOST_TEST(Scheduler::FlightHeadroomBytes(bdp, 10, 200) == bdp * 2000 / 1000);
    // Saturating multiplication must not wrap for an extreme BDP.
    BOOST_TEST(Scheduler::FlightHeadroomBytes(std::numeric_limits<std::uint64_t>::max(),
        1, std::numeric_limits<std::uint64_t>::max()) ==
        std::numeric_limits<std::uint64_t>::max());
}

BOOST_AUTO_TEST_CASE(adaptive_flight_credit_accepts_measured_rtt_pair) {
    constexpr std::uint64_t window = 32 * 1024 * 1024;
    constexpr std::uint64_t share = window / 3;
    constexpr std::uint64_t startup = 256 * 1024;
    constexpr std::uint64_t frame = 65535;
    constexpr std::uint64_t bdp = 2 * 1024 * 1024;
    constexpr std::uint64_t fallback_target =
        bdp + bdp / Scheduler::FlightHeadroomDivisor;
    // A symmetric RTT pair is identical to the no-measurement fallback.
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, 0, 100,
        startup, frame, 0, 10, 10) == fallback_target);
    // A 2.5x srtt/min_rtt sample raises the target to 2.5x BDP, and the
    // measured target is a floor even at reduced scheduling weight.
    constexpr std::uint64_t dilated_target = bdp * 2500 / 1000;
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, 0, 100,
        startup, frame, 0, 10, 25) == dilated_target);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, 0, 25,
        startup, frame, 0, 10, 25) == dilated_target);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, bdp, 100,
        startup, frame, 0, 10, 25) == dilated_target - bdp);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, dilated_target, 100,
        startup, frame, 0, 10, 25) == 0u);
    // The reversal ratio caps the target at 3x BDP.
    constexpr std::uint64_t capped_target = bdp * 3;
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, bdp, 0, 100,
        startup, frame, 0, 10, 90) == capped_target);
    // The session share always remains the hard ceiling.
    constexpr std::uint64_t high_bdp = 16 * 1024 * 1024;
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3, high_bdp, 0, 100,
        startup, frame, 0, 10, 90) == share);
    BOOST_TEST(Scheduler::AdaptiveFlightCredit(window, 3,
        std::numeric_limits<std::uint64_t>::max(), 0, 100, startup, frame,
        0, 1, std::numeric_limits<std::uint64_t>::max()) == share);
}

BOOST_AUTO_TEST_CASE(capacity_probe_needed_tracks_only_a_fresh_capacity_gap) {
    mp::PathTelemetry telemetry;
    telemetry.at_capacity = true;
    telemetry.capacity_rate_bytes_per_second = 10 * 1024 * 1024;
    telemetry.current_rate_bytes_per_second = 10 * 1024 * 1024;
    // Delivery at the estimate means the path is full, not under-discovered.
    BOOST_TEST(!Scheduler::CapacityProbeNeeded(telemetry));
    telemetry.current_rate_bytes_per_second = 8 * 1024 * 1024 + 1;
    BOOST_TEST(!Scheduler::CapacityProbeNeeded(telemetry));
    // Exactly a fifth below the estimate is already a discovery gap, matching
    // the probe gate's NeedsCapacityProbe threshold.
    telemetry.current_rate_bytes_per_second = 8 * 1024 * 1024;
    BOOST_TEST(Scheduler::CapacityProbeNeeded(telemetry));
    telemetry.current_rate_bytes_per_second = 1024 * 1024;
    BOOST_TEST(Scheduler::CapacityProbeNeeded(telemetry));
    // at_capacity is the freshness signal: below it, or without a fresh
    // capacity/delivery pair, the allowance must stay closed.
    telemetry.at_capacity = false;
    BOOST_TEST(!Scheduler::CapacityProbeNeeded(telemetry));
    telemetry.at_capacity = true;
    telemetry.current_rate_bytes_per_second = 0;
    BOOST_TEST(!Scheduler::CapacityProbeNeeded(telemetry));
    telemetry.current_rate_bytes_per_second = 1024 * 1024;
    telemetry.capacity_rate_bytes_per_second = 0;
    BOOST_TEST(!Scheduler::CapacityProbeNeeded(telemetry));
}

BOOST_AUTO_TEST_CASE(discovery_allowance_requires_a_live_probe_owner_or_a_gap) {
    constexpr std::uint64_t high_water = 256 * 1024;
    constexpr std::uint64_t burst = 16 * 1024 * 1024;
    // A live owner publishes its remaining burst, capped by the per-link write
    // window so one calculation never adds more than one high water.
    BOOST_TEST(Scheduler::DiscoveryAllowanceBytes(true, 1024 * 1024, false,
        burst, high_water) == high_water);
    BOOST_TEST(Scheduler::DiscoveryAllowanceBytes(true, 64 * 1024, false,
        burst, high_water) == 64u * 1024u);
    // An exhausted remainder is not itself a grant.
    BOOST_TEST(Scheduler::DiscoveryAllowanceBytes(true, 0, false, burst,
        high_water) == 0u);
    // A fresh capacity gap still justifies the default budget.
    BOOST_TEST(Scheduler::DiscoveryAllowanceBytes(false, 0, true, burst,
        high_water) == high_water);
    // r57 regression: when the probe owner moved to another carrier (or the
    // burst was cleared), this carrier's per-link cached remainder stayed
    // nonzero. It must not keep granting discovery flight from stale state.
    BOOST_TEST(Scheduler::DiscoveryAllowanceBytes(false, 6 * 1024 * 1024,
        false, burst, high_water) == 0u);
    // With a real gap the default budget applies, never the stale remainder.
    BOOST_TEST(Scheduler::DiscoveryAllowanceBytes(false, 6 * 1024 * 1024,
        true, burst, high_water) == high_water);
    // Degenerate limits never grant bytes.
    BOOST_TEST(Scheduler::DiscoveryAllowanceBytes(true, burst, false, burst,
        0) == 0u);
    BOOST_TEST(Scheduler::DiscoveryAllowanceBytes(false, 0, true, 0,
        high_water) == 0u);
}

BOOST_AUTO_TEST_CASE(capacity_freshness_uses_its_own_sample_time) {
    mp::PathTelemetry telemetry;
    telemetry.has_sample = true;
    telemetry.updated_ms = 4000;
    telemetry.capacity_rate_bytes_per_second = 8 * 1024 * 1024;
    telemetry.current_rate_bytes_per_second = 256 * 1024;
    telemetry.srtt_ms = 20;
    telemetry.confidence = 100;

    // Nonzero capacity alone is not enough: it needs the sample that produced it.
    BOOST_TEST(!Scheduler::CapacityIsFresh(telemetry, 5000));
    telemetry.capacity_updated_ms = 4000;
    BOOST_TEST(Scheduler::CapacityIsFresh(telemetry, 5000));
    telemetry.capacity_updated_ms = 4999;
    BOOST_TEST(!Scheduler::CapacityIsFresh(telemetry, 10000));
    BOOST_TEST(telemetry.updated_ms == 4000u); // RTT/delivery freshness is separate.

    mp::PathControlSnapshot snapshot;
    snapshot.state = mp::PathState::Active;
    snapshot.telemetry = telemetry;
    snapshot.telemetry.updated_ms = 9800;
    snapshot.capabilities.payload_delivery_rate = true;

    // Fresh delivery metrics with an expired capacity sample must fall back to
    // the observed delivery rate, not keep using the stale capacity estimate.
    const auto stale = Scheduler::ProjectCandidate(1, snapshot, 10000, 1024 * 1024);
    BOOST_REQUIRE(stale.throughput_bytes_per_second);
    BOOST_TEST(*stale.throughput_bytes_per_second == 256u * 1024u);

    snapshot.telemetry.capacity_updated_ms = 9900;
    const auto fresh = Scheduler::ProjectCandidate(1, snapshot, 10000, 1024 * 1024);
    BOOST_REQUIRE(fresh.throughput_bytes_per_second);
    BOOST_TEST(*fresh.throughput_bytes_per_second == 8u * 1024u * 1024u);
}

BOOST_AUTO_TEST_CASE(expired_capacity_keeps_a_fresh_delivery_rate_schedulable) {
    constexpr std::uint64_t now = 10000;
    constexpr std::uint64_t delivery_rate = 4 * 1024 * 1024;
    constexpr std::uint64_t expired_capacity = 8 * 1024 * 1024;

    mp::PathControlSnapshot snapshot;
    snapshot.state = mp::PathState::Active;
    snapshot.telemetry.has_sample = true;
    snapshot.telemetry.updated_ms = now - 100;
    snapshot.telemetry.srtt_ms = 20;
    snapshot.telemetry.min_rtt_ms = 20;
    snapshot.telemetry.confidence = 100;
    snapshot.telemetry.current_rate_bytes_per_second = delivery_rate;
    snapshot.capabilities.active_probe = true;
    // The production wiring published the capability only while the capacity
    // sample was fresh. The projection contract must not depend on that: a
    // fresh delivery sample is still evidence of a usable rate.
    snapshot.capabilities.payload_delivery_rate =
        Scheduler::CapacityIsFresh(snapshot.telemetry, now);
    BOOST_TEST(!snapshot.capabilities.payload_delivery_rate);

    // An expired capacity sample must not be promoted to a fresh capacity, but
    // the live delivery sample still drives the throughput projection.
    snapshot.telemetry.capacity_rate_bytes_per_second = expired_capacity;
    snapshot.telemetry.capacity_updated_ms = now - Scheduler::MetricsMaxAgeMs - 1;
    const auto stale_capacity =
        Scheduler::ProjectCandidate(1, snapshot, now, 1024 * 1024);
    BOOST_REQUIRE(stale_capacity.throughput_bytes_per_second);
    BOOST_TEST(*stale_capacity.throughput_bytes_per_second == delivery_rate);
    BOOST_TEST(!stale_capacity.capacity_known);
    BOOST_REQUIRE(stale_capacity.delivery_rate_bytes_per_second);
    BOOST_TEST(*stale_capacity.delivery_rate_bytes_per_second == delivery_rate);

    // Production also scrubs the expired capacity fields before projection.
    snapshot.telemetry.capacity_rate_bytes_per_second = 0;
    snapshot.telemetry.capacity_updated_ms = 0;
    const auto fallback = Scheduler::ProjectCandidate(1, snapshot, now, 1024 * 1024);
    BOOST_REQUIRE(fallback.throughput_bytes_per_second);
    BOOST_TEST(*fallback.throughput_bytes_per_second == delivery_rate);
    BOOST_TEST(*fallback.throughput_bytes_per_second !=
        Scheduler::DefaultUnknownThroughputBytesPerSecond);
    BOOST_TEST(!fallback.capacity_known);

    // A fresh capacity sample still outranks the delivery sample.
    snapshot.telemetry.capacity_rate_bytes_per_second = expired_capacity;
    snapshot.telemetry.capacity_updated_ms = now - 100;
    const auto fresh_capacity =
        Scheduler::ProjectCandidate(1, snapshot, now, 1024 * 1024);
    BOOST_REQUIRE(fresh_capacity.throughput_bytes_per_second);
    BOOST_TEST(*fresh_capacity.throughput_bytes_per_second == expired_capacity);
    BOOST_TEST(fresh_capacity.capacity_known);

    // Consumer behavior: the fallback keeps the carrier on its measured rate.
    // Without it the 20 ms carrier would be charged the 64 KiB/s default for a
    // 256 KiB frame (~4000 ms) and lose to the 5 ms carrier that is genuinely
    // unknown; at 4 MiB/s it wins comfortably.
    Scheduler scheduler;
    const Candidate unknown_rate = Measured(2, 5,
        Scheduler::DefaultUnknownThroughputBytesPerSecond, now);
    const auto selected = scheduler.Select(Scheduler::Policy::Adaptive,
        std::vector<Candidate>{fallback, unknown_rate}, now, 256 * 1024);
    BOOST_TEST(IsStatus(selected.status, Scheduler::Status::Selected));
    BOOST_TEST(selected.carrier_id == 1);
}

BOOST_AUTO_TEST_CASE(adaptive_carrier_uses_baseline_flight_and_responds_to_pressure) {
    Scheduler scheduler;
    Candidate high_capacity = Measured(1, 90, 30 * 1024 * 1024, 1000);
    high_capacity.bdp_bytes = 2 * 1024 * 1024;
    high_capacity.unacked_bytes = *high_capacity.bdp_bytes;

    Candidate slower = Measured(2, 125, 12 * 1024 * 1024, 1000);
    slower.bdp_bytes = 0;
    auto selected = scheduler.Select(Scheduler::Policy::Adaptive,
        {high_capacity, slower}, 1000, 4096);
    BOOST_TEST(selected.carrier_id == 1);

    high_capacity.jitter_ms = 20;
    high_capacity.scheduling_weight = 50;
    selected = scheduler.Select(Scheduler::Policy::Adaptive, {high_capacity, slower}, 1100, 4096);
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

BOOST_AUTO_TEST_CASE(adaptive_accounts_for_prior_service_bytes) {
    Scheduler scheduler;
    Candidate first = Measured(1, 5, 10 * 1024 * 1024, 1000);
    Candidate second = Measured(2, 80, 10 * 1024 * 1024, 1000);
    first.sent_payload_bytes = 32 * 1024 * 1024;
    auto selected = scheduler.Select(Scheduler::Policy::Adaptive,
        {first, second}, 1000, 4096);
    BOOST_TEST(selected.carrier_id == 2);

    first.sent_payload_bytes = 0;
    selected = scheduler.Select(Scheduler::Policy::Adaptive,
        {first, second}, 1001, 4096);
    BOOST_TEST(selected.carrier_id == 1);
    // A backward counter is a new service epoch, not a debt to keep paying.
    BOOST_TEST(scheduler.FairnessDebtMs(first) == 0);
}

BOOST_AUTO_TEST_CASE(early_low_rate_service_does_not_pin_a_corrected_fast_path) {
    // Long probe interval: this test isolates the fairness epoch from bursts.
    Scheduler scheduler(60000);
    constexpr std::uint64_t start = 1000;
    constexpr std::uint64_t frame = 4096;

    // While path 1's delivery rate was badly underestimated it was charged
    // 512 MiB at 1 MiB/s, i.e. over 512 s of virtual service. A session-total
    // comparator would keep that history for the rest of the session.
    Candidate fast = Measured(1, 5, 1 * 1024 * 1024, start);
    fast.sent_payload_bytes = 512ull * 1024 * 1024;
    Candidate slow = Measured(2, 50, 2 * 1024 * 1024, start);

    auto selected = scheduler.Select(Scheduler::Policy::Adaptive,
        {fast, slow}, start, frame);
    BOOST_TEST(selected.carrier_id == 2);
    // The very first epoch is already bounded, not 512 s.
    BOOST_TEST(scheduler.FairnessDebtMs(fast) == Scheduler::FairnessDebtCapMs);

    // The estimator converges to the real rate; the sent counter is unchanged.
    // Re-pricing the bounded epoch at the corrected rate must immediately drop
    // most of the early history instead of waiting for it to time out.
    fast.throughput_bytes_per_second = 32 * 1024 * 1024;
    std::uint64_t previous_debt = scheduler.FairnessDebtMs(fast);
    for (std::uint64_t now = start + Scheduler::FairnessDecayHalfLifeMs;
         now <= start + 6 * Scheduler::FairnessDecayHalfLifeMs;
         now += Scheduler::FairnessDecayHalfLifeMs) {
        fast.metrics_updated_ms = now;
        fast.last_active_ms = now;
        slow.metrics_updated_ms = now;
        slow.last_active_ms = now;
        selected = scheduler.Select(Scheduler::Policy::Adaptive,
            {fast, slow}, now, frame);
        const std::uint64_t debt = scheduler.FairnessDebtMs(fast);
        BOOST_TEST(debt <= previous_debt);
        if (now == start + Scheduler::FairnessDecayHalfLifeMs) {
            // 4,000 ms of debt priced at 1 MiB/s becomes ~63 ms at 32 MiB/s
            // (after one half-life); a session-total comparator would still
            // hold the full early epoch.
            BOOST_TEST(debt <= Scheduler::FairnessDebtCapMs / 32);
        }
        previous_debt = debt;
    }
    // The debt is far below the 46 ms RTT gap, so the corrected fast path is
    // selected again instead of being locked out permanently.
    BOOST_TEST(scheduler.FairnessDebtMs(fast) <=
        Scheduler::FairnessDebtCapMs / 64 + 1);
    BOOST_TEST(selected.carrier_id == 1);
}

BOOST_AUTO_TEST_CASE(session_length_does_not_inflate_bounded_service_debt) {
    // Debt accounting runs before probe/burst selection, so the bound holds
    // regardless of which carrier is picked; a long interval keeps the
    // synthetic counter feed deterministic.
    Scheduler scheduler(60000);
    constexpr std::uint64_t start = 1000;
    constexpr std::uint64_t bytes_per_second = 32 * 1024 * 1024;
    constexpr std::uint64_t epoch_bytes =
        bytes_per_second * Scheduler::FairnessDecayHalfLifeMs / 1000;
    Candidate busy = Measured(1, 5, bytes_per_second, start);
    Candidate idle = Measured(2, 50, bytes_per_second, start);

    // 120 s of 32 MiB/s service. The session-total comparator would hold
    // ~120,000 ms of virtual service forever; the epoch tracks only the
    // recent, decaying service and stays within the cap.
    std::uint64_t sent = 0;
    std::uint64_t phase1_end = start;
    for (std::uint64_t now = start; now < start + 120000;
         now += Scheduler::FairnessDecayHalfLifeMs) {
        sent += epoch_bytes;
        busy.sent_payload_bytes = sent;
        busy.metrics_updated_ms = now;
        busy.last_active_ms = now;
        idle.metrics_updated_ms = now;
        idle.last_active_ms = now;
        (void)scheduler.Select(Scheduler::Policy::Adaptive,
            {busy, idle}, now, 4096);
        phase1_end = now;
    }
    BOOST_TEST(scheduler.FairnessDebtMs(busy) <= Scheduler::FairnessDebtCapMs);

    // Once the carrier stops sending, the bounded epoch fades away instead of
    // persisting as a session-long penalty.
    for (std::uint64_t now = phase1_end + Scheduler::FairnessDecayHalfLifeMs;
         now <= phase1_end + 8 * Scheduler::FairnessDecayHalfLifeMs;
         now += Scheduler::FairnessDecayHalfLifeMs) {
        busy.metrics_updated_ms = now;
        busy.last_active_ms = now;
        idle.metrics_updated_ms = now;
        idle.last_active_ms = now;
        (void)scheduler.Select(Scheduler::Policy::Adaptive,
            {busy, idle}, now, 4096);
    }
    BOOST_TEST(scheduler.FairnessDebtMs(busy) <=
        Scheduler::FairnessDebtCapMs / 64 + 1);
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
    Scheduler scheduler(1000, 1200);
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

BOOST_AUTO_TEST_CASE(control_selection_does_not_postpone_payload_probe) {
    Scheduler scheduler(10, 8 * 1024);
    Candidate unknown;
    unknown.id = 1;
    unknown.eligible = true;
    unknown.send_credit_bytes = 32 * 1024 * 1024;
    unknown.probe_eligible = false;

    Candidate measured = Measured(2, 20, 32 * 1024 * 1024, 1000);
    measured.queue_bytes = 1024 * 1024 * 1024;
    std::vector<Candidate> candidates{unknown, measured};
    BOOST_TEST(scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1024).carrier_id == 1);
    BOOST_TEST(scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1005, 1024).carrier_id == 1);

    measured.queue_bytes = 0;
    unknown.probe_eligible = true;
    candidates = {unknown, measured};
    const auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1010, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(selected.probe);
}

BOOST_AUTO_TEST_CASE(active_probe_owner_survives_non_payload_control_selection) {
    Scheduler scheduler(1000, 3 * 1024);
    auto fast = Measured(1, 5, 32 * 1024 * 1024, 1000);
    auto probe_path = Measured(2, 200, 32 * 1024 * 1024, 1000);
    std::vector<Candidate> candidates{fast, probe_path};

    (void)scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1024);
    auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);
    BOOST_TEST(selected.probe_remaining_bytes == 2u * 1024u);
    BOOST_TEST(scheduler.GetProbeRuntime().owner_active);
    BOOST_TEST(scheduler.GetProbeRuntime().owner_id == 2);

    // VMUX marks every candidate probe_eligible=false for an ACK/control or
    // retransmission frame. That selection sends no probe payload, so it must
    // not clear the owner or spend the committed burst budget.
    for (Candidate& candidate : candidates) candidate.probe_eligible = false;
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2001, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
    BOOST_TEST(scheduler.GetProbeRuntime().owner_active);
    BOOST_TEST(scheduler.GetProbeRuntime().owner_id == 2);
    BOOST_TEST(scheduler.GetProbeRuntime().last_probe_ms == 2000u);
    BOOST_TEST(scheduler.ProbeRemainingBytes(probe_path) == 2u * 1024u);

    // The next payload frame resumes the same owner and spends only that
    // frame's bytes, so the burst can still finish.
    for (Candidate& candidate : candidates) candidate.probe_eligible = true;
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2002, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);
    BOOST_TEST(selected.probe_remaining_bytes == 1024u);

    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2003, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);
    BOOST_TEST(selected.probe_remaining_bytes == 0u);
    BOOST_TEST(!scheduler.GetProbeRuntime().owner_active);

    // The budget is spent, not renewed by the intervening control frame.
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2004, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
}

BOOST_AUTO_TEST_CASE(at_capacity_best_plateau_probe_is_budgeted_and_rate_limited) {
    // A known capacity estimate that matches delivery exactly never trips the
    // 20% gap gate, so the best carrier is normally skipped by the probe scan.
    // A bounded plateau burst must still be possible, but only at a much longer
    // per-carrier interval and with an explicit small budget.
    Scheduler scheduler(10, 16 * 1024, 3 * 1024);
    auto best = Measured(1, 5, 32 * 1024 * 1024, 1000);
    best.capacity_known = true;
    best.at_capacity = true;
    best.delivery_rate_bytes_per_second = 32 * 1024 * 1024;
    std::vector<Candidate> candidates{best};

    auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);

    // One ordinary probe interval is not enough for a plateau burst.
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1010, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);

    // One plateau interval after tracking, the best carrier may probe itself.
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(selected.probe);
    BOOST_TEST(selected.probe_budget_bytes == 3u * 1024u);
    BOOST_TEST(selected.probe_remaining_bytes == 2u * 1024u);

    // A control selection must not strand the plateau burst either.
    for (Candidate& candidate : candidates) candidate.probe_eligible = false;
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2001, 1024);
    BOOST_TEST(!selected.probe);
    BOOST_TEST(selected.probe_remaining_bytes == 2u * 1024u);
    BOOST_TEST(scheduler.GetProbeRuntime().owner_active);
    BOOST_TEST(scheduler.ProbeRemainingBytes(best) == 2u * 1024u);

    // The resumed burst spends its remaining budget and then ends.
    for (Candidate& candidate : candidates) candidate.probe_eligible = true;
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2002, 1024);
    BOOST_TEST(selected.probe);
    BOOST_TEST(selected.probe_remaining_bytes == 1024u);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2003, 1024);
    BOOST_TEST(selected.probe);
    BOOST_TEST(selected.probe_remaining_bytes == 0u);

    // The next plateau burst is due one plateau interval after the last probe
    // frame, not after the latest ordinary selection; an at-capacity best can
    // therefore not hold a permanent extra share.
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2004, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 3003, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(selected.probe);
    BOOST_TEST(selected.probe_remaining_bytes == 2u * 1024u);

    // A zero plateau budget restores the strict at-capacity skip.
    Scheduler no_plateau(10, 16 * 1024, 0);
    (void)no_plateau.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1024);
    selected = no_plateau.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
}

BOOST_AUTO_TEST_CASE(ordinary_selections_do_not_starve_the_probe_interval) {
    // Probe recency must follow probe payload, not the ordinary selection
    // clock. A carrier picked every few milliseconds used to refresh the same
    // timestamp the probe gate read, so it could never become probe-eligible.
    Scheduler scheduler(10, 1024);
    auto target = Measured(1, 5, 32 * 1024 * 1024, 1000);
    auto alternative = Measured(2, 200, 32 * 1024 * 1024, 1000);
    std::vector<Candidate> candidates{target, alternative};

    // The target is the cheapest path, so it is selected ordinarily several
    // times inside one probe interval.
    for (std::uint64_t now : {1000u, 1003u, 1006u}) {
        const auto ordinary = scheduler.Select(Scheduler::Policy::Adaptive, candidates, now, 1024);
        BOOST_TEST(ordinary.carrier_id == 1);
        BOOST_TEST(!ordinary.probe);
    }

    // The target stops being the cheapest path exactly one interval after it
    // was tracked. Its ordinary selections must not postpone the probe.
    candidates[0].queue_bytes = 8 * 1024 * 1024;
    candidates[1].queue_bytes = 0;
    auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1010, 1024);
    BOOST_TEST(IsStatus(selected.status, Scheduler::Status::Selected));
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(selected.probe);
    BOOST_TEST(scheduler.GetProbeRuntime().last_probe_ms == 1010u);

    // A frame-sized budget ends the burst immediately, so these become
    // ordinary selections of the target again.
    candidates[0].queue_bytes = 0;
    candidates[1].queue_bytes = 8 * 1024 * 1024;
    for (std::uint64_t now : {1013u, 1016u}) {
        const auto ordinary = scheduler.Select(Scheduler::Policy::Adaptive, candidates, now, 1024);
        BOOST_TEST(ordinary.carrier_id == 1);
        BOOST_TEST(!ordinary.probe);
    }

    // The next probe is due one interval after the previous probe, not one
    // interval after the latest ordinary selection of the target.
    candidates[0].queue_bytes = 8 * 1024 * 1024;
    candidates[1].queue_bytes = 0;
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1020, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(selected.probe);
}

BOOST_AUTO_TEST_CASE(adaptive_probe_uses_a_bounded_consecutive_byte_burst) {
    Scheduler scheduler(1000, 3 * 1024);
    auto preferred = Measured(1, 5, 32 * 1024 * 1024, 1000);
    auto probe = Measured(2, 200, 32 * 1024 * 1024, 1000);
    const std::vector<Candidate> candidates{preferred, probe};

    BOOST_TEST(scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1024).carrier_id == 1);
    auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.payload_bytes == 1024u);
    BOOST_TEST(selected.selected_send_credit_bytes == 1024u * 1024u);
    BOOST_TEST(selected.selected_queue_bytes == 0u);
    BOOST_TEST(selected.selected_wire_inflight_bytes == 0u);
    BOOST_TEST(selected.probe_budget_bytes == 3u * 1024u);
    BOOST_TEST(selected.probe_remaining_bytes == 2u * 1024u);
    BOOST_TEST(scheduler.ProbeRemainingBytes(probe) == 2u * 1024u);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
}

BOOST_AUTO_TEST_CASE(adaptive_probe_budget_caps_a_burst_and_rejects_short_budget) {
    Scheduler scheduler(1000, 8 * 1024);
    auto preferred = Measured(1, 5, 32 * 1024 * 1024, 1000);
    auto probe = Measured(2, 200, 32 * 1024 * 1024, 1000);
    probe.probe_budget_bytes = 2048;
    const std::vector<Candidate> candidates{preferred, probe};

    (void)scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1024);
    BOOST_TEST(scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024).probe);
    BOOST_TEST(scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024).probe);
    BOOST_TEST(!scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024).probe);

    Scheduler short_budget_scheduler(1000, 8 * 1024);
    probe.probe_budget_bytes = 512;
    const std::vector<Candidate> short_budget{preferred, probe};
    (void)short_budget_scheduler.Select(Scheduler::Policy::Adaptive, short_budget, 1000, 1024);
    BOOST_TEST(!short_budget_scheduler.Select(Scheduler::Policy::Adaptive,
        short_budget, 2000, 1024).probe);
}

BOOST_AUTO_TEST_CASE(adaptive_probe_keeps_only_one_burst_owner) {
    Scheduler scheduler(1000, 8 * 1024);
    const Candidate preferred = Measured(1, 5, 32 * 1024 * 1024, 1000);
    Candidate first_probe = Measured(2, 200, 32 * 1024 * 1024, 1000);
    const Candidate second_probe = Measured(3, 300, 32 * 1024 * 1024, 1000);
    const std::vector<Candidate> initial{preferred, first_probe, second_probe};

    (void)scheduler.Select(Scheduler::Policy::Adaptive, initial, 1000, 1024);
    BOOST_TEST(scheduler.Select(Scheduler::Policy::Adaptive, initial, 2000, 1024).carrier_id == 2);
    BOOST_TEST(scheduler.ProbeRemainingBytes(first_probe) == 7u * 1024u);

    first_probe.send_credit_bytes = 0;
    std::vector<Candidate> next{preferred, first_probe, second_probe};
    auto selected = scheduler.Select(Scheduler::Policy::Adaptive, next, 3000, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
    BOOST_TEST(scheduler.ProbeRemainingBytes(first_probe) == 7u * 1024u);
    BOOST_TEST(scheduler.ProbeRemainingBytes(second_probe) == 0u);

    first_probe.send_credit_bytes = 8 * 1024;
    next[1] = first_probe;
    selected = scheduler.Select(Scheduler::Policy::Adaptive, next, 3001, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);
    BOOST_TEST(scheduler.ProbeRemainingBytes(first_probe) == 6u * 1024u);
    BOOST_TEST(scheduler.ProbeRemainingBytes(second_probe) == 0u);
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

BOOST_AUTO_TEST_CASE(probe_skips_a_path_at_its_estimated_capacity) {
    Scheduler scheduler(1000, 4 * 1024);
    Candidate fast = Measured(1, 5, 32 * 1024 * 1024, 1000);
    Candidate full = Measured(2, 200, 32 * 1024 * 1024, 1000);
    full.capacity_known = true;
    full.delivery_rate_bytes_per_second = 32 * 1024 * 1024;
    const std::vector<Candidate> candidates{fast, full};

    (void)scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1024);
    full.at_capacity = true;
    BOOST_TEST(!scheduler.Select(Scheduler::Policy::Adaptive,
        {fast, full}, 2000, 1024).probe);

    full.at_capacity = false;
    const auto selected = scheduler.Select(Scheduler::Policy::Adaptive,
        {fast, full}, 3000, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);
}

BOOST_AUTO_TEST_CASE(probe_revalidates_a_capacity_model_when_delivery_lags) {
    Scheduler scheduler(1000, 4 * 1024);
    Candidate fast = Measured(1, 5, 32 * 1024 * 1024, 1000);
    Candidate lagging = Measured(2, 200, 32 * 1024 * 1024, 1000);
    lagging.capacity_known = true;
    lagging.delivery_rate_bytes_per_second = 24 * 1024 * 1024;
    lagging.at_capacity = true;

    (void)scheduler.Select(Scheduler::Policy::Adaptive,
        {fast, lagging}, 1000, 1024);
    auto selected = scheduler.Select(Scheduler::Policy::Adaptive,
        {fast, lagging}, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);

    scheduler.Reset();
    lagging.delivery_rate_bytes_per_second = 32 * 1024 * 1024;
    (void)scheduler.Select(Scheduler::Policy::Adaptive,
        {fast, lagging}, 1000, 1024);
    selected = scheduler.Select(Scheduler::Policy::Adaptive,
        {fast, lagging}, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
}

BOOST_AUTO_TEST_CASE(best_carrier_probes_its_own_lagging_capacity) {
    // A low-RTT carrier wins every ordinary selection, so the probe scan used
    // to skip it unconditionally and its depressed delivery sample could never
    // be revalidated. Only a best carrier with a real capacity-probe need may
    // claim the probe slot.
    Scheduler scheduler(1000, 4 * 1024);
    Candidate best = Measured(1, 5, 32 * 1024 * 1024, 1000);
    best.capacity_known = true;
    best.delivery_rate_bytes_per_second = 8 * 1024 * 1024;
    Candidate healthy = Measured(2, 200, 32 * 1024 * 1024, 1000);
    healthy.capacity_known = true;
    healthy.delivery_rate_bytes_per_second = 32 * 1024 * 1024;
    std::vector<Candidate> candidates{best, healthy};

    const auto ordinary = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1024);
    BOOST_TEST(ordinary.carrier_id == 1);
    BOOST_TEST(!ordinary.probe);

    // One interval after tracking, the best carrier may probe itself.
    auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(selected.probe);
    BOOST_TEST(selected.probe_budget_bytes == 4u * 1024u);
    BOOST_TEST(selected.probe_remaining_bytes == 3u * 1024u);

    // The bounded burst keeps its single owner until the budget is spent.
    for (std::uint64_t remaining : {2u * 1024u, 1024u, 0u}) {
        selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
        BOOST_TEST(selected.carrier_id == 1);
        BOOST_TEST(selected.probe);
        BOOST_TEST(selected.probe_remaining_bytes == remaining);
    }
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);

    // The next due interval goes to the carrier probed least recently, so a
    // capacity-needy best cannot monopolize the global probe slot.
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 3000, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);

    // A best carrier with no capacity-probe need stays excluded.
    Scheduler healthy_scheduler(1000, 4 * 1024);
    Candidate healthy_best = Measured(1, 5, 32 * 1024 * 1024, 1000);
    healthy_best.capacity_known = true;
    healthy_best.delivery_rate_bytes_per_second = 32 * 1024 * 1024;
    const std::vector<Candidate> solo{healthy_best};
    (void)healthy_scheduler.Select(Scheduler::Policy::Adaptive, solo, 1000, 1024);
    selected = healthy_scheduler.Select(Scheduler::Policy::Adaptive, solo, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
}

BOOST_AUTO_TEST_CASE(active_probe_burst_survives_capacity_transition) {
    Scheduler scheduler(1000, 3 * 1024);
    Candidate fast = Measured(1, 5, 32 * 1024 * 1024, 1000);
    Candidate full = Measured(2, 200, 32 * 1024 * 1024, 1000);
    std::vector<Candidate> candidates{fast, full};

    (void)scheduler.Select(Scheduler::Policy::Adaptive, candidates, 1000, 1024);
    auto selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);

    full.at_capacity = true;
    candidates[1] = full;
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 2);
    BOOST_TEST(selected.probe);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 2000, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
    selected = scheduler.Select(Scheduler::Policy::Adaptive, candidates, 3000, 1024);
    BOOST_TEST(selected.carrier_id == 1);
    BOOST_TEST(!selected.probe);
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
    BOOST_REQUIRE(candidate.delivery_rate_bytes_per_second);
    BOOST_TEST(*candidate.delivery_rate_bytes_per_second == 9000u);
    BOOST_TEST(!candidate.capacity_known);
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

namespace {

mp::PathTelemetry LinkWriteTelemetry(std::uint64_t capacity_rate,
                                     std::uint64_t min_rtt_ms,
                                     std::uint64_t now_ms) {
    mp::PathTelemetry telemetry;
    telemetry.has_sample = true;
    telemetry.updated_ms = now_ms;
    telemetry.capacity_rate_bytes_per_second = capacity_rate;
    telemetry.capacity_updated_ms = now_ms;
    telemetry.current_rate_bytes_per_second = capacity_rate;
    telemetry.min_rtt_ms = min_rtt_ms;
    telemetry.srtt_ms = min_rtt_ms;
    telemetry.confidence = 100;
    return telemetry;
}

} // namespace

BOOST_AUTO_TEST_CASE(link_write_window_unknown_capacity_stays_conservative) {
    constexpr std::uint64_t now = 10000;
    mp::PathTelemetry telemetry; // No sample at all.
    auto window = Scheduler::ComputeLinkWriteWindow(telemetry, now,
        Scheduler::LinkWriteHighWaterCeilingBytes, now - 50);
    BOOST_TEST(!window.capacity_known);
    BOOST_TEST(window.high_water_bytes == Scheduler::LinkWriteHighWaterFloorBytes);
    BOOST_TEST(window.target_bytes == Scheduler::LinkWriteHighWaterFloorBytes);
    BOOST_TEST(!window.backpressure);
    BOOST_TEST(window.credit_bytes == Scheduler::LinkWriteHighWaterFloorBytes);

    // get_link_telemetry publishes a fallback BDP from a fresh delivery rate
    // after the capacity sample expires. That is scheduling evidence, not a
    // capacity estimate, and must not size the local write window.
    telemetry.has_sample = true;
    telemetry.updated_ms = now;
    telemetry.current_rate_bytes_per_second = 32 * 1024 * 1024;
    telemetry.bdp_bytes = 8 * 1024 * 1024;
    telemetry.min_rtt_ms = 100;
    window = Scheduler::ComputeLinkWriteWindow(telemetry, now,
        Scheduler::LinkWriteHighWaterCeilingBytes, now - 50);
    BOOST_TEST(!window.capacity_known);
    BOOST_TEST(window.high_water_bytes == Scheduler::LinkWriteHighWaterFloorBytes);
    BOOST_TEST(window.target_bytes == Scheduler::LinkWriteHighWaterFloorBytes);

    // A capacity sample older than MetricsMaxAgeMs is stale even when the
    // carrier still reports a fresh delivery fallback BDP. An expanded window
    // from before the expiry is discarded, not retained.
    telemetry = LinkWriteTelemetry(32 * 1024 * 1024, 100,
        now - Scheduler::MetricsMaxAgeMs - 1);
    telemetry.bdp_bytes = 8 * 1024 * 1024;
    window = Scheduler::ComputeLinkWriteWindow(telemetry, now,
        Scheduler::LinkWriteHighWaterCeilingBytes, now - 10);
    BOOST_TEST(!window.capacity_known);
    BOOST_TEST(window.target_bytes == Scheduler::LinkWriteHighWaterFloorBytes);
    BOOST_TEST(window.high_water_bytes == Scheduler::LinkWriteHighWaterFloorBytes);
    BOOST_TEST(window.credit_bytes == Scheduler::LinkWriteHighWaterFloorBytes);

    // Fresh capacity without a min RTT cannot produce a BDP.
    telemetry.capacity_rate_bytes_per_second = 32 * 1024 * 1024;
    telemetry.capacity_updated_ms = now - 1;
    telemetry.min_rtt_ms = 0;
    window = Scheduler::ComputeLinkWriteWindow(telemetry, now,
        Scheduler::LinkWriteHighWaterFloorBytes, now - 10);
    BOOST_TEST(!window.capacity_known);
    BOOST_TEST(window.high_water_bytes == Scheduler::LinkWriteHighWaterFloorBytes);

    // A measured BDP below the floor still uses the floor.
    telemetry.capacity_rate_bytes_per_second = 64 * 1024;
    telemetry.capacity_updated_ms = now - 1;
    telemetry.min_rtt_ms = 10;
    window = Scheduler::ComputeLinkWriteWindow(telemetry, now,
        Scheduler::LinkWriteHighWaterFloorBytes, now - 10);
    BOOST_TEST(window.capacity_known);
    BOOST_TEST(window.target_bytes == Scheduler::LinkWriteHighWaterFloorBytes);
    BOOST_TEST(window.high_water_bytes == Scheduler::LinkWriteHighWaterFloorBytes);

    // Strict backpressure: at or above the current window there is no credit.
    window = Scheduler::ComputeLinkWriteWindow(telemetry, now, 0, 0,
        Scheduler::LinkWriteHighWaterFloorBytes - 1);
    BOOST_TEST(!window.backpressure);
    BOOST_TEST(window.credit_bytes == 1u);
    window = Scheduler::ComputeLinkWriteWindow(telemetry, now, 0, 0,
        Scheduler::LinkWriteHighWaterFloorBytes);
    BOOST_TEST(window.backpressure);
    BOOST_TEST(window.credit_bytes == 0u);
    window = Scheduler::ComputeLinkWriteWindow(telemetry, now, 0, 0,
        Scheduler::LinkWriteHighWaterFloorBytes + 4096);
    BOOST_TEST(window.backpressure);
    BOOST_TEST(window.credit_bytes == 0u);
}

BOOST_AUTO_TEST_CASE(link_write_window_grows_toward_a_fresh_bdp_target) {
    constexpr std::uint64_t now = 20000;
    constexpr std::uint64_t rate = 4 * 1024 * 1024;
    constexpr std::uint64_t min_rtt = 250; // 4 MiB/s * 250 ms = 1 MiB.
    constexpr std::uint64_t bdp = rate * min_rtt / 1000;
    auto telemetry = LinkWriteTelemetry(rate, min_rtt, now - 50);
    // Observed RTT and jitter never inflate the target: the window is sized
    // from the minimum RTT, so queueing delay cannot grow its own buffer.
    telemetry.srtt_ms = 900;
    telemetry.jitter_ms = 200;

    auto first = Scheduler::ComputeLinkWriteWindow(telemetry, now, 0, 0);
    BOOST_TEST(first.capacity_known);
    BOOST_TEST(first.target_bytes == bdp);
    BOOST_TEST(first.high_water_bytes ==
        Scheduler::LinkWriteHighWaterFloorBytes);

    // One full 250 ms tick adds exactly the bounded growth step, not the whole
    // measured BDP: a fresh target must not arrive as one instantaneous burst.
    constexpr std::uint64_t step =
        Scheduler::LinkWriteHighWaterGrowthBytesPerSecond *
        Scheduler::LinkWriteHighWaterMaxGrowthIntervalMs / 1000;
    BOOST_TEST(step == Scheduler::LinkWriteHighWaterMaxGrowthStepBytes);
    BOOST_TEST(step < bdp);
    auto second = Scheduler::ComputeLinkWriteWindow(telemetry, now + 250,
        first.high_water_bytes, now);
    BOOST_TEST(second.high_water_bytes ==
        Scheduler::LinkWriteHighWaterFloorBytes + step);
    BOOST_TEST(second.high_water_bytes < second.target_bytes);

    // Repeated ticks converge on the measured target and never overshoot it.
    std::uint64_t previous = second.high_water_bytes;
    std::uint64_t previous_ms = now + 250;
    for (int i = 0; i < 8 && previous < bdp; ++i) {
        const auto tick = previous_ms + 250;
        auto window = Scheduler::ComputeLinkWriteWindow(telemetry, tick,
            previous, previous_ms);
        BOOST_TEST(window.target_bytes == bdp);
        BOOST_TEST(window.high_water_bytes >= previous);
        BOOST_TEST(window.high_water_bytes <= bdp);
        previous = window.high_water_bytes;
        previous_ms = tick;
    }
    BOOST_TEST(previous == bdp);
    // A fresh sample keeps the reached target rather than overshooting it.
    auto settled = Scheduler::ComputeLinkWriteWindow(telemetry,
        previous_ms + 250, previous, previous_ms);
    BOOST_TEST(settled.target_bytes == bdp);
    BOOST_TEST(settled.high_water_bytes == bdp);
    BOOST_TEST(settled.credit_bytes == bdp);
}

BOOST_AUTO_TEST_CASE(link_write_window_high_rtt_is_capped_at_the_ceiling) {
    constexpr std::uint64_t now = 30000;
    constexpr std::uint64_t ceiling = Scheduler::LinkWriteHighWaterCeilingBytes;
    constexpr std::uint64_t floor = Scheduler::LinkWriteHighWaterFloorBytes;
    constexpr std::uint64_t step =
        Scheduler::LinkWriteHighWaterMaxGrowthStepBytes;
    static_assert(ceiling >= 7 * 1024 * 1024,
        "ceiling must cover the observed ~6.7 MiB path3 BDP");
    static_assert(ceiling <= 16 * 1024 * 1024,
        "ceiling must stay a bounded local queue");
    // 100 MiB/s at 400 ms is a 40 MiB BDP, far above the ceiling.
    auto telemetry = LinkWriteTelemetry(100 * 1024 * 1024, 400, now);
    auto window = Scheduler::ComputeLinkWriteWindow(telemetry, now, 0, 0);
    BOOST_TEST(window.capacity_known);
    BOOST_TEST(window.target_bytes == ceiling);
    BOOST_TEST(window.high_water_bytes == floor);

    // The ramp to the ceiling is bounded per tick, so a huge fresh BDP does
    // not arrive as one instantaneous burst.
    window = Scheduler::ComputeLinkWriteWindow(telemetry, now + 250,
        floor, now);
    BOOST_TEST(window.high_water_bytes == floor + step);
    BOOST_TEST(window.high_water_bytes < ceiling);

    std::uint64_t previous = window.high_water_bytes;
    std::uint64_t previous_ms = now + 250;
    constexpr std::uint64_t ticks = (ceiling - floor + step - 1) / step;
    for (std::uint64_t i = 2; i <= ticks; ++i) {
        const auto tick = now + i * 250;
        window = Scheduler::ComputeLinkWriteWindow(telemetry, tick,
            previous, previous_ms);
        BOOST_TEST(window.target_bytes == ceiling);
        BOOST_TEST(window.high_water_bytes <= ceiling);
        BOOST_TEST(window.high_water_bytes >= previous);
        BOOST_TEST(window.high_water_bytes <= window.target_bytes);
        previous = window.high_water_bytes;
        previous_ms = tick;
    }
    BOOST_TEST(previous == ceiling);
}

BOOST_AUTO_TEST_CASE(link_write_window_covers_observed_high_bdp_paths) {
    constexpr std::uint64_t now = 35000;
    constexpr std::uint64_t ceiling = Scheduler::LinkWriteHighWaterCeilingBytes;
    constexpr std::uint64_t floor = Scheduler::LinkWriteHighWaterFloorBytes;
    constexpr std::uint64_t warmup_ms = 36 * 1000;
    constexpr std::uint64_t tick_ms = 250;
    // r59: path2 measured ~3.1 MiB (200 Mbps x ~130 ms) and path3 ~6.7 MiB
    // (300 Mbps x ~187 ms). Both must fit below the hard ceiling and ramp
    // inside the 36 s warm-up without the first tick granting the whole BDP.
    struct PathBdp {
        std::uint64_t rate_bytes_per_second;
        std::uint64_t min_rtt_ms;
        std::uint64_t bdp_bytes;
    };
    constexpr PathBdp paths[] = {
        {200u * 1000u * 1000u / 8u, 130, 3'250'000},  // ~3.1 MiB
        {300u * 1000u * 1000u / 8u, 187, 7'012'500},  // ~6.7 MiB
    };

    for (const auto& path : paths) {
        // Both observed BDPs fit with headroom, so the ceiling never clips
        // them even before the sample-noise margin.
        BOOST_TEST(path.bdp_bytes < ceiling);
        auto telemetry = LinkWriteTelemetry(path.rate_bytes_per_second,
            path.min_rtt_ms, now);
        auto window = Scheduler::ComputeLinkWriteWindow(telemetry, now, 0, 0);
        BOOST_TEST(window.capacity_known);
        BOOST_TEST(window.target_bytes == path.bdp_bytes);
        BOOST_TEST(window.target_bytes > floor);
        // No instant burst: a fresh but much larger BDP starts at the floor.
        BOOST_TEST(window.high_water_bytes == floor);

        std::uint64_t previous = window.high_water_bytes;
        std::uint64_t previous_ms = now;
        std::uint64_t reached_ms = 0;
        for (std::uint64_t elapsed = tick_ms; elapsed <= warmup_ms;
             elapsed += tick_ms) {
            // Model the estimator refreshing the capacity sample: without a
            // refresh the sample would expire after MetricsMaxAgeMs and the
            // window would correctly fall back to the floor.
            telemetry.capacity_updated_ms = now + elapsed;
            telemetry.updated_ms = now + elapsed;
            const auto tick = now + elapsed;
            window = Scheduler::ComputeLinkWriteWindow(telemetry, tick,
                previous, previous_ms);
            BOOST_TEST(window.target_bytes == path.bdp_bytes);
            BOOST_TEST(window.high_water_bytes >= previous);
            BOOST_TEST(window.high_water_bytes <= path.bdp_bytes);
            previous = window.high_water_bytes;
            previous_ms = tick;
            if (previous == path.bdp_bytes) {
                reached_ms = elapsed;
                break;
            }
        }
        BOOST_TEST(reached_ms != 0);
        BOOST_TEST(reached_ms <= warmup_ms);
        // 8 MiB ceiling / (ceiling/4 per s) = 4 s, so even the 6.7 MiB BDP
        // reaches its target inside one MetricsMaxAgeMs sample lifetime.
        BOOST_TEST(reached_ms <= Scheduler::MetricsMaxAgeMs);
        BOOST_TEST(window.credit_bytes == path.bdp_bytes);
        // A full ceiling ramp takes ~4 s, so both observed BDPs must settle
        // well inside the warm-up even with coalesced ticks.
        BOOST_TEST(reached_ms <= warmup_ms / 2);
    }
}

BOOST_AUTO_TEST_CASE(link_write_window_saturates_and_reclamps_overflow) {
    constexpr std::uint64_t now = 45000;
    constexpr std::uint64_t ceiling = Scheduler::LinkWriteHighWaterCeilingBytes;

    // Degenerate values saturate at the ceiling instead of wrapping through
    // the BDP product or the growth budget.
    auto telemetry = LinkWriteTelemetry(
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(), now);
    auto window = Scheduler::ComputeLinkWriteWindow(telemetry, now, 0, 0);
    BOOST_TEST(window.capacity_known);
    BOOST_TEST(window.target_bytes == ceiling);
    BOOST_TEST(window.high_water_bytes ==
        Scheduler::LinkWriteHighWaterFloorBytes);

    // A pathological gap between caller ticks is clipped to one growth step,
    // so overflow of the elapsed time cannot bank a jump to the ceiling.
    telemetry.capacity_updated_ms = now + 100000;
    telemetry.updated_ms = now + 100000;
    window = Scheduler::ComputeLinkWriteWindow(telemetry, now + 100000,
        Scheduler::LinkWriteHighWaterFloorBytes, now);
    BOOST_TEST(window.target_bytes == ceiling);
    BOOST_TEST(window.high_water_bytes ==
        Scheduler::LinkWriteHighWaterFloorBytes +
        Scheduler::LinkWriteHighWaterMaxGrowthStepBytes);
    BOOST_TEST(window.high_water_bytes < ceiling);

    // A previous window produced by an older, larger policy is clamped into
    // the current [floor, ceiling] range before growth is applied, and a
    // fresh smaller BDP shrinks it immediately.
    auto small = LinkWriteTelemetry(4 * 1024 * 1024, 125, now); // 512 KiB.
    window = Scheduler::ComputeLinkWriteWindow(small, now, ceiling * 4,
        now - 10);
    BOOST_TEST(window.capacity_known);
    BOOST_TEST(window.target_bytes == 512 * 1024);
    BOOST_TEST(window.high_water_bytes == 512 * 1024);
    BOOST_TEST(window.high_water_bytes <= ceiling);
}

BOOST_AUTO_TEST_CASE(link_write_window_expiry_discards_the_expanded_window) {
    constexpr std::uint64_t now = 50000;
    constexpr std::uint64_t rate = 8 * 1024 * 1024;
    constexpr std::uint64_t min_rtt = 200;
    constexpr std::uint64_t target = rate * min_rtt / 1000; // 1.6 MiB.
    auto telemetry = LinkWriteTelemetry(rate, min_rtt, now);

    auto window = Scheduler::ComputeLinkWriteWindow(telemetry, now, 0, 0);
    std::uint64_t previous = window.high_water_bytes;
    std::uint64_t previous_ms = now;
    for (int i = 1; i <= 8; ++i) {
        const auto tick = now + static_cast<std::uint64_t>(i) * 250;
        window = Scheduler::ComputeLinkWriteWindow(telemetry, tick,
            previous, previous_ms);
        previous = window.high_water_bytes;
        previous_ms = tick;
    }
    BOOST_TEST(previous == target);

    // One tick past the freshness limit: no estimate survives.
    const auto expired_at = now + Scheduler::MetricsMaxAgeMs + 1;
    window = Scheduler::ComputeLinkWriteWindow(telemetry, expired_at,
        previous, expired_at - 1);
    BOOST_TEST(!window.capacity_known);
    BOOST_TEST(window.target_bytes == Scheduler::LinkWriteHighWaterFloorBytes);
    BOOST_TEST(window.high_water_bytes == Scheduler::LinkWriteHighWaterFloorBytes);
    BOOST_TEST(window.credit_bytes == Scheduler::LinkWriteHighWaterFloorBytes);

    // A fresh sample after a long stale gap grows only one bounded step; the
    // gap is not banked into an instant jump back to the target.
    const auto recovered_at = expired_at + 10000;
    telemetry.capacity_updated_ms = recovered_at;
    window = Scheduler::ComputeLinkWriteWindow(telemetry, recovered_at,
        window.high_water_bytes, expired_at);
    BOOST_TEST(window.capacity_known);
    BOOST_TEST(window.target_bytes == target);
    BOOST_TEST(window.high_water_bytes ==
        Scheduler::LinkWriteHighWaterFloorBytes +
        Scheduler::LinkWriteHighWaterGrowthBytesPerSecond *
        Scheduler::LinkWriteHighWaterMaxGrowthIntervalMs / 1000);
    BOOST_TEST(window.high_water_bytes < window.target_bytes);
}

BOOST_AUTO_TEST_CASE(link_write_window_does_not_amplify_capacity_jitter) {
    constexpr std::uint64_t now = 70000;
    constexpr std::uint64_t rtt = 125;
    auto low = LinkWriteTelemetry(4 * 1024 * 1024, rtt, now);   // 512 KiB.
    auto high = LinkWriteTelemetry(12 * 1024 * 1024, rtt, now); // 1536 KiB.
    low.srtt_ms = 800;
    low.jitter_ms = 300;
    constexpr std::uint64_t low_target = 512 * 1024;
    constexpr std::uint64_t high_target = 1536 * 1024;

    std::uint64_t previous = 0;
    std::uint64_t previous_ms = 0;
    const auto step = [&](const mp::PathTelemetry& telemetry,
                          std::uint64_t tick) {
        const auto window = Scheduler::ComputeLinkWriteWindow(telemetry,
            tick, previous, previous_ms);
        previous = window.high_water_bytes;
        previous_ms = tick;
        return window;
    };

    // Ramp to the high target first so a subsequent low sample has to shrink.
    step(high, now);
    step(high, now + 250);
    step(high, now + 500);
    const auto at_high = step(high, now + 750);
    BOOST_TEST(at_high.high_water_bytes == high_target);
    BOOST_TEST(at_high.target_bytes == high_target);

    // A lower fresh sample shrinks immediately; the larger guess is gone.
    const auto at_low = step(low, now + 1000);
    BOOST_TEST(at_low.capacity_known);
    BOOST_TEST(at_low.target_bytes == low_target);
    BOOST_TEST(at_low.high_water_bytes == low_target);

    // Rising again is rate-limited, so one noisy sample cannot jump back.
    const auto rising = step(high, now + 1250);
    BOOST_TEST(rising.high_water_bytes == low_target +
        Scheduler::LinkWriteHighWaterGrowthBytesPerSecond *
        Scheduler::LinkWriteHighWaterMaxGrowthIntervalMs / 1000);
    BOOST_TEST(rising.high_water_bytes < rising.target_bytes);

    // Alternating samples stay between the floor and the highest target.
    std::uint64_t lowest = rising.high_water_bytes;
    std::uint64_t highest = rising.high_water_bytes;
    for (int i = 0; i < 6; ++i) {
        const auto tick = now + 1500 + static_cast<std::uint64_t>(i) * 250;
        const auto window = step((i % 2 == 0) ? low : high, tick);
        BOOST_TEST(window.high_water_bytes >=
            Scheduler::LinkWriteHighWaterFloorBytes);
        BOOST_TEST(window.high_water_bytes <=
            Scheduler::LinkWriteHighWaterCeilingBytes);
        BOOST_TEST(window.high_water_bytes <= window.target_bytes);
        lowest = std::min(lowest, window.high_water_bytes);
        highest = std::max(highest, window.high_water_bytes);
    }
    BOOST_TEST(lowest >= low_target);
    BOOST_TEST(highest <= high_target);
}
