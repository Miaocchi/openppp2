#pragma once

#include <cstdint>
#include <limits>

namespace ppp::app::multipath {

/**
 * @file PathPacingPolicy.h
 * @brief Pure per-carrier wire-pacing state math for automatic TCP pacing.
 *
 * VMUX measures a carrier's capacity in delivered payload bytes per second and
 * the kernel paces the TCP wire bytes per second. This policy turns fresh
 * capacity samples into a conservative pacing target for one carrier.
 *
 * Invariants:
 * - No evidence, no cap. Unknown or stale capacity returns Clear (or None when
 *   no cap was applied); a guessed cap is never produced.
 * - A payload rate is not a wire rate. The estimate below is delivered payload
 *   while the kernel cap is wire bytes, so pacing requires an observed
 *   payload-to-wire ratio (wire bytes per payload byte, expressed as a fraction
 *   that cannot be below one). Without it no cap is applied, and a cap left
 *   over from a previously known ratio is cleared: the conversion would be a
 *   guess, and a guessed wire cap sits below the carrier's real wire rate,
 *   which then bounds every later delivery sample at the cap.
 * - The cap is derived from the highest fresh capacity observed, so a paced
 *   delivery sample can never ratchet the baseline down. Pacing is an upper
 *   bound: a too-high cap only wastes headroom, whereas lowering the cap on
 *   paced feedback would collapse the path to its own pacing rate.
 * - The estimate must stay stable for a maturity window before it is trusted.
 *   A first slow-start sample otherwise becomes a permanent cap that hides the
 *   capacity above it (the cap would bound every later delivery sample).
 * - A target that differs from the applied rate by less than the minimum
 *   change is not re-applied, so a steady path never repeats the syscall.
 * - A capacity sample cannot prove the capacity that lies above the cap that
 *   produced it: the estimator measures delivered payload, so a cap below the
 *   path's real rate makes every later sample plateau at the cap. While
 *   discovery is unconfirmed the target therefore sits above the measured rate
 *   (DiscoverySlackPermille), and the mature cap (the measured payload-to-wire
 *   ratio) is applied only after that headroom demonstrably produced no
 *   additional delivery while the carrier had filled its measured BDP: a
 *   starved carrier plateaus at what the sender offered, which is no evidence
 *   about the cap above it.
 * - Discovery ends at most once per carrier and only while unconfirmed: the
 *   single downward step it may produce is evidence-backed (a non-binding cap
 *   reached nothing), never a reaction to paced feedback.
 */
enum class PathPacingAction : std::uint8_t {
    None,
    Apply,
    Clear,
};

enum class PathPacingReason : std::uint8_t {
    UnknownCapacity,
    StaleCapacity,
    Warming,
    StableCapacity,
    SkipFailedRate,
    /** Fresh capacity, but no payload-to-wire ratio: no cap can be derived. */
    UnknownWireRatio,
};

struct PathPacingObservation final {
    /** Fresh capacity estimate in payload bytes per second (0 = unknown). */
    std::uint64_t capacity_bytes_per_second = 0;
    /** Tick of the sample that produced the capacity estimate (0 = none). */
    std::uint64_t capacity_updated_ms = 0;
    std::uint64_t now_ms = 0;
    /** True only when the carrier has filled its measured BDP. */
    bool at_capacity = false;
    /**
     * Optional wire-per-payload ratio, expressed as a fraction >= 1 (e.g.
     * 103/100 describes 3% framing/TCP overhead). Not "payload per wire": the
     * numerator counts wire bytes per `denominator` payload bytes, so a valid
     * fraction converts payload capacity into a wire rate by multiplication.
     * Any zero, or numerator < denominator, means unavailable.
     */
    std::uint64_t payload_to_wire_numerator = 0;
    std::uint64_t payload_to_wire_denominator = 0;

};
struct PathPacingDecision final {
    PathPacingAction action = PathPacingAction::None;
    PathPacingReason reason = PathPacingReason::UnknownCapacity;
    /** Rate to apply on Action::Apply, or the currently applied rate otherwise. */
    std::uint64_t rate_bytes_per_second = 0;
};

class PathPacingPolicy final {
public:
    /** Capacity older than this is no evidence; the cap is cleared. */
    static constexpr std::uint64_t DefaultFreshnessMs = 5000;
    /** A fresh capacity must stay this long without a >=5% rise before use. */
    static constexpr std::uint64_t DefaultMaturityMs = 1000;
    /**
     * Discovery headroom: while the capacity estimate is not yet confirmed by a
     * measurement taken above the cap, pace at 125% of measured payload
     * capacity, or at the wire-ratio rate when that is larger. Any cap at or
     * below the measured rate bounds the very next sample, so only an
     * overshooting probe can reveal the capacity behind a paced plateau.
     */
    static constexpr std::uint64_t DiscoverySlackPermille = 1250;
    /**
     * Binding-probe headroom: when the measured capacity already reaches
     * DiscoveryBindingPermille of the probe cap in force, that cap shaped the
     * sample, so the sample says nothing about the capacity behind it. Growing
     * the probe by DiscoverySlackPermille per maturity window then needs many
     * seconds to climb out of an early low estimate (a loss in slow start
     * leaves CUBIC far below the path, and a cap that binds also stops the
     * kernel from growing cwnd). A binding probe therefore doubles instead.
     */
    static constexpr std::uint64_t DiscoveryBindingSlackPermille = 2000;
    static constexpr std::uint64_t DiscoveryBindingPermille = 900;
    /** Re-apply only when the target moves at least this much. */
    static constexpr std::uint64_t MinRateChangePermille = 50;
    /**
     * Mature-cap headroom: once discovery has confirmed the capacity, pace the
     * carrier slightly below it. A cap at the full measured rate keeps the
     * bottleneck queue (and the sender's own backlog) standing, which inflates
     * every RTT sample, starves the reverse ACK stream, and drives BBR into
     * periodic ProbeRTT collapses (cwnd 4). The high-water candidate never
     * falls on paced feedback, so this margin cannot ratchet the cap down.
     */
    static constexpr std::uint64_t MatureHeadroomPermille = 975;

    /**
     * @brief Evaluates one carrier observation.
     * @return The action the caller must commit (see CommitApplied/CommitFailed/
     *         CommitCleared) after talking to the transport.
     */
    PathPacingDecision Observe(const PathPacingObservation& observation) noexcept {
        if (!observation.at_capacity && applied_bytes_per_second_ == 0) {
            return Make(PathPacingAction::None, PathPacingReason::Warming, 0);
        }
        const std::uint64_t now = observation.now_ms;
        const bool fresh = observation.capacity_bytes_per_second != 0 &&
            observation.capacity_updated_ms != 0 &&
            observation.capacity_updated_ms <= now &&
            now - observation.capacity_updated_ms <= DefaultFreshnessMs;
        if (!fresh) {
            // Unknown/stale capacity is not evidence: never keep a cap (and
            // never invent one). The observed high-water is retained only as
            // history; it is not re-applied from this observation. The probe
            // and its headroom go with the cap: a re-applied cap must re-earn
            // them from fresh delivery.
            failed_rate_bytes_per_second_ = 0;
            pending_discovery_capacity_bytes_per_second_ = 0;
            pending_discovery_ms_ = 0;
            discovery_granted_capacity_bytes_per_second_ = 0;
            discovery_granted_ms_ = 0;
            if (applied_bytes_per_second_ != 0) {
                applied_bytes_per_second_ = 0;
                return Make(PathPacingAction::Clear, PathPacingReason::StaleCapacity, 0);
            }
            return Make(PathPacingAction::None, PathPacingReason::StaleCapacity, 0);
        }

        if (!HasWireRatio(observation.payload_to_wire_numerator,
            observation.payload_to_wire_denominator)) {
            // A payload rate is not a wire rate. Without an observed conversion
            // any target would be a guess, and a guessed wire cap sits below the
            // carrier's real wire rate: the kernel would then shape every later
            // delivery sample to the cap. So no ratio, no cap (and no probe).
            // A cap left over from a previously known ratio is retracted rather
            // than kept on stale conversion evidence, and a re-applied cap must
            // re-earn both maturity and discovery headroom from fresh delivery.
            failed_rate_bytes_per_second_ = 0;
            pending_discovery_capacity_bytes_per_second_ = 0;
            pending_discovery_ms_ = 0;
            discovery_granted_capacity_bytes_per_second_ = 0;
            discovery_granted_ms_ = 0;
            if (applied_bytes_per_second_ != 0) {
                applied_bytes_per_second_ = 0;
                return Make(PathPacingAction::Clear,
                    PathPacingReason::UnknownWireRatio, 0);
            }
            return Make(PathPacingAction::None,
                PathPacingReason::UnknownWireRatio, 0);
        }

        const std::uint64_t capacity = observation.capacity_bytes_per_second;
        if (candidate_bytes_per_second_ == 0) {
            candidate_bytes_per_second_ = capacity;
            candidate_since_ms_ = now;
        }
        else if (ExceedsByPermille(capacity, candidate_bytes_per_second_,
            MinRateChangePermille)) {
            // A real new high-water restarts the stability window.
            candidate_bytes_per_second_ = capacity;
            candidate_since_ms_ = now;
        }
        else if (capacity > candidate_bytes_per_second_) {
            // Sub-threshold growth is adopted without masking graduation.
            candidate_bytes_per_second_ = capacity;
        }

        if (candidate_since_ms_ == 0 || now < candidate_since_ms_) {
            candidate_since_ms_ = now;
        }
        if (now - candidate_since_ms_ < DefaultMaturityMs) {
            return Make(PathPacingAction::None, PathPacingReason::Warming,
                applied_bytes_per_second_);
        }

        // Discovery ends only once the headroom granted for this capacity had
        // a full maturity window to show more delivery and did not. The
        // estimate must also have been published after the probe landed, so a
        // sample predating it cannot end discovery, and the carrier must have
        // filled its measured BDP: a starved sender's plateau reports the
        // offered load, not the capacity above the cap. Then the cap did not
        // shape that measurement, so the mature cap is trustworthy.
        bool completed_now = false;
        if (discovery_ && discovery_granted_capacity_bytes_per_second_ != 0 &&
            observation.at_capacity &&
            observation.capacity_updated_ms > discovery_granted_ms_ &&
            now >= discovery_granted_ms_ &&
            now - discovery_granted_ms_ >= DefaultMaturityMs &&
            !ExceedsByPermille(candidate_bytes_per_second_,
                discovery_granted_capacity_bytes_per_second_,
                MinRateChangePermille)) {
            discovery_ = false;
            discovery_granted_capacity_bytes_per_second_ = 0;
            discovery_granted_ms_ = 0;
            completed_now = true;
        }

        const bool binding_probe = discovery_ && applied_bytes_per_second_ != 0 &&
            SaturatingMultiply(DeriveRate(candidate_bytes_per_second_,
                observation.payload_to_wire_numerator,
                observation.payload_to_wire_denominator), 1000) >=
            SaturatingMultiply(applied_bytes_per_second_, DiscoveryBindingPermille);
        std::uint64_t target = discovery_
            ? DeriveDiscoveryRate(candidate_bytes_per_second_,
                observation.payload_to_wire_numerator,
                observation.payload_to_wire_denominator,
                binding_probe ? DiscoveryBindingSlackPermille : DiscoverySlackPermille)
            : DeriveMatureRate(candidate_bytes_per_second_,
                observation.payload_to_wire_numerator,
                observation.payload_to_wire_denominator);
        if (target == 0) {
            return Make(PathPacingAction::None, PathPacingReason::StableCapacity,
                applied_bytes_per_second_);
        }
        if (failed_rate_bytes_per_second_ != 0 &&
            !ExceedsByPermille(target, failed_rate_bytes_per_second_,
                MinRateChangePermille)) {
            // The last attempt at this target failed; do not repeat the
            // syscall until the evidence moves.
            return Make(PathPacingAction::None, PathPacingReason::SkipFailedRate,
                applied_bytes_per_second_);
        }
        if (applied_bytes_per_second_ != 0 &&
            !ExceedsByPermille(target, applied_bytes_per_second_,
                MinRateChangePermille)) {
            // A target at or below the applied cap is paced feedback, not
            // evidence: it never ratchets the cap down. The one exception is
            // the completion step, which is backed by a non-binding probe.
            if (!completed_now || target >= applied_bytes_per_second_) {
                return Make(PathPacingAction::None, PathPacingReason::StableCapacity,
                    applied_bytes_per_second_);
            }
        }
        if (discovery_) {
            pending_discovery_capacity_bytes_per_second_ = candidate_bytes_per_second_;
            pending_discovery_ms_ = now;
        }
        return Make(PathPacingAction::Apply, PathPacingReason::StableCapacity, target);
    }

    /** Records a rate the transport accepted. */
    void CommitApplied(std::uint64_t rate_bytes_per_second) noexcept {
        applied_bytes_per_second_ = rate_bytes_per_second;
        failed_rate_bytes_per_second_ = 0;
        if (pending_discovery_capacity_bytes_per_second_ != 0) {
            // The probe is in force now: its headroom is judged from here.
            discovery_granted_capacity_bytes_per_second_ =
                pending_discovery_capacity_bytes_per_second_;
            discovery_granted_ms_ = pending_discovery_ms_;
            pending_discovery_capacity_bytes_per_second_ = 0;
            pending_discovery_ms_ = 0;
        }
    }

    /** Records a rate the transport rejected; the previous cap remains. */
    void CommitFailed(std::uint64_t rate_bytes_per_second) noexcept {
        failed_rate_bytes_per_second_ = rate_bytes_per_second;
        pending_discovery_capacity_bytes_per_second_ = 0;
        pending_discovery_ms_ = 0;
    }

    /** Records that the transport cleared the cap. */
    void CommitCleared() noexcept {
        applied_bytes_per_second_ = 0;
        failed_rate_bytes_per_second_ = 0;
        pending_discovery_capacity_bytes_per_second_ = 0;
        pending_discovery_ms_ = 0;
        // A cleared cap is no longer probing anything: a re-applied cap must
        // re-earn its headroom from fresh delivery.
        discovery_granted_capacity_bytes_per_second_ = 0;
        discovery_granted_ms_ = 0;
    }

    /** Forgets every observation (new carrier attachment). */
    void Reset() noexcept {
        candidate_bytes_per_second_ = 0;
        candidate_since_ms_ = 0;
        applied_bytes_per_second_ = 0;
        failed_rate_bytes_per_second_ = 0;
        discovery_ = true;
        pending_discovery_capacity_bytes_per_second_ = 0;
        pending_discovery_ms_ = 0;
        discovery_granted_capacity_bytes_per_second_ = 0;
        discovery_granted_ms_ = 0;
    }

    /** @brief Wire pacing rate currently applied to the carrier (0 = uncapped). */
    std::uint64_t AppliedRate() const noexcept { return applied_bytes_per_second_; }
    /** @brief Highest fresh capacity observed; history, not current evidence. */
    std::uint64_t CandidateCapacity() const noexcept { return candidate_bytes_per_second_; }

    /**
     * @brief True when the observation carries a usable wire conversion.
     * @details A fraction below one cannot describe wire bytes (payload would
     *          exceed the wire that carried it), so it is treated as unknown.
     */
    static bool HasWireRatio(std::uint64_t payload_to_wire_numerator,
        std::uint64_t payload_to_wire_denominator) noexcept {
        return payload_to_wire_numerator != 0 &&
            payload_to_wire_denominator != 0 &&
            payload_to_wire_numerator >= payload_to_wire_denominator;
    }

    /**
     * @brief Derives the mature wire pacing target from a capacity sample.
     * @param capacity Payload bytes per second.
     * @param payload_to_wire_numerator Wire bytes per payload byte (>= 1).
     * @param payload_to_wire_denominator Payload bytes in the same fraction.
     * @return Saturation-safe wire target, or 0 (no cap) when the ratio is
     *         unavailable: a payload-derived wire rate is never guessed.
     */
    static std::uint64_t DeriveRate(std::uint64_t capacity,
        std::uint64_t payload_to_wire_numerator,
        std::uint64_t payload_to_wire_denominator) noexcept {
        if (capacity == 0 || !HasWireRatio(payload_to_wire_numerator,
            payload_to_wire_denominator)) {
            return 0;
        }
        if (capacity >
            std::numeric_limits<std::uint64_t>::max() / payload_to_wire_numerator) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return (capacity * payload_to_wire_numerator) /
            payload_to_wire_denominator;
    }

    /**
     * @brief Derives the mature target: the wire-ratio rate scaled by
     *        MatureHeadroomPermille (never below 1 byte/s once non-zero).
     */
    static std::uint64_t DeriveMatureRate(std::uint64_t capacity,
        std::uint64_t payload_to_wire_numerator,
        std::uint64_t payload_to_wire_denominator) noexcept {
        const std::uint64_t ratio = DeriveRate(capacity,
            payload_to_wire_numerator, payload_to_wire_denominator);
        if (ratio == 0) {
            return 0;
        }
        const std::uint64_t scaled =
            ratio > std::numeric_limits<std::uint64_t>::max() / MatureHeadroomPermille
            ? (ratio / 1000) * MatureHeadroomPermille
            : (ratio * MatureHeadroomPermille) / 1000;
        return scaled != 0 ? scaled : 1;
    }

    /**
     * @brief Derives the discovery target: the larger of the wire-ratio rate
     *        and DiscoverySlackPermille of the measured payload capacity.
     * @return Saturation-safe wire target, or 0 (no probe) when the ratio is
     *         unavailable: even the probe is a wire cap, so it needs the same
     *         observed conversion as the mature target.
     */
    static std::uint64_t DeriveDiscoveryRate(std::uint64_t capacity,
        std::uint64_t payload_to_wire_numerator,
        std::uint64_t payload_to_wire_denominator,
        std::uint64_t slack_permille = DiscoverySlackPermille) noexcept {
        if (capacity == 0 || !HasWireRatio(payload_to_wire_numerator,
            payload_to_wire_denominator)) {
            return 0;
        }
        const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
        const std::uint64_t headroom =
            capacity > maximum / slack_permille
            ? maximum
            : (capacity * slack_permille) / 1000;
        const std::uint64_t ratio = DeriveRate(capacity,
            payload_to_wire_numerator, payload_to_wire_denominator);
        return ratio > headroom ? ratio : headroom;
    }

private:
    static PathPacingDecision Make(PathPacingAction action, PathPacingReason reason,
        std::uint64_t rate) noexcept {
        PathPacingDecision decision;
        decision.action = action;
        decision.reason = reason;
        decision.rate_bytes_per_second = rate;
        return decision;
    }

    static bool ExceedsByPermille(std::uint64_t value, std::uint64_t baseline,
        std::uint64_t permille) noexcept {
        if (value <= baseline) {
            return false;
        }
        return SaturatingMultiply(value - baseline, 1000) >
            SaturatingMultiply(baseline, permille);
    }

    static std::uint64_t SaturatingMultiply(std::uint64_t value,
        std::uint64_t factor) noexcept {
        const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
        return value > maximum / factor ? maximum : value * factor;
    }

    std::uint64_t candidate_bytes_per_second_ = 0;
    std::uint64_t candidate_since_ms_ = 0;
    std::uint64_t applied_bytes_per_second_ = 0;
    std::uint64_t failed_rate_bytes_per_second_ = 0;
    /** True until headroom is proven unnecessary (or delivery confirms it). */
    bool discovery_ = true;
    /** Capacity of the probe awaiting CommitApplied (0 = nothing pending). */
    std::uint64_t pending_discovery_capacity_bytes_per_second_ = 0;
    std::uint64_t pending_discovery_ms_ = 0;
    /** Capacity that justified the probe now in force (0 = no probe). */
    std::uint64_t discovery_granted_capacity_bytes_per_second_ = 0;
    std::uint64_t discovery_granted_ms_ = 0;
};

} // namespace ppp::app::multipath
