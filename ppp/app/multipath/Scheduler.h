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
        std::optional<std::uint64_t> send_credit_bytes;
        std::optional<std::uint64_t> rtt_ms;
        std::optional<std::uint64_t> throughput_bytes_per_second;
        std::uint64_t last_active_ms = 0;
        std::uint64_t metrics_updated_ms = 0;
        // Active preserves the legacy API behavior for callers without a
        // PathControl snapshot.
        CandidateState state = CandidateState::Active;
        bool probe_eligible = true;
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
    };

    static constexpr std::size_t MaxCandidates = 32;
    static constexpr std::uint64_t MetricsMaxAgeMs = 5000;
    static constexpr std::uint64_t DefaultUnknownRttMs = 500;
    static constexpr std::uint64_t DefaultUnknownThroughputBytesPerSecond = 64 * 1024;

    explicit PathScheduler(std::uint64_t probe_interval_ms = 1000) noexcept
        : probe_interval_ms_(std::max<std::uint64_t>(1,
            std::min<std::uint64_t>(probe_interval_ms, 60000))) {}

    static std::uint64_t AdaptiveFlightCredit(std::uint64_t session_window,
        std::size_t live_carriers, std::uint64_t bdp, std::uint64_t inflight,
        unsigned weight, std::uint64_t startup_window, std::uint64_t minimum_window) noexcept {
        const auto share = session_window / std::max<std::size_t>(1, live_carriers);
        // Headroom lets the sender discover capacity above its current estimate.
        const auto target = std::min(share, std::max(startup_window, SaturatingAdd(bdp, bdp)));
        const auto bounded_weight = std::min(100u, weight);
        const auto weighted = (target / 100) * bounded_weight +
            (target % 100) * bounded_weight / 100;
        const auto limit = std::min(share, std::max(minimum_window, weighted));
        return limit > inflight ? limit - inflight : 0;
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
            if (snapshot.capabilities.payload_delivery_rate) {
                const auto rate = CapacityIsFresh(snapshot.telemetry, now_ms)
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
        last_probe_ms_ = 0;
        probe_clock_started_ = false;
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
        std::uint64_t generation = 0;
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

    static bool CanProbe(const Candidate& candidate, std::uint64_t payload_bytes) noexcept {
        if (!candidate.probe_eligible ||
            (candidate.state != CandidateState::Active &&
             candidate.state != CandidateState::Warming)) {
            return false;
        }
        return !candidate.probe_budget_bytes || *candidate.probe_budget_bytes >= payload_bytes;
    }

    static std::uint64_t SaturatingAdd(std::uint64_t a, std::uint64_t b) noexcept {
        const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
        return b > maximum - a ? maximum : a + b;
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
        bool any_eligible = false;
        const Candidate* best = nullptr;
        std::uint64_t best_cost = std::numeric_limits<std::uint64_t>::max();
        for (const Candidate& candidate : candidates) {
            if (!candidate.eligible || !CanCarry(candidate)) continue;
            any_eligible = true;
            if (!HasCredit(candidate, payload_bytes)) continue;
            const std::uint64_t cost = EstimateArrival(candidate, now_ms, payload_bytes);
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
        EnsureTrackers(candidates, now_ms);

        const Candidate* probe_candidate = nullptr;
        Tracker* probe_tracker = nullptr;
        if (now_ms >= last_probe_ms_ && now_ms - last_probe_ms_ >= probe_interval_ms_) {
            for (const Candidate& candidate : candidates) {
                if (!candidate.eligible || !CanCarry(candidate) ||
                    !CanProbe(candidate, payload_bytes) || candidate.id == best->id ||
                    !HasCredit(candidate, payload_bytes)) continue;
                Tracker* tracker = FindTracker(candidate);
                if (!tracker || now_ms < tracker->last_selected_ms ||
                    now_ms - tracker->last_selected_ms < probe_interval_ms_) continue;
                if (!probe_tracker || tracker->last_selected_ms < probe_tracker->last_selected_ms ||
                    (tracker->last_selected_ms == probe_tracker->last_selected_ms &&
                     candidate.id < probe_candidate->id)) {
                    probe_candidate = &candidate;
                    probe_tracker = tracker;
                }
            }
        }

        const Candidate& selected = probe_candidate ? *probe_candidate : *best;
        Tracker* selected_tracker = FindTracker(selected);
        if (selected_tracker) selected_tracker->last_selected_ms = now_ms;
        if (probe_candidate) last_probe_ms_ = now_ms;
        return {Status::Selected, selected.id,
            probe_candidate ? EstimateArrival(*probe_candidate, now_ms, payload_bytes) : best_cost,
            probe_candidate != nullptr};
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
                tracker.generation = candidate.generation;
                tracker.used = true;
                break;
            }
        }
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
    }

    std::array<Tracker, MaxCandidates> trackers_{};
    std::uint64_t probe_interval_ms_ = 1000;
    std::uint64_t last_probe_ms_ = 0;
    bool probe_clock_started_ = false;
};

}
