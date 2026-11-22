#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "PathControlTypes.h"

namespace ppp::app::multipath {

class PathScheduler final {
public:
    enum class Policy : std::uint8_t { Adaptive, Failover };
    enum class Status : std::uint8_t { Selected, NoEligible, Backpressure, InvalidInput };
    enum class CandidateState : std::uint8_t { Active, Warming, Degraded, Draining, Unavailable };

    struct Candidate final {
        std::uint16_t id = 0;
        bool eligible = false;
        bool preferred = false;
        std::uint64_t queue_bytes = 0;
        std::uint64_t unacked_bytes = 0;
        // Cumulative unique payload sent on this carrier in the current session.
        // The scheduler only diffs this monotonic counter into a bounded,
        // decaying epoch of virtual service time; session totals are never
        // compared directly, so an early misestimate cannot pin a carrier.
        std::uint64_t sent_payload_bytes = 0;
        std::optional<std::uint64_t> send_credit_bytes;
        std::optional<std::uint64_t> rtt_ms;
        std::optional<std::uint64_t> delivery_rate_bytes_per_second;
        std::optional<std::uint64_t> throughput_bytes_per_second;
        std::uint64_t last_active_ms = 0;
        std::uint64_t metrics_updated_ms = 0;
        // Active preserves the legacy API behavior for callers without a
        // PathControl snapshot.
        CandidateState state = CandidateState::Active;
        bool probe_eligible = true;
        bool at_capacity = false;
        bool capacity_known = false;
        std::optional<std::uint64_t> probe_budget_bytes;
        // The VMUX carrier's ACK flight includes outstanding socket writes.
        // An absent BDP preserves the legacy contract for other callers.
        std::optional<std::uint64_t> bdp_bytes;
        std::uint64_t jitter_ms = 0;
        unsigned scheduling_weight = 100;
        std::uint64_t generation = 0;
    };

    struct Selection final {
        Status status = Status::NoEligible;
        std::uint16_t carrier_id = 0;
        std::uint64_t estimated_arrival_ms = 0;
        bool probe = false;
        std::uint64_t payload_bytes = 0;
        std::uint64_t selected_send_credit_bytes = 0;
        std::uint64_t selected_queue_bytes = 0;
        std::uint64_t selected_wire_inflight_bytes = 0;
        std::uint64_t probe_budget_bytes = 0;
        std::uint64_t probe_remaining_bytes = 0;
    };
    enum class ProbeGate : std::uint32_t {
        NotEligible = 1u << 0,
        NotProbeEligible = 1u << 1,
        InvalidState = 1u << 2,
        AtCapacity = 1u << 3,
        Budget = 1u << 4,
        Best = 1u << 5,
        NoCredit = 1u << 6,
        ProbeInterval = 1u << 7,
        Recency = 1u << 8,
        NoTracker = 1u << 9,
        ProbeOwner = 1u << 10,
    };

    struct ProbeDiagnostics final {
        bool available = false;
        bool can_probe = false;
        bool needs_capacity_probe = false;
        std::uint32_t gate_mask = 0;
        std::uint64_t last_selected_ms = 0;
        std::uint64_t probe_remaining_bytes = 0;
    };

    struct ProbeRuntime final {
        std::uint64_t interval_ms = 0;
        std::uint64_t last_probe_ms = 0;
        bool clock_started = false;
        bool owner_active = false;
        std::uint16_t owner_id = 0;
    };



    static constexpr std::size_t MaxCandidates = 32;
    static constexpr std::uint64_t MetricsMaxAgeMs = 5000;
    // A plateau probe re-tests a best carrier that sits at a self-consistent
    // capacity estimate: delivery is within CapacityGapNeeded of the estimate,
    // so the normal gap gate never opens, yet the estimate itself may be the
    // ceiling. The burst is deliberately small and the per-carrier interval is
    // much longer than the ordinary probe interval, so it cannot become a
    // permanent extra share. Zero disables plateau probes.
    static constexpr std::uint64_t DefaultPlateauProbeBudgetBytes = 256 * 1024;
    static constexpr std::uint64_t MinPlateauProbeIntervalMs = 1000;
    // Virtual service is an epoch, not a session total. A carrier's charged
    // service time decays by half every FairnessDecayHalfLifeMs, so a bad
    // early rate estimate stops mattering within roughly two metrics
    // lifetimes, and a single epoch can never charge more than
    // FairnessDebtCapMs. At saturation the fixed point is
    // ~1.44 * FairnessDecayHalfLifeMs, below the cap, so the cap only clips
    // transient over-service and does not change steady-state shares.
    static constexpr std::uint64_t FairnessDecayHalfLifeMs = 2000;
    static constexpr std::uint64_t FairnessDebtCapMs = 2 * FairnessDecayHalfLifeMs;
    static constexpr std::uint64_t DefaultUnknownRttMs = 500;
    static constexpr std::uint64_t DefaultUnknownThroughputBytesPerSecond = 64 * 1024;

    explicit PathScheduler(std::uint64_t probe_interval_ms = 1000,
                           std::uint64_t probe_burst_bytes = DefaultProbeBurstBytes,
                           std::uint64_t plateau_probe_budget_bytes =
                               DefaultPlateauProbeBudgetBytes) noexcept
        : probe_interval_ms_(std::max<std::uint64_t>(1,
            std::min<std::uint64_t>(probe_interval_ms, 60000)))
        , probe_burst_bytes_(std::min(probe_burst_bytes, MaxProbeBurstBytes))
        , plateau_probe_budget_bytes_(std::min(plateau_probe_budget_bytes,
            MaxProbeBurstBytes)) {}

    // Keep a full BDP of extra flight available while a fresh estimate is
    // being validated, i.e. a 2x BDP target. Reverse-path ACK queueing may
    // expand that headroom up to the bounded 3x-BDP target. The session
    // window remains the hard aggregate bound; unknown or symmetric RTT keeps
    // the 2x baseline.
    static constexpr std::uint64_t FlightHeadroomDivisor = 1;
    static constexpr std::uint64_t MaxAckReversalPermille = 3000;
    static std::uint64_t FlightHeadroomBytes(std::uint64_t bdp,
        std::optional<std::uint64_t> min_rtt_ms = {},
        std::optional<std::uint64_t> observed_rtt_ms = {}) noexcept {
        if (bdp == 0) return 0;
        const auto baseline = bdp / FlightHeadroomDivisor;
        if (!min_rtt_ms || !observed_rtt_ms) return baseline;
        const auto min_rtt = *min_rtt_ms;
        if (min_rtt == 0 || *observed_rtt_ms <= min_rtt) return baseline;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        const auto scaled = *observed_rtt_ms > maximum / 1000
            ? maximum : *observed_rtt_ms * 1000;
        const auto reversal_permille =
            std::min<std::uint64_t>(scaled / min_rtt, MaxAckReversalPermille);
        if (reversal_permille <= 1000) return baseline;
        const auto excess_permille = reversal_permille - 1000;
        const auto expanded = bdp > maximum / excess_permille
            ? maximum : bdp * excess_permille;
        return std::max(baseline, expanded / 1000);
    }

    static std::uint64_t AdaptiveFlightCredit(std::uint64_t session_window,
        std::size_t live_carriers, std::uint64_t bdp, std::uint64_t inflight,
        unsigned weight, std::uint64_t startup_window, std::uint64_t minimum_window,
        std::uint64_t discovery_budget_bytes = 0,
        std::optional<std::uint64_t> min_rtt_ms = {},
        std::optional<std::uint64_t> observed_rtt_ms = {}) noexcept {
        const auto share = session_window / std::max<std::size_t>(1, live_carriers);
        // A known BDP is the flight needed to keep the pipe full, so the target
        // is that BDP plus derived headroom; only an unknown (zero) BDP falls
        // back to the startup window. The session share still bounds the target
        // when a carrier's BDP exceeds its fair share of the retransmit window.
        const bool bdp_known = bdp != 0;
        const auto headroom = bdp_known
            ? FlightHeadroomBytes(bdp, min_rtt_ms, observed_rtt_ms) : 0;
        const auto flight_target = bdp_known
            ? SaturatingAdd(bdp, headroom) : 0;
        const auto target = std::min(share, std::max(startup_window, flight_target));
        const auto bounded_weight = std::min(100u, weight);
        const auto weighted = (target / 100) * bounded_weight +
            (target % 100) * bounded_weight / 100;
        // Weighting may share a carrier down, but discovery still needs the
        // measured target (BDP plus reversal headroom) as its floor. Without
        // it, a carrier at exactly its provisional BDP receives zero credit
        // and can never generate a larger sample or probe its real capacity.
        // The floor also never drops below the startup window: a known-but-tiny
        // BDP (a low capacity carrier measures roughly one 64 KiB frame per
        // RTT) would otherwise quantize the credit to a single frame, so
        // delivery never grows a sample and the estimator locks at the low
        // estimate.
        const auto credit_floor = std::max(minimum_window,
            std::max(startup_window, flight_target));
        const auto limit = std::min(share, std::max(credit_floor, weighted));
        // A capacity probe is the one grant allowed above the provisional BDP
        // target: that target is the estimate under test, so capping the burst
        // at it would feed the estimator only its own ceiling. The allowance is
        // the probe's remaining burst budget and the session share remains the
        // hard ceiling, so the extra flight disappears when the burst is spent.
        // Callers pass zero for a mature path whose delivery already supports
        // its capacity model, which keeps this from becoming a permanent grant.
        const auto discovery_limit = discovery_budget_bytes != 0 && bdp_known
            ? std::min(share, SaturatingAdd(limit, discovery_budget_bytes)) : limit;
        return discovery_limit > inflight ? discovery_limit - inflight : 0;
    }

    // ---- Borrowed flight-credit plan -------------------------------------
    // AdaptiveFlightCredit() above caps every carrier at the equal session
    // share. On heterogeneous paths that gives a high-BDP carrier no more than
    // session_window / live_carriers even when the other carriers cannot use
    // their share: r49 path3 held ~11.14 MiB == 32 MiB / 3 of flight while
    // path1 and path2 left their shares idle.
    //
    // The plan below keeps that equal share as the base allocation, then lets
    // a carrier with measured demand above its share borrow only the idle part
    // of the other eligible carriers' shares. A lender's idle share is
    // max(0, share - inflight_bytes), so outstanding flight is never lent
    // twice. A borrow is additionally bounded by the borrower's demand above
    // its share and by the remaining session window. Unknown or stale
    // capacity cannot justify more than the equal share, so such a carrier
    // never borrows. Lending is stateless: a carrier that starts sending
    // raises its own inflight and automatically reduces what it can lend.
    //
    // Demand is per carrier: max(startup, minimum, BDP + RTT headroom) when a
    // fresh capacity sample backs the BDP, and the equal session share
    // (session_window / live_carriers, never below the startup/minimum floor)
    // when capacity is unknown or stale, matching the legacy equal-share
    // fallback. The sum of the targets never exceeds the session window, so
    // returning unused capacity to a busy path cannot overrun the retransmit
    // window. Discovery is a separate grant taken only from session capacity
    // no eligible target claimed; it is bounded by both the caller's remaining
    // probe budget and that unclaimed capacity, so a probe can never become a
    // permanent extra share or push the total over the cap.
    //
    // The plan is stateless, so carrier join/leave is just a different request
    // list. An avoided/draining/unavailable carrier is excluded from the
    // demand total and receives a zero allocation, but its supplied outstanding
    // flight still reserves session capacity. Discovery and borrowing reserve
    // max(base target, outstanding flight) for each eligible carrier: a reduced
    // estimate cannot lend bytes that have not yet been acknowledged.
    // Arithmetic saturates, and
    // a plan that cannot be built (null buffers, no requests, or more than
    // MaxCandidates) returns 0 so callers keep their local credit policy.
    struct FlightCreditRequest final {
        std::uint16_t id = 0;
        // False for an avoided/draining/unavailable carrier: excluded from
        // the demand total and allocated zero credit.
        bool eligible = false;
        // True only when bdp_bytes came from a fresh capacity sample. An
        // expired or missing sample cannot justify demand above the equal
        // share, so the plan falls back to that share (never below the
        // startup/minimum floor) instead of trusting an unverified estimate.
        bool capacity_known = false;
        std::uint64_t bdp_bytes = 0;
        std::uint64_t inflight_bytes = 0;
        std::optional<std::uint64_t> min_rtt_ms;
        std::optional<std::uint64_t> observed_rtt_ms;
        std::uint64_t discovery_budget_bytes = 0;
        // True only when the send path will actually apply the adaptive plan
        // to this carrier. The production path keeps local credit until a
        // carrier reaches its measured BDP (so discovery can fill it), so a
        // carrier that is not yet at capacity must not reserve window slack as
        // a borrower it would never use. Pure callers default to true.
        bool borrow_eligible = true;
    };

    struct FlightCreditAllocation final {
        std::uint16_t id = 0;
        bool eligible = false;
        bool capacity_known = false;
        std::uint64_t demand_bytes = 0;
        // Base share plus borrow; never greater than demand_bytes or the
        // session window.
        std::uint64_t target_bytes = 0;
        // Separately bounded probe grant. Zero unless the plan has unclaimed
        // session capacity and the carrier asked for discovery.
        std::uint64_t discovery_bytes = 0;
        // max(0, target_bytes + discovery_bytes - inflight_bytes).
        std::uint64_t credit_bytes = 0;
        // Equal session share used as the base target before borrowing.
        std::uint64_t share_bytes = 0;
        // Idle share borrowed from other eligible carriers. Zero for unknown
        // or stale capacity, for carriers whose demand fits their share, and
        // when no peer has unused share or session window left.
        std::uint64_t borrow_bytes = 0;
    };

    static std::uint64_t FlightDemandBytes(std::uint64_t bdp_bytes,
        bool capacity_known, std::uint64_t startup_window,
        std::uint64_t minimum_window,
        std::optional<std::uint64_t> min_rtt_ms = {},
        std::optional<std::uint64_t> observed_rtt_ms = {}) noexcept {
        const auto floor = std::max(startup_window, minimum_window);
        if (!capacity_known || bdp_bytes == 0) return floor;
        const auto flight_target = SaturatingAdd(bdp_bytes,
            FlightHeadroomBytes(bdp_bytes, min_rtt_ms, observed_rtt_ms));
        return std::max(floor, flight_target);
    }

    // Adapts one carrier's telemetry to a plan request. Using the same
    // freshness rule as the scheduler projection means the send path and the
    // runtime snapshot cannot disagree about which carriers have usable
    // capacity.
    static FlightCreditRequest MakeFlightCreditRequest(std::uint16_t id,
        const PathTelemetry& telemetry, bool eligible, std::uint64_t now_ms,
        std::uint64_t discovery_budget_bytes = 0) noexcept {
        FlightCreditRequest request;
        request.id = id;
        request.eligible = eligible;
        request.capacity_known = eligible &&
            CapacityIsFresh(telemetry, now_ms) && telemetry.bdp_bytes != 0;
        request.bdp_bytes = request.capacity_known ? telemetry.bdp_bytes : 0;
        request.inflight_bytes = telemetry.wire_inflight_bytes;
        if (telemetry.min_rtt_ms != 0) {
            request.min_rtt_ms = telemetry.min_rtt_ms;
        }
        if (telemetry.srtt_ms != 0) {
            request.observed_rtt_ms = telemetry.srtt_ms;
        }
        request.discovery_budget_bytes = discovery_budget_bytes;
        return request;
    }

    // Standalone pure allocator for production wiring. Allocates the session
    // window across demands, subtracts outstanding flight, and returns the
    // total allocated (always <= session_window). A zero demand means the
    // caller has no capacity estimate for that carrier; it falls back to an
    // equal share of the session window so an unmeasured carrier is never
    // starved and the arithmetic cannot wrap. `inflights` may be null.
    static std::uint64_t AllocateFlightCredits(std::uint64_t session_window,
        const std::uint64_t* demands, const std::uint64_t* inflights,
        std::size_t count, std::uint64_t* credits) noexcept {
        if (!demands || !credits || count == 0 || count > MaxCandidates) {
            return 0;
        }
        std::uint64_t effective[MaxCandidates] = {};
        const auto equal_share = session_window / count;
        for (std::size_t i = 0; i < count; ++i) {
            effective[i] = demands[i] != 0 ? demands[i] : equal_share;
        }
        std::uint64_t targets[MaxCandidates] = {};
        const auto allocated = AllocateTargets(
            session_window, effective, count, targets);
        for (std::size_t i = 0; i < count; ++i) {
            const auto inflight = inflights ? inflights[i] : 0;
            credits[i] = targets[i] > inflight ? targets[i] - inflight : 0;
        }
        return allocated;
    }

    // Returns the number of allocations written, or 0 when no plan could be
    // built and the caller must fall back to local/legacy credit.
    static std::size_t BuildFlightCreditPlan(std::uint64_t session_window,
        const FlightCreditRequest* requests, std::size_t count,
        std::uint64_t startup_window, std::uint64_t minimum_window,
        FlightCreditAllocation* allocations) noexcept {
        if (!requests || !allocations || count == 0 || count > MaxCandidates) {
            return 0;
        }
        std::fill_n(allocations, count, FlightCreditAllocation{});
        std::size_t live = 0;
        for (std::size_t i = 0; i < count; ++i) {
            allocations[i].id = requests[i].id;
            allocations[i].eligible = requests[i].eligible;
            if (requests[i].eligible) ++live;
        }
        if (live == 0) return count;

        const auto share = session_window / live;
        const auto floor = FlightDemandBytes(0, false, startup_window,
            minimum_window);
        std::uint64_t inflights[MaxCandidates] = {};
        std::uint64_t unused[MaxCandidates] = {};
        std::uint64_t reserved_total = 0;
        std::uint64_t total_unused = 0;
        for (std::size_t i = 0; i < count; ++i) {
            FlightCreditAllocation& allocation = allocations[i];
            const FlightCreditRequest& request = requests[i];
            if (!allocation.eligible) {
                // Draining/avoided flight still occupies the session window
                // even though the carrier receives no new allocation.
                reserved_total = SaturatingAdd(reserved_total, request.inflight_bytes);
                continue;
            }
            allocation.capacity_known =
                request.capacity_known && request.bdp_bytes != 0;
            // A known capacity sample supplies the demand above the equal
            // share. An unknown or stale estimate cannot justify borrowing, so
            // it falls back to the equal share (never below the startup/
            // minimum floor) and is treated as a non-borrower below.
            allocation.demand_bytes = allocation.capacity_known
                ? FlightDemandBytes(request.bdp_bytes, true, startup_window,
                    minimum_window, request.min_rtt_ms,
                    request.observed_rtt_ms)
                : std::max(floor, share);
            allocation.share_bytes = share;
            // Base allocation is the equal share, further capped by demand.
            // A low-demand carrier therefore leaves its unused portion of the
            // window available to a high-demand peer.
            allocation.target_bytes = std::min(allocation.demand_bytes, share);
            inflights[i] = request.inflight_bytes;
            // Only the part of a share not already in flight can be lent;
            // an over-full carrier has nothing idle.
            unused[i] = share > request.inflight_bytes
                ? share - request.inflight_bytes : 0;
            // A capacity decrease can put existing flight above its new
            // target. That overhang cannot be returned to another carrier
            // until ACKs release it; reserving only the target lends it twice.
            reserved_total = SaturatingAdd(reserved_total,
                std::max(allocation.target_bytes, request.inflight_bytes));
            total_unused = SaturatingAdd(total_unused, unused[i]);
        }

        // Discovery is a separate, caller-bounded probe grant. Fund it before
        // borrowing from the same slack so an ordinary probe cannot be starved
        // by a peer's bulk demand; both paths are still capped by the session
        // window. Process carriers by ascending id so the split does not
        // depend on the order of the request list.
        std::uint64_t remaining = session_window > reserved_total
            ? session_window - reserved_total : 0;
        std::uint64_t total_discovery = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (!allocations[i].eligible) continue;
            total_discovery = SaturatingAdd(total_discovery,
                requests[i].discovery_budget_bytes);
        }
        if (remaining != 0 && total_discovery != 0) {
            const auto discovery_capacity = remaining;
            bool served[MaxCandidates] = {};
            for (;;) {
                std::size_t pick = count;
                std::uint16_t pick_id = std::numeric_limits<std::uint16_t>::max();
                for (std::size_t i = 0; i < count; ++i) {
                    if (!allocations[i].eligible || served[i] ||
                        requests[i].discovery_budget_bytes == 0) continue;
                    if (requests[i].id < pick_id) {
                        pick_id = requests[i].id;
                        pick = i;
                    }
                }
                if (pick == count || remaining == 0) break;
                served[pick] = true;
                FlightCreditAllocation& allocation = allocations[pick];
                const auto requested = requests[pick].discovery_budget_bytes;
                const auto grant = std::min(requested,
                    MulDiv(discovery_capacity, requested, total_discovery));
                allocation.discovery_bytes = std::min(grant, remaining);
                remaining -= allocation.discovery_bytes;
            }
        }

        // Borrow only measured demand above the equal share, and only from
        // the idle share of other eligible carriers. A borrower may never
        // consume its own unused share, the borrow pool, the remaining session
        // window, or more than its own unmet demand.
        std::uint64_t borrow_pool = total_unused;
        bool served_borrower[MaxCandidates] = {};
        for (;;) {
            std::size_t pick = count;
            std::uint16_t pick_id = std::numeric_limits<std::uint16_t>::max();
            std::uint64_t pick_need = 0;
            for (std::size_t i = 0; i < count; ++i) {
                FlightCreditAllocation& allocation = allocations[i];
                if (!allocation.eligible || served_borrower[i] ||
                    !allocation.capacity_known || !requests[i].borrow_eligible ||
                    allocation.demand_bytes <= share) continue;
                const auto need = allocation.demand_bytes - share;
                if (pick == count || need > pick_need ||
                    (need == pick_need && allocation.id < pick_id)) {
                    pick = i;
                    pick_id = allocation.id;
                    pick_need = need;
                }
            }
            if (pick == count || remaining == 0 || borrow_pool == 0) break;
            served_borrower[pick] = true;
            FlightCreditAllocation& allocation = allocations[pick];
            const auto own_unused = unused[pick];
            const auto peer_unused = total_unused > own_unused
                ? total_unused - own_unused : 0;
            const auto needed = allocation.demand_bytes - share;
            const auto grant = std::min(needed,
                std::min(std::min(borrow_pool, peer_unused), remaining));
            allocation.borrow_bytes = grant;
            allocation.target_bytes = SaturatingAdd(share, grant);
            remaining -= grant;
            borrow_pool -= grant;
        }

        for (std::size_t i = 0; i < count; ++i) {
            FlightCreditAllocation& allocation = allocations[i];
            if (!allocation.eligible) continue;
            const auto target = SaturatingAdd(allocation.target_bytes,
                allocation.discovery_bytes);
            allocation.credit_bytes = target > inflights[i]
                ? target - inflights[i] : 0;
        }
        return count;
    }

    // Capacity is usable only while the sample that produced it is fresh.
    // The RTT/metrics timestamp (updated_ms) is deliberately not reused here:
    // delivery samples can stay fresh long after the capacity estimate expires.
    static bool CapacityIsFresh(const PathTelemetry& telemetry,
                                std::uint64_t now_ms) noexcept {
        return telemetry.capacity_rate_bytes_per_second != 0 &&
            telemetry.capacity_updated_ms != 0 &&
            telemetry.capacity_updated_ms <= now_ms &&
            now_ms - telemetry.capacity_updated_ms <= MetricsMaxAgeMs;
    }

    // A gap exists when delivery is at least a fifth below the capacity
    // estimate. This is the only evidence that the estimate itself may be the
    // ceiling: a carrier sitting at its measured capacity needs no probe, while
    // one that keeps delivering materially less is under-filling its own model.
    // The probe gate and the flight-credit discovery allowance share this
    // definition so they cannot disagree about when the estimate is in doubt.
    static bool CapacityGapNeeded(std::uint64_t capacity_rate,
                                  std::uint64_t delivery_rate) noexcept {
        return capacity_rate != 0 && delivery_rate != 0 &&
            delivery_rate < capacity_rate &&
            delivery_rate <= capacity_rate - capacity_rate / 5;
    }

    // Telemetry form used by the send-credit path (which has no Candidate yet).
    // at_capacity is produced only from a fresh capacity sample, so an expired
    // estimate can never unlock discovery credit.
    static bool CapacityProbeNeeded(const PathTelemetry& telemetry) noexcept {
        return telemetry.at_capacity &&
            CapacityGapNeeded(telemetry.capacity_rate_bytes_per_second,
                telemetry.current_rate_bytes_per_second);
    }

    // Bounded per-carrier discovery allowance used by the VMUX send-credit
    // path. A published probe remainder is a grant only while this carrier is
    // the scheduler's current probe owner: the send loop republishes the
    // remainder on the owner, but a burst that was cleared (owner moved,
    // carrier became ineligible, generation changed) leaves that carrier's
    // cached value untouched. Treating the stale cache as a live commitment
    // would keep granting the default budget forever, subtracting it from the
    // session window and from every real borrower. A fresh capacity gap is the
    // only evidence that justifies the default budget without an owner. The
    // result is capped by the per-link high water so discovery never adds more
    // than one write window per calculation.
    static std::uint64_t DiscoveryAllowanceBytes(bool is_probe_owner,
        std::uint64_t owner_remaining_bytes, bool capacity_probe_needed,
        std::uint64_t default_burst_bytes, std::uint64_t high_water_bytes) noexcept {
        const std::uint64_t remaining =
            is_probe_owner ? owner_remaining_bytes : 0;
        if (remaining == 0 && !capacity_probe_needed) return 0;
        const std::uint64_t budget =
            remaining != 0 ? remaining : default_burst_bytes;
        return std::min(budget, high_water_bytes);
    }

    // ---- Per-link write high-water ----------------------------------------
    // VMUX's per-carrier local write queue needs to hold roughly one BDP so a
    // fresh scheduler credit does not run out while the socket drains. The
    // fixed 256 KiB window is below the BDP of a fast or high-RTT carrier, so
    // local credit can be exhausted while the socket is still draining and
    // the scheduler then observes queue-limited starvation even though the
    // carrier itself is healthy. This policy derives a bounded window from a
    // fresh capacity sample; it does not change the session window or ACK
    // flight.
    //
    // Invariants:
    // - No fresh capacity sample means no estimate. The result is exactly
    //   LinkWriteHighWaterFloorBytes; an earlier expanded window is not kept.
    // - A fresh capacity sample is usable only together with a nonzero min
    //   RTT. The target is clamp(capacity * min_rtt / 1000, floor, ceiling).
    //   The observed srtt and jitter are deliberately excluded: using
    //   queueing-delayed RTT would grow the very queue that creates it.
    // - The ceiling is a fixed hard bound, not a second estimate. The r59
    //   multi-path run showed path3's BDP at ~6.7 MiB (300 Mbps x ~187 ms) and
    //   path2's at ~3.1 MiB (200 Mbps x ~130 ms); a 2 MiB ceiling clipped both
    //   and left the local queue below one BDP, so credit ran out while the
    //   socket was still draining. 8 MiB is the smallest power-of-two bound
    //   that clears 6.7 MiB with ~20% headroom for capacity/RTT sample noise,
    //   while still bounding three concurrent carriers to 24 MiB of local
    //   queue. It is not a return to the old unconditional 8 MiB window: the
    //   floor stays at 256 KiB and only a fresh capacity + min_rtt sample can
    //   raise the target, so unknown or stale telemetry still yields the floor.
    //   This ceiling sizes only the local write queue; it does not change the
    //   scheduler's session window, ACK flight, or probe/discovery budgets.
    // - Growth is rate-limited. Callers pass the previous window and the tick
    //   of the previous observation; the returned window advances by at most
    //   LinkWriteHighWaterGrowthBytesPerSecond over the elapsed time, and a
    //   long gap is clipped to LinkWriteHighWaterMaxGrowthIntervalMs so a
    //   stale period cannot bank a step and jump to the ceiling.
    //   The rate is ceiling/4 per second: a full 8 MiB ramp takes 4 s (11% of
    //   a 36 s warm-up) and the ~6.7 MiB path3 target takes ~3.2 s, leaving
    //   margin for coalesced ticks and fitting inside the 5 s
    //   MetricsMaxAgeMs freshness window, so a ramp can complete before an
    //   unrefreshed sample expires. Even a 1 Hz caller still reaches 8 MiB in
    //   ~16 s. One 250 ms tick adds at most 512 KiB (ceiling/16), so the local
    //   queue grows in bounded steps rather than accepting a whole BDP at once.
    // - A fresh target below the current window shrinks immediately: a smaller
    //   measured BDP must not leave an oversized local queue behind. This also
    //   means a noisy downward capacity sample never keeps a bigger guess.
    // - Backpressure is strict: a queue at or above the window yields zero
    //   credit. There is no "one oversize frame" exception; the floor is
    //   larger than the maximum VMUX payload frame, so the normal path never
    //   needs one.
    //
    // The function is pure. The caller owns high_water_bytes and its update
    // tick per carrier, and should store now_ms after every call (even when
    // the window did not change) so the growth budget reflects wall time.
    static constexpr std::uint64_t LinkWriteHighWaterFloorBytes = 256 * 1024;
    static constexpr std::uint64_t LinkWriteHighWaterCeilingBytes =
        8 * 1024 * 1024;
    static constexpr std::uint64_t LinkWriteHighWaterGrowthBytesPerSecond =
        LinkWriteHighWaterCeilingBytes / 4;
    static constexpr std::uint64_t LinkWriteHighWaterMaxGrowthIntervalMs = 250;
    // A single 250 ms growth step; kept below one VMUX BDP-sized burst.
    static constexpr std::uint64_t LinkWriteHighWaterMaxGrowthStepBytes =
        LinkWriteHighWaterGrowthBytesPerSecond *
        LinkWriteHighWaterMaxGrowthIntervalMs / 1000;

    struct LinkWriteWindow final {
        // Conservative default for an unobserved or expired carrier.
        std::uint64_t high_water_bytes = LinkWriteHighWaterFloorBytes;
        // Fresh BDP target clamped to [floor, ceiling], or the floor when the
        // capacity sample is unknown/expired. high_water_bytes may trail this
        // while growth is rate-limited.
        std::uint64_t target_bytes = LinkWriteHighWaterFloorBytes;
        // Space left before the window is full; a caller admits a frame only
        // when this is at least the frame length. Zero under backpressure.
        std::uint64_t credit_bytes = 0;
        // True when queued_bytes >= high_water_bytes.
        bool backpressure = false;
        // True only when a fresh capacity sample and min RTT produced target.
        bool capacity_known = false;
    };

    static LinkWriteWindow ComputeLinkWriteWindow(
        const PathTelemetry& telemetry, std::uint64_t now_ms,
        std::uint64_t previous_high_water_bytes,
        std::uint64_t previous_updated_ms,
        std::uint64_t queued_bytes = 0) noexcept {
        LinkWriteWindow window;
        const bool capacity_fresh = CapacityIsFresh(telemetry, now_ms);
        if (capacity_fresh && telemetry.min_rtt_ms != 0) {
            window.capacity_known = true;
            const auto bdp = MulDiv(telemetry.capacity_rate_bytes_per_second,
                telemetry.min_rtt_ms, 1000);
            window.target_bytes = std::max(LinkWriteHighWaterFloorBytes,
                std::min(LinkWriteHighWaterCeilingBytes, bdp));
            const auto previous = previous_high_water_bytes == 0
                ? LinkWriteHighWaterFloorBytes
                : std::max(LinkWriteHighWaterFloorBytes,
                    std::min(LinkWriteHighWaterCeilingBytes,
                        previous_high_water_bytes));
            if (window.target_bytes <= previous) {
                // Shrinking is immediate: the fresh sample is the best
                // evidence, and an oversized window would keep charging the
                // queueing delay that the sample is trying to measure.
                window.high_water_bytes = window.target_bytes;
            } else {
                const auto elapsed = previous_updated_ms != 0 &&
                    now_ms > previous_updated_ms
                    ? now_ms - previous_updated_ms : 0;
                const auto growth = MulDiv(
                    LinkWriteHighWaterGrowthBytesPerSecond,
                    std::min(elapsed, LinkWriteHighWaterMaxGrowthIntervalMs),
                    1000);
                window.high_water_bytes = std::min(window.target_bytes,
                    SaturatingAdd(previous, growth));
            }
        }
        // Unknown or expired capacity falls through with the floor and no
        // retained expansion. This includes telemetry.bdp_bytes:
        // get_link_telemetry may fill it from a delivery sample after the
        // capacity sample expired, and that fallback is scheduling evidence,
        // not a fresh capacity estimate for sizing the local write queue.
        window.backpressure = queued_bytes >= window.high_water_bytes;
        window.credit_bytes = window.backpressure
            ? 0 : window.high_water_bytes - queued_bytes;
        return window;
    }


    // Adapts the control-plane snapshot to the legacy scheduler contract.
    // Callers retain ownership of the carrier id and send-credit policy.
    static Candidate ProjectCandidate(std::uint16_t id,
                                      const PathControlSnapshot& snapshot,
                                      std::uint64_t now_ms,
                                      std::optional<std::uint64_t> send_credit = {},
                                      bool preferred = false) noexcept {
        Candidate candidate;
        candidate.id = id;
        candidate.preferred = preferred;
        candidate.send_credit_bytes = send_credit;
        candidate.queue_bytes = snapshot.telemetry.local_queue_bytes;
        candidate.unacked_bytes = snapshot.telemetry.wire_inflight_bytes;
        candidate.last_active_ms = snapshot.telemetry.updated_ms;
        candidate.metrics_updated_ms = snapshot.telemetry.updated_ms;
        candidate.probe_eligible = snapshot.capabilities.active_probe &&
            IsEligibleState(snapshot.state);
        candidate.at_capacity = snapshot.telemetry.at_capacity;
        candidate.state = ToCandidateState(snapshot.state);
        candidate.eligible = IsEligibleState(snapshot.state) &&
            snapshot.last_failure != FailureClass::InvalidTransition;

        // Confidence is part of freshness: an old or untrusted sample must
        // fall back to the scheduler's conservative estimates.
        const bool fresh = snapshot.telemetry.has_sample &&
            snapshot.telemetry.updated_ms != 0 &&
            snapshot.telemetry.updated_ms <= now_ms &&
            now_ms - snapshot.telemetry.updated_ms <= MetricsMaxAgeMs &&
            snapshot.telemetry.confidence > 0;
        if (fresh) {
            if (snapshot.telemetry.srtt_ms != 0) {
                candidate.rtt_ms = snapshot.telemetry.srtt_ms;
            } else if (snapshot.telemetry.min_rtt_ms != 0) {
                candidate.rtt_ms = snapshot.telemetry.min_rtt_ms;
            }
            if (snapshot.telemetry.current_rate_bytes_per_second != 0) {
                candidate.delivery_rate_bytes_per_second =
                    snapshot.telemetry.current_rate_bytes_per_second;
            }
            // A fresh capacity sample outranks the newest delivery sample, but
            // the capability flag describes the producer, not the age of one
            // estimate. A producer that only advertises delivery rates while
            // its capacity sample is fresh must not turn an expired capacity
            // into an unknown-throughput path: a fresh delivery sample is
            // itself evidence of a usable rate for the scheduler.
            const bool capacity_fresh = CapacityIsFresh(snapshot.telemetry, now_ms);
            candidate.capacity_known = capacity_fresh;
            if (snapshot.capabilities.payload_delivery_rate ||
                candidate.delivery_rate_bytes_per_second.has_value()) {
                const auto rate = capacity_fresh
                    ? snapshot.telemetry.capacity_rate_bytes_per_second
                    : snapshot.telemetry.current_rate_bytes_per_second;
                if (rate != 0) candidate.throughput_bytes_per_second = rate;
            }
        }
        return candidate;
    }

    static Candidate CandidateFromSnapshot(std::uint16_t id,
                                           const PathControlSnapshot& snapshot,
                                           std::uint64_t now_ms,
                                           std::optional<std::uint64_t> send_credit = {},
                                           bool preferred = false) noexcept {
        return ProjectCandidate(id, snapshot, now_ms, send_credit, preferred);
    }

    Selection Select(Policy policy, const std::vector<Candidate>& candidates,
                    std::uint64_t now_ms, std::uint64_t payload_bytes) noexcept {
        if (probe_diagnostics_enabled_) probe_diagnostics_ = {};
        if (candidates.size() > MaxCandidates ||
            (policy != Policy::Adaptive && policy != Policy::Failover) ||
            HasDuplicateIds(candidates)) {
            return {Status::InvalidInput, 0, 0, false};
        }
        PruneTrackers(candidates);
        if (policy == Policy::Failover) return SelectFailover(candidates, payload_bytes);
        return SelectAdaptive(candidates, now_ms, payload_bytes);
    }

    void Reset() noexcept {
        trackers_ = {};
        probe_diagnostics_ = {};
        last_probe_ms_ = 0;
        probe_owner_active_ = false;
        probe_owner_id_ = 0;
        probe_owner_generation_ = 0;
        probe_clock_started_ = false;
    }

    std::uint64_t ProbeRemainingBytes(const Candidate& candidate) const noexcept {
        for (const Tracker& tracker : trackers_) {
            if (tracker.used && tracker.id == candidate.id &&
                tracker.generation == candidate.generation) {
                return tracker.probe_remaining_bytes;
            }
        }
        return 0;
    }
    // Bounded virtual service debt of the carrier, as of the most recent
    // Select(). It is derived from the sent-payload counter observed at that
    // call, halves every FairnessDecayHalfLifeMs, and never exceeds
    // FairnessDebtCapMs. Exposed for scheduler diagnostics and regression
    // coverage of the epoch bound.
    std::uint64_t FairnessDebtMs(const Candidate& candidate) const noexcept {
        return FairnessCost(candidate);
    }
    ProbeDiagnostics GetProbeDiagnostics(const Candidate& candidate) const noexcept {
        for (const auto& entry : probe_diagnostics_) {
            if (entry.used && entry.id == candidate.id &&
                entry.generation == candidate.generation) return entry.value;
        }
        return {};
    }

    ProbeRuntime GetProbeRuntime() const noexcept {
        return {probe_interval_ms_, last_probe_ms_, probe_clock_started_,
            probe_owner_active_, probe_owner_id_};
    }
    // Byte budget the scheduler assigns to one probe burst. Send-credit callers
    // use it as the bounded discovery allowance while a capacity gap exists.
    std::uint64_t ProbeBurstBytes() const noexcept { return probe_burst_bytes_; }
    // Byte budget the scheduler assigns to one at-capacity best-carrier plateau
    // probe. Zero disables plateau probes.
    std::uint64_t PlateauProbeBudgetBytes() const noexcept {
        return plateau_probe_budget_bytes_;
    }
    void SetProbeDiagnosticsEnabled(bool enabled) noexcept {
        probe_diagnostics_enabled_ = enabled;
        if (!enabled) probe_diagnostics_ = {};
    }

private:
    static CandidateState ToCandidateState(PathState state) noexcept {
        switch (state) {
        case PathState::Warming:
        case PathState::Probing:
            return CandidateState::Warming;
        case PathState::Degraded:
            return CandidateState::Degraded;
        case PathState::Draining:
        case PathState::Retiring:
            return CandidateState::Draining;
        case PathState::Active:
            return CandidateState::Active;
        default:
            return CandidateState::Unavailable;
        }
    }

    static bool IsEligibleState(PathState state) noexcept {
        return state == PathState::Active || state == PathState::Warming ||
            state == PathState::Probing || state == PathState::Degraded;
    }

    struct Tracker final {
        std::uint16_t id = 0;
        std::uint64_t last_selected_ms = 0;
        // Time the last probe payload was sent on this path. Ordinary
        // selections never advance it: a carrier that is picked every few
        // milliseconds would otherwise keep resetting its own probe recency
        // and could never start the next probe. Probe start, probe-owner
        // continuation, and the probe recency/tie-break use this clock.
        std::uint64_t last_probe_ms = 0;
        std::uint64_t probe_remaining_bytes = 0;
        // Total budget assigned to the active burst. Plateau bursts cap the
        // nominal probe budget, and continuation must report the same total.
        std::uint64_t probe_budget_bytes = 0;
        std::uint64_t generation = 0;
        // Bounded epoch accounting: diff the monotonic sent-payload counter,
        // price each delta at the current rate, decay the accumulated virtual
        // service time, and cap it. Never sum the session counter itself.
        std::uint64_t fairness_observed_bytes = 0;
        std::uint64_t fairness_updated_ms = 0;
        double fairness_debt_ms = 0.0;
        // Rate at which the stored debt is currently priced. A corrected
        // capacity estimate re-prices the bounded epoch, which is what breaks
        // the early-underestimate feedback loop.
        double fairness_priced_rate = 0.0;
        bool fairness_baseline_known = false;
        bool used = false;
    };
    struct ProbeDiagnosticEntry final {
        std::uint16_t id = 0;
        std::uint64_t generation = 0;
        ProbeDiagnostics value{};
        bool used = false;
    };

    static bool HasDuplicateIds(const std::vector<Candidate>& candidates) noexcept {
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            for (std::size_t j = i + 1; j < candidates.size(); ++j) {
                if (candidates[i].id == candidates[j].id) return true;
            }
        }
        return false;
    }

    static bool HasCredit(const Candidate& candidate, std::uint64_t payload_bytes) noexcept {
        return candidate.send_credit_bytes && *candidate.send_credit_bytes >= payload_bytes;
    }

    static bool CanCarry(const Candidate& candidate) noexcept {
        return candidate.state != CandidateState::Draining &&
            candidate.state != CandidateState::Unavailable;
    }

    static bool CanFailover(const Candidate& candidate) noexcept {
        return candidate.state == CandidateState::Active ||
            candidate.state == CandidateState::Warming;
    }

    static bool CanContinueProbe(const Candidate& candidate) noexcept {
        return candidate.probe_eligible &&
            (candidate.state == CandidateState::Active ||
             candidate.state == CandidateState::Warming);
    }

    static std::uint64_t SaturatingAdd(std::uint64_t a, std::uint64_t b) noexcept {
        const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
        return b > maximum - a ? maximum : a + b;
    }

    // floor(value * multiplier / divisor) with a saturating, non-wrapping
    // intermediate. Byte budgets are normally far below the 64-bit product
    // range; the wide fallback only runs for pathological values (a measured
    // or misconfigured BDP near the integer limit) and clamps instead of
    // wrapping.
    static std::uint64_t MulDiv(std::uint64_t value, std::uint64_t multiplier,
                                std::uint64_t divisor) noexcept {
        if (value == 0 || multiplier == 0 || divisor == 0) return 0;
        const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
        if (multiplier <= maximum / value) {
            return (value * multiplier) / divisor;
        }
        const long double result = static_cast<long double>(value) *
            static_cast<long double>(multiplier) /
            static_cast<long double>(divisor);
        if (!(result > 0.0L)) return 0;
        if (result >= static_cast<long double>(maximum)) return maximum;
        return static_cast<std::uint64_t>(result);
    }

    // Shared by the standalone allocator and the telemetry-driven plan. Each
    // demand is capped at its own value and the running total is capped at the
    // session window, so no rounding path can oversubscribe the window.
    static std::uint64_t AllocateTargets(std::uint64_t session_window,
        const std::uint64_t* demands, std::size_t count,
        std::uint64_t* targets) noexcept {
        if (!demands || !targets || count == 0 || count > MaxCandidates) {
            return 0;
        }
        std::uint64_t total_demand = 0;
        for (std::size_t i = 0; i < count; ++i) {
            total_demand = SaturatingAdd(total_demand, demands[i]);
        }
        std::uint64_t allocated = 0;
        for (std::size_t i = 0; i < count; ++i) {
            auto target = total_demand == 0
                ? 0 : std::min(demands[i],
                    MulDiv(session_window, demands[i], total_demand));
            target = allocated >= session_window
                ? 0 : std::min(target, session_window - allocated);
            targets[i] = target;
            allocated = SaturatingAdd(allocated, target);
        }
        return allocated;
    }

    static bool NeedsCapacityProbe(const Candidate& candidate) noexcept {
        if (!candidate.capacity_known || !candidate.delivery_rate_bytes_per_second ||
            !candidate.throughput_bytes_per_second ||
            *candidate.throughput_bytes_per_second == 0) return false;
        return CapacityGapNeeded(*candidate.throughput_bytes_per_second,
            *candidate.delivery_rate_bytes_per_second);
    }

    // A plateau probe is restricted to the ordinary best carrier: re-testing a
    // non-best carrier is what the normal fairness-ordered probe slot already
    // does, while redirecting ordinary payload away from best would distort the
    // share. The delivery-at-capacity condition (no gap) is the case the normal
    // gap gate skips, so the estimate under test is not yet contradicted.
    static bool IsPlateauProbeCandidate(const Candidate& candidate,
                                        std::uint16_t best_id) noexcept {
        if (candidate.id != best_id || !candidate.at_capacity ||
            !candidate.capacity_known ||
            !candidate.throughput_bytes_per_second ||
            !candidate.delivery_rate_bytes_per_second ||
            *candidate.throughput_bytes_per_second == 0 ||
            *candidate.delivery_rate_bytes_per_second == 0) return false;
        return !CapacityGapNeeded(*candidate.throughput_bytes_per_second,
            *candidate.delivery_rate_bytes_per_second);
    }

    static bool CanProbe(const Candidate& candidate, std::uint64_t payload_bytes,
                         bool plateau_allowed) noexcept {
        if (!CanContinueProbe(candidate) ||
            (candidate.at_capacity && !NeedsCapacityProbe(candidate) &&
             !plateau_allowed)) return false;
        return !candidate.probe_budget_bytes || *candidate.probe_budget_bytes >= payload_bytes;
    }

    static std::uint64_t TransferTime(std::uint64_t bytes, std::uint64_t rate) noexcept {
        if (rate == 0) return std::numeric_limits<std::uint64_t>::max();
        const long double ms = std::ceil(static_cast<long double>(bytes) * 1000.0L /
            static_cast<long double>(rate));
        if (ms >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return static_cast<std::uint64_t>(ms);
    }

    static bool MetricsAreFresh(const Candidate& candidate, std::uint64_t now_ms) noexcept {
        return candidate.metrics_updated_ms != 0 && candidate.metrics_updated_ms <= now_ms &&
            now_ms - candidate.metrics_updated_ms <= MetricsMaxAgeMs;
    }

    // Exponential half-life decay of charged virtual service. The fractional
    // milliseconds are kept until the cost is converted, because rounding each
    // frame's service time separately would systematically overcharge fast
    // carriers (a 1500-byte frame at 10 MB/s is 0.15 ms, not 1 ms).
    static double DecayedDebtMs(double debt_ms, std::uint64_t elapsed_ms) noexcept {
        if (!(debt_ms > 0.0)) return 0.0;
        if (elapsed_ms >= FairnessDecayHalfLifeMs * 64) return 0.0;
        return debt_ms * std::exp2(-static_cast<double>(elapsed_ms) /
            static_cast<double>(FairnessDecayHalfLifeMs));
    }

    static std::uint64_t BoundedDebtMs(double debt_ms) noexcept {
        if (!(debt_ms > 0.0)) return 0;
        const double cap = static_cast<double>(FairnessDebtCapMs);
        const double bounded = debt_ms < cap ? debt_ms : cap;
        return static_cast<std::uint64_t>(bounded + 0.5);
    }

    // Diffs the monotonic sent-payload counter into a bounded, decaying epoch.
    // Deltas are priced with the carrier's *current* rate belief, so once a
    // capacity estimate is corrected the stored epoch is re-priced by
    // priced_rate/current_rate instead of pinning the carrier at its earlier
    // (lower) estimate. A counter that moves backwards marks a new session
    // epoch: the old service history is not comparable, so the debt is dropped
    // rather than ratcheted.
    void UpdateFairnessDebt(const std::vector<Candidate>& candidates,
                            std::uint64_t now_ms) noexcept {
        for (const Candidate& candidate : candidates) {
            Tracker* tracker = FindTracker(candidate);
            if (!tracker) continue;

            const bool fresh = MetricsAreFresh(candidate, now_ms) &&
                candidate.throughput_bytes_per_second &&
                *candidate.throughput_bytes_per_second != 0;
            const double rate = fresh
                ? static_cast<double>(*candidate.throughput_bytes_per_second) : 0.0;
            const double cap = static_cast<double>(FairnessDebtCapMs);

            const std::uint64_t counter = candidate.sent_payload_bytes;
            std::uint64_t delta = 0;
            if (!tracker->fairness_baseline_known) {
                tracker->fairness_baseline_known = true;
                tracker->fairness_updated_ms = now_ms;
                delta = counter;
            } else {
                if (now_ms > tracker->fairness_updated_ms) {
                    tracker->fairness_debt_ms = DecayedDebtMs(
                        tracker->fairness_debt_ms,
                        now_ms - tracker->fairness_updated_ms);
                    tracker->fairness_updated_ms = now_ms;
                }
                if (counter < tracker->fairness_observed_bytes) {
                    tracker->fairness_debt_ms = 0.0;
                    delta = 0;
                } else {
                    delta = counter - tracker->fairness_observed_bytes;
                }
            }
            tracker->fairness_observed_bytes = counter;

            if (rate != 0.0) {
                // Re-price the stored epoch at the new belief. The cap is
                // applied afterwards so a downward correction cannot exceed the
                // same bound. This must run even with no new bytes: the common
                // early-underestimate case is a corrected rate with a frozen
                // sent counter.
                if (tracker->fairness_priced_rate != 0.0 &&
                    tracker->fairness_priced_rate != rate) {
                    tracker->fairness_debt_ms = std::min(cap,
                        tracker->fairness_debt_ms * tracker->fairness_priced_rate / rate);
                }
                tracker->fairness_priced_rate = rate;
            }

            if (delta == 0 || rate == 0.0) {
                // Unpriceable bytes are absorbed into the baseline instead of
                // being back-charged later at a different rate belief.
                continue;
            }
            const double service_ms = static_cast<double>(delta) * 1000.0 / rate;
            tracker->fairness_debt_ms = std::min(cap,
                tracker->fairness_debt_ms + service_ms);
        }
    }

    std::uint64_t FairnessCost(const Candidate& candidate) const noexcept {
        for (const Tracker& tracker : trackers_) {
            if (tracker.used && tracker.id == candidate.id &&
                tracker.generation == candidate.generation) {
                return BoundedDebtMs(tracker.fairness_debt_ms);
            }
        }
        return 0;
    }

    static std::uint64_t ActivityPenalty(const Candidate& candidate,
                                         std::uint64_t now_ms) noexcept {
        if (candidate.last_active_ms == 0 || candidate.last_active_ms > now_ms) return 250;
        const std::uint64_t idle_ms = now_ms - candidate.last_active_ms;
        return std::min<std::uint64_t>(idle_ms / 10, 1000);
    }

    static std::uint64_t EstimateArrival(const Candidate& candidate,
        std::uint64_t now_ms, std::uint64_t payload_bytes) noexcept {
        const bool fresh = MetricsAreFresh(candidate, now_ms);
        const std::uint64_t rtt = fresh && candidate.rtt_ms && *candidate.rtt_ms > 0
            ? *candidate.rtt_ms : DefaultUnknownRttMs;
        const std::uint64_t rate = fresh && candidate.throughput_bytes_per_second &&
            *candidate.throughput_bytes_per_second > 0
            ? *candidate.throughput_bytes_per_second : DefaultUnknownThroughputBytesPerSecond;
        auto queued = SaturatingAdd(candidate.queue_bytes, candidate.unacked_bytes);
        std::uint64_t pressure_ms = 0;
        if (candidate.bdp_bytes && *candidate.bdp_bytes != 0) {
            const auto bdp = *candidate.bdp_bytes;
            const auto excess = candidate.unacked_bytes > bdp ? candidate.unacked_bytes - bdp : 0;
            queued = std::max(candidate.queue_bytes, excess);
            const auto reduced_weight = 100 - std::min(100u, candidate.scheduling_weight);
            pressure_ms = SaturatingAdd(candidate.jitter_ms,
                (rtt / 100) * reduced_weight + (rtt % 100) * reduced_weight / 100);
        }
        const std::uint64_t bytes = SaturatingAdd(queued, payload_bytes);
        return SaturatingAdd(SaturatingAdd(SaturatingAdd(rtt, pressure_ms), TransferTime(bytes, rate)),
            ActivityPenalty(candidate, now_ms));
    }

    Selection SelectFailover(const std::vector<Candidate>& candidates,
                             std::uint64_t payload_bytes) const noexcept {
        const Candidate* preferred = nullptr;
        bool any_eligible = false;
        for (const Candidate& candidate : candidates) {
            if (!candidate.eligible || !CanFailover(candidate)) continue;
            any_eligible = true;
            if (candidate.preferred && (!preferred || candidate.id < preferred->id)) {
                preferred = &candidate;
            }
        }
        if (!any_eligible) return {Status::NoEligible, 0, 0, false};
        if (preferred) {
            if (!HasCredit(*preferred, payload_bytes)) {
                return {Status::Backpressure, 0, 0, false};
            }
            return {Status::Selected, preferred->id, 0, false};
        }

        const Candidate* fallback = nullptr;
        for (const Candidate& candidate : candidates) {
            if (candidate.eligible && CanFailover(candidate) &&
                (!fallback || candidate.id < fallback->id)) {
                fallback = &candidate;
            }
        }
        if (!fallback) return {Status::NoEligible, 0, 0, false};
        return HasCredit(*fallback, payload_bytes)
            ? Selection{Status::Selected, fallback->id, 0, false}
            : Selection{Status::Backpressure, 0, 0, false};
    }

    Selection SelectAdaptive(const std::vector<Candidate>& candidates,
                             std::uint64_t now_ms,
                             std::uint64_t payload_bytes) noexcept {
        // Trackers must exist before costing so the service debt advances even
        // for carriers that are currently ineligible or out of credit.
        EnsureTrackers(candidates, now_ms);
        UpdateFairnessDebt(candidates, now_ms);
        bool any_eligible = false;
        const Candidate* best = nullptr;
        std::uint64_t best_cost = std::numeric_limits<std::uint64_t>::max();
        for (const Candidate& candidate : candidates) {
            if (!candidate.eligible || !CanCarry(candidate)) continue;
            any_eligible = true;
            if (!HasCredit(candidate, payload_bytes)) continue;
            const std::uint64_t arrival = EstimateArrival(candidate, now_ms, payload_bytes);
            const std::uint64_t cost = SaturatingAdd(arrival,
                FairnessCost(candidate));
            if (!best || cost < best_cost || (cost == best_cost && candidate.id < best->id)) {
                best = &candidate;
                best_cost = cost;
            }
        }
        if (!best) {
            return {any_eligible ? Status::Backpressure : Status::NoEligible, 0, 0, false};
        }

        if (!probe_clock_started_) {
            probe_clock_started_ = true;
            last_probe_ms_ = now_ms;
        }
        if (probe_diagnostics_enabled_) {
            CaptureProbeDiagnostics(candidates, now_ms, payload_bytes, *best);
        }

        // Once a probe starts, keep a bounded byte budget on that path. One
        // selected frame per interval is too sparse to raise a self-suppressed
        // delivery estimate; a bounded burst supplies measurable evidence while
        // preserving the normal send-credit and eligibility gates.
        const auto make_selection = [payload_bytes](const Candidate& candidate,
            std::uint64_t cost, bool probe, std::uint64_t probe_budget,
            std::uint64_t probe_remaining) noexcept {
            Selection result{Status::Selected, candidate.id, cost, probe};
            result.payload_bytes = payload_bytes;
            result.selected_send_credit_bytes = candidate.send_credit_bytes.value_or(0);
            result.selected_queue_bytes = candidate.queue_bytes;
            result.selected_wire_inflight_bytes = candidate.unacked_bytes;
            result.probe_budget_bytes = probe ? probe_budget : 0;
            result.probe_remaining_bytes = probe ? probe_remaining : 0;
            return result;
        };

        // VMUX marks every candidate probe_eligible=false for a non-payload
        // selection (ACK/control/retransmission). Such a selection sends no
        // probe payload, so it must leave probe ownership, recency, and the
        // remaining burst budget untouched. Previously the owner check below
        // saw probe_eligible=false and cleared an active burst, so an ordinary
        // control frame between two payload frames stranded the committed
        // budget and the probe could never complete.
        const bool payload_selection = std::any_of(candidates.begin(), candidates.end(),
            [](const Candidate& candidate) { return candidate.probe_eligible; });
        if (!payload_selection) {
            Selection result = make_selection(*best, best_cost, false, 0, 0);
            // The VMUX send-credit path reads the selected carrier's cached
            // remainder as the discovery grant. A control frame does not spend
            // the burst, so if it happens to select the probe owner it must
            // republish that remaining commitment; otherwise the next payload
            // frame would lose its discovery allowance even though the
            // scheduler still owns the burst.
            if (probe_owner_active_ && best->id == probe_owner_id_ &&
                best->generation == probe_owner_generation_) {
                result.probe_remaining_bytes = ProbeRemainingBytes(*best);
            }
            return result;
        }

        const Candidate* active_probe_candidate = nullptr;
        Tracker* active_probe_tracker = nullptr;
        bool keep_probe_owner = false;
        if (probe_owner_active_) {
            for (const Candidate& candidate : candidates) {
                if (candidate.id != probe_owner_id_ ||
                    candidate.generation != probe_owner_generation_) continue;
                Tracker* tracker = FindTracker(candidate);
                const bool valid_owner = tracker &&
                    tracker->probe_remaining_bytes >= payload_bytes &&
                    candidate.eligible && CanCarry(candidate) &&
                    CanContinueProbe(candidate);
                if (valid_owner && HasCredit(candidate, payload_bytes)) {
                    active_probe_candidate = &candidate;
                    active_probe_tracker = tracker;
                } else if (valid_owner) {
                    // Credit can be transiently exhausted while another path
                    // drains. Keep the owner so the bounded burst can resume.
                    keep_probe_owner = true;
                }
                break;
            }
            if (!active_probe_candidate && !keep_probe_owner) ClearProbeState();
        }
        if (active_probe_candidate) {
            active_probe_tracker->probe_remaining_bytes -= payload_bytes;
            // The frame is both an ordinary selection of the carrier and probe
            // payload; the latter advances the probe duty-cycle clock so a
            // finished burst cannot immediately restart on the same path.
            active_probe_tracker->last_selected_ms = now_ms;
            active_probe_tracker->last_probe_ms = now_ms;
            const auto budget = active_probe_tracker->probe_budget_bytes != 0
                ? active_probe_tracker->probe_budget_bytes
                : active_probe_candidate->probe_budget_bytes.value_or(probe_burst_bytes_);
            // The owner ends with the burst, not on the next selection that
            // notices the empty budget, so runtime/diagnostic state never
            // reports an owner with nothing left to spend.
            if (active_probe_tracker->probe_remaining_bytes == 0) {
                probe_owner_active_ = false;
                probe_owner_id_ = 0;
                probe_owner_generation_ = 0;
            }
            return make_selection(*active_probe_candidate,
                EstimateArrival(*active_probe_candidate, now_ms, payload_bytes),
                true, budget, active_probe_tracker->probe_remaining_bytes);
        }

        const Candidate* probe_candidate = nullptr;
        Tracker* probe_tracker = nullptr;
        bool probe_is_plateau = false;
        if (!keep_probe_owner && now_ms >= last_probe_ms_ &&
            now_ms - last_probe_ms_ >= probe_interval_ms_) {
            for (const Candidate& candidate : candidates) {
                const bool plateau = plateau_probe_budget_bytes_ != 0 &&
                    IsPlateauProbeCandidate(candidate, best->id);
                if (!candidate.eligible || !CanCarry(candidate) ||
                    !CanProbe(candidate, payload_bytes, plateau) ||
                    !HasCredit(candidate, payload_bytes)) continue;
                // The best carrier normally carries this frame, so probing it
                // discovers nothing new. It is the exception when its own
                // delivery still lags its known capacity (a real gap) or when
                // it sits at a self-consistent capacity plateau. A low-RTT
                // carrier is best on every ordinary selection, so skipping it
                // here would leave its depressed delivery estimate
                // unrevalidated forever. Probe recency still bounds how often
                // any single carrier consumes the global probe slot.
                if (candidate.id == best->id && !NeedsCapacityProbe(candidate) &&
                    !plateau) continue;
                Tracker* tracker = FindTracker(candidate);
                // Probe recency follows probe payload only. Using the ordinary
                // selection clock here starves any carrier whose normal send
                // cadence is shorter than the probe interval. A plateau burst
                // is additionally rate-limited to MinPlateauProbeIntervalMs so
                // an at-capacity best cannot hold a permanent extra share.
                const std::uint64_t required_interval = plateau
                    ? std::max(probe_interval_ms_, MinPlateauProbeIntervalMs)
                    : probe_interval_ms_;
                if (!tracker || now_ms < tracker->last_probe_ms ||
                    now_ms - tracker->last_probe_ms < required_interval) continue;
                if (!probe_tracker || tracker->last_probe_ms < probe_tracker->last_probe_ms ||
                    (tracker->last_probe_ms == probe_tracker->last_probe_ms &&
                     candidate.id < probe_candidate->id)) {
                    probe_candidate = &candidate;
                    probe_tracker = tracker;
                    probe_is_plateau = plateau;
                }
            }
        }

        const Candidate& selected = probe_candidate ? *probe_candidate : *best;
        Tracker* selected_tracker = FindTracker(selected);
        // Probe eligibility is false for VMUX control/ACK frames. They may
        // select a carrier, but they are not payload selections, so they do not
        // update the ordinary-selection timestamp. Probe recency is a separate
        // clock and is never advanced here.
        if (selected_tracker && selected.probe_eligible) selected_tracker->last_selected_ms = now_ms;
        if (probe_candidate) {
            ClearProbeState();
            probe_tracker = FindTracker(selected);
            probe_tracker->last_probe_ms = now_ms;
            const auto nominal_budget =
                selected.probe_budget_bytes.value_or(probe_burst_bytes_);
            const auto budget = probe_is_plateau
                ? std::min(nominal_budget, plateau_probe_budget_bytes_)
                : nominal_budget;
            probe_tracker->probe_budget_bytes = budget;
            probe_tracker->probe_remaining_bytes = budget >= payload_bytes
                ? budget - payload_bytes : 0;
            if (probe_tracker->probe_remaining_bytes != 0) {
                probe_owner_active_ = true;
                probe_owner_id_ = selected.id;
                probe_owner_generation_ = selected.generation;
            }
            last_probe_ms_ = now_ms;
            return make_selection(selected, EstimateArrival(selected, now_ms, payload_bytes),
                true, budget, probe_tracker->probe_remaining_bytes);
        }
        return make_selection(selected, best_cost, false, 0, 0);
    }

    void CaptureProbeDiagnostics(const std::vector<Candidate>& candidates,
                                 std::uint64_t now_ms,
                                 std::uint64_t payload_bytes,
                                 const Candidate& best) noexcept {
        probe_diagnostics_ = {};
        for (const Candidate& candidate : candidates) {
            ProbeDiagnosticEntry* entry = nullptr;
            for (ProbeDiagnosticEntry& item : probe_diagnostics_) {
                if (item.used) continue;
                entry = &item;
                break;
            }
            if (!entry) break;
            entry->id = candidate.id;
            entry->generation = candidate.generation;
            entry->used = true;
            auto& diagnostic = entry->value;
            diagnostic.available = true;
            diagnostic.needs_capacity_probe = NeedsCapacityProbe(candidate);
            const bool plateau = plateau_probe_budget_bytes_ != 0 &&
                IsPlateauProbeCandidate(candidate, best.id);
            const auto add_gate = [&diagnostic](ProbeGate gate) noexcept {
                diagnostic.gate_mask |= static_cast<std::uint32_t>(gate);
            };
            if (!candidate.eligible) add_gate(ProbeGate::NotEligible);
            if (!CanCarry(candidate)) add_gate(ProbeGate::InvalidState);
            if (!candidate.probe_eligible) add_gate(ProbeGate::NotProbeEligible);
            if (candidate.state != CandidateState::Active &&
                candidate.state != CandidateState::Warming) {
                add_gate(ProbeGate::InvalidState);
            }
            if (candidate.at_capacity && !diagnostic.needs_capacity_probe &&
                !plateau) {
                add_gate(ProbeGate::AtCapacity);
            }
            if (candidate.probe_budget_bytes &&
                *candidate.probe_budget_bytes < payload_bytes) {
                add_gate(ProbeGate::Budget);
            }
            // The best carrier is excluded from probing unless its own
            // delivery lags its known capacity or it qualifies for a bounded
            // plateau probe, matching the probe scan.
            if (candidate.id == best.id && !diagnostic.needs_capacity_probe &&
                !plateau) {
                add_gate(ProbeGate::Best);
            }
            if (!HasCredit(candidate, payload_bytes)) add_gate(ProbeGate::NoCredit);
            if (!probe_clock_started_ || now_ms < last_probe_ms_ ||
                now_ms - last_probe_ms_ < probe_interval_ms_) {
                add_gate(ProbeGate::ProbeInterval);
            }
            const Tracker* tracker = nullptr;
            for (const Tracker& item : trackers_) {
                if (item.used && item.id == candidate.id &&
                    item.generation == candidate.generation) {
                    tracker = &item;
                    break;
                }
            }
            if (!tracker) {
                add_gate(ProbeGate::NoTracker);
            } else {
                diagnostic.last_selected_ms = tracker->last_selected_ms;
                diagnostic.probe_remaining_bytes = tracker->probe_remaining_bytes;
                // Recency reports the probe gate, which is driven by the probe
                // clock rather than the ordinary selection clock. Plateau
                // probes additionally wait MinPlateauProbeIntervalMs.
                const std::uint64_t required_interval = plateau
                    ? std::max(probe_interval_ms_, MinPlateauProbeIntervalMs)
                    : probe_interval_ms_;
                if (now_ms < tracker->last_probe_ms ||
                    now_ms - tracker->last_probe_ms < required_interval) {
                    add_gate(ProbeGate::Recency);
                }
            }
            if (probe_owner_active_ &&
                (candidate.id != probe_owner_id_ ||
                 candidate.generation != probe_owner_generation_)) {
                add_gate(ProbeGate::ProbeOwner);
            }
            diagnostic.can_probe = diagnostic.gate_mask == 0;
        }
    }

    Tracker* FindTracker(const Candidate& candidate) noexcept {
        for (Tracker& tracker : trackers_) {
            if (tracker.used && tracker.id == candidate.id &&
                tracker.generation == candidate.generation) return &tracker;
        }
        return nullptr;
    }

    void EnsureTrackers(const std::vector<Candidate>& candidates,
                        std::uint64_t now_ms) noexcept {
        for (const Candidate& candidate : candidates) {
            if (FindTracker(candidate)) continue;
            for (Tracker& tracker : trackers_) {
                if (tracker.used) continue;
                tracker.id = candidate.id;
                tracker.last_selected_ms = now_ms;
                // A newly tracked path waits one probe interval, as before,
                // but only probe activity extends that wait after this point.
                tracker.last_probe_ms = now_ms;
                tracker.generation = candidate.generation;
                tracker.used = true;
                break;
            }
        }
    }

    void ClearProbeState() noexcept {
        for (Tracker& tracker : trackers_) {
            tracker.probe_remaining_bytes = 0;
            tracker.probe_budget_bytes = 0;
        }
        probe_owner_active_ = false;
        probe_owner_id_ = 0;
        probe_owner_generation_ = 0;
    }

    void PruneTrackers(const std::vector<Candidate>& candidates) noexcept {
        for (Tracker& tracker : trackers_) {
            if (!tracker.used) continue;
            const bool present = std::any_of(candidates.begin(), candidates.end(),
                [&tracker](const Candidate& candidate) {
                    return candidate.id == tracker.id && candidate.generation == tracker.generation;
                });
            if (!present) tracker = Tracker();
        }
        if (probe_owner_active_) {
            const bool owner_present = std::any_of(candidates.begin(), candidates.end(),
                [this](const Candidate& candidate) {
                    return candidate.id == probe_owner_id_ &&
                        candidate.generation == probe_owner_generation_ &&
                        FindTracker(candidate) != nullptr;
                });
            if (!owner_present) ClearProbeState();
        }
    }

    static constexpr std::uint64_t DefaultProbeBurstBytes = 2 * 1024 * 1024;
    static constexpr std::uint64_t MaxProbeBurstBytes = 32 * 1024 * 1024;
    std::array<Tracker, MaxCandidates> trackers_{};
    std::array<ProbeDiagnosticEntry, MaxCandidates> probe_diagnostics_{};
    bool probe_diagnostics_enabled_ = false;
    bool probe_owner_active_ = false;
    std::uint16_t probe_owner_id_ = 0;
    std::uint64_t probe_owner_generation_ = 0;
    std::uint64_t probe_interval_ms_ = 1000;
    std::uint64_t probe_burst_bytes_ = DefaultProbeBurstBytes;
    std::uint64_t plateau_probe_budget_bytes_ = DefaultPlateauProbeBudgetBytes;
    std::uint64_t last_probe_ms_ = 0;
    bool probe_clock_started_ = false;
};

}
