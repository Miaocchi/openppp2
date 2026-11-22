#define BOOST_TEST_MODULE path_pacing_policy_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/multipath/PathPacingPolicy.h>

#include <cstdint>
#include <limits>

namespace mp = ppp::app::multipath;
using Policy = mp::PathPacingPolicy;
using Action = mp::PathPacingAction;
using Reason = mp::PathPacingReason;

namespace {
constexpr std::uint64_t Megabyte = 1000 * 1000;
/** Measured wire-per-payload ratio: 3% framing/TCP overhead. */
constexpr std::uint64_t WireNumerator = 103;
constexpr std::uint64_t WireDenominator = 100;

/** Fresh capacity without a payload-to-wire ratio (production observation). */
mp::PathPacingObservation Fresh(std::uint64_t capacity, std::uint64_t now_ms) {
    mp::PathPacingObservation observation;
    observation.capacity_bytes_per_second = capacity;
    observation.capacity_updated_ms = now_ms;
    observation.now_ms = now_ms;
    observation.at_capacity = true;
    return observation;
}

/** Fresh capacity with a measured wire conversion. */
mp::PathPacingObservation FreshWire(std::uint64_t capacity, std::uint64_t now_ms,
    std::uint64_t numerator = WireNumerator,
    std::uint64_t denominator = WireDenominator) {
    auto observation = Fresh(capacity, now_ms);
    observation.payload_to_wire_numerator = numerator;
    observation.payload_to_wire_denominator = denominator;
    return observation;
}

bool IsAction(const mp::PathPacingDecision& decision, Action expected) {
    return static_cast<int>(decision.action) == static_cast<int>(expected);
}

bool IsReason(const mp::PathPacingDecision& decision, Reason expected) {
    return static_cast<int>(decision.reason) == static_cast<int>(expected);
}
}

BOOST_AUTO_TEST_CASE(unknown_or_stale_capacity_never_invents_a_cap) {
    Policy policy;
    BOOST_TEST(IsAction(policy.Observe({0, 0, 1000}), Action::None));
    // No capacity timestamp: an estimate with unknown provenance is not fresh.
    BOOST_TEST(IsAction(policy.Observe({Megabyte, 0, 2000}), Action::None));
    // A sample from the future (clock authority mismatch) is not fresh.
    BOOST_TEST(IsAction(policy.Observe({Megabyte, 5000, 2000}), Action::None));
    // A sample older than the freshness window is not fresh.
    BOOST_TEST(IsAction(policy.Observe({Megabyte, 1000, 1000 + Policy::DefaultFreshnessMs + 1}),
        Action::None));
    BOOST_TEST(policy.AppliedRate() == 0u);
    BOOST_TEST(policy.CandidateCapacity() == 0u);
}

BOOST_AUTO_TEST_CASE(estimate_must_mature_before_it_becomes_a_cap) {
    Policy policy;
    const auto first = policy.Observe(FreshWire(Megabyte, 1));
    BOOST_TEST(IsAction(first, Action::None));
    BOOST_TEST(IsReason(first, Reason::Warming));
    BOOST_TEST(IsAction(policy.Observe(FreshWire(Megabyte, Policy::DefaultMaturityMs)),
        Action::None));
    const auto mature = policy.Observe(FreshWire(Megabyte, Policy::DefaultMaturityMs + 1));
    BOOST_TEST(IsAction(mature, Action::Apply));
    // Discovery probe: above the measured rate, because a cap at or below it
    // would bound every later capacity sample.
    BOOST_TEST(mature.rate_bytes_per_second ==
        Megabyte * Policy::DiscoverySlackPermille / 1000);
}

BOOST_AUTO_TEST_CASE(paced_delivery_samples_never_ratchet_the_cap_down) {
    Policy policy;
    (void)policy.Observe(FreshWire(Megabyte, 1));
    const auto decision = policy.Observe(FreshWire(Megabyte, Policy::DefaultMaturityMs + 1));
    BOOST_TEST(IsAction(decision, Action::Apply));
    policy.CommitApplied(decision.rate_bytes_per_second);

    // A capped path measures at or below its own cap: that is not evidence of
    // lower capacity, so the applied rate and the high-water stay put.
    const auto paced = policy.Observe(FreshWire(decision.rate_bytes_per_second / 2,
        Policy::DefaultMaturityMs + 2));
    BOOST_TEST(IsAction(paced, Action::None));
    BOOST_TEST(IsReason(paced, Reason::StableCapacity));
    BOOST_TEST(paced.rate_bytes_per_second == decision.rate_bytes_per_second);

    const auto much_lower = policy.Observe(FreshWire(Megabyte / 2,
        Policy::DefaultMaturityMs + 3));
    BOOST_TEST(IsAction(much_lower, Action::None));
    BOOST_TEST(policy.AppliedRate() == decision.rate_bytes_per_second);
    BOOST_TEST(policy.CandidateCapacity() == Megabyte);
}

BOOST_AUTO_TEST_CASE(higher_fresh_capacity_raises_the_cap_after_maturity) {
    Policy policy;
    (void)policy.Observe(FreshWire(Megabyte, 1));
    policy.CommitApplied(policy.Observe(FreshWire(Megabyte, Policy::DefaultMaturityMs + 1))
        .rate_bytes_per_second);

    const std::uint64_t doubled = 2 * Megabyte;
    const auto rising = policy.Observe(FreshWire(doubled, 2000));
    BOOST_TEST(IsAction(rising, Action::None));
    BOOST_TEST(IsReason(rising, Reason::Warming));
    BOOST_TEST(policy.CandidateCapacity() == doubled);

    const auto raised = policy.Observe(FreshWire(doubled, 2000 + Policy::DefaultMaturityMs));
    BOOST_TEST(IsAction(raised, Action::Apply));
    // Still discovering, so the probe stays above the raised measurement.
    BOOST_TEST(raised.rate_bytes_per_second ==
        doubled * Policy::DiscoverySlackPermille / 1000);
    BOOST_TEST(raised.rate_bytes_per_second > doubled);
}

BOOST_AUTO_TEST_CASE(discovery_headroom_escapes_the_paced_capacity_plateau) {
    Policy policy;
    // The ramp sample is all the estimator reports before the first cap lands.
    // A cap at the measured payload (or any payload-derived fraction of it)
    // would bound every later delivery sample, so the policy probes above the
    // measurement instead of under it.
    constexpr std::uint64_t ramp = 10 * Megabyte;
    (void)policy.Observe(FreshWire(ramp, 1));
    const auto probe = policy.Observe(FreshWire(ramp, Policy::DefaultMaturityMs + 1));
    BOOST_TEST(IsAction(probe, Action::Apply));
    BOOST_TEST(probe.rate_bytes_per_second == 12500000u);
    BOOST_TEST(probe.rate_bytes_per_second > ramp);
    policy.CommitApplied(probe.rate_bytes_per_second);

    // Delivery now saturates exactly at the cap, so the estimator reports the
    // cap itself: the self-referential plateau that used to be read as steady
    // state. It must not be mistaken for evidence that the cap is right.
    const std::uint64_t plateau_capacity = probe.rate_bytes_per_second;
    const auto plateau = policy.Observe(FreshWire(plateau_capacity,
        Policy::DefaultMaturityMs + 2));
    BOOST_TEST(IsAction(plateau, Action::None));
    BOOST_TEST(IsReason(plateau, Reason::Warming));

    // After maturity the plateau is escaped: a rate strictly above the paced
    // delivery, so the next sample can finally see the capacity above the cap.
    const auto escaped = policy.Observe(FreshWire(plateau_capacity,
        2 * Policy::DefaultMaturityMs + 2));
    BOOST_TEST(IsAction(escaped, Action::Apply));
    BOOST_TEST(escaped.rate_bytes_per_second == 15625000u);
    BOOST_TEST(escaped.rate_bytes_per_second >
        plateau.rate_bytes_per_second);
    policy.CommitApplied(escaped.rate_bytes_per_second);

    // This link really tops out at the first probe's rate, so the extra
    // headroom stays empty and capacity does not follow it. One full maturity
    // window of that is proof the cap was not shaping the measurement, so the
    // mature cap is restored from the measured wire ratio.
    const auto settled = policy.Observe(FreshWire(plateau_capacity,
        3 * Policy::DefaultMaturityMs + 2));
    BOOST_TEST(IsAction(settled, Action::Apply));
    BOOST_TEST(settled.rate_bytes_per_second ==
        plateau_capacity * WireNumerator / WireDenominator);
    BOOST_TEST(policy.CandidateCapacity() == plateau_capacity);
    policy.CommitApplied(settled.rate_bytes_per_second);

    // With the mature cap in force the path measures at the cap and the policy
    // stops moving: no downward ratchet and no repeated syscall.
    const auto steady = policy.Observe(FreshWire(settled.rate_bytes_per_second,
        3 * Policy::DefaultMaturityMs + 3));
    BOOST_TEST(IsAction(steady, Action::None));
    BOOST_TEST(IsReason(steady, Reason::StableCapacity));
    BOOST_TEST(steady.rate_bytes_per_second == settled.rate_bytes_per_second);
    BOOST_TEST(policy.AppliedRate() == settled.rate_bytes_per_second);
}

BOOST_AUTO_TEST_CASE(starved_carrier_sample_cannot_end_discovery) {
    Policy policy;
    constexpr std::uint64_t ramp = 10 * Megabyte;
    (void)policy.Observe(FreshWire(ramp, 1));
    const auto probe = policy.Observe(FreshWire(ramp, Policy::DefaultMaturityMs + 1));
    BOOST_TEST(IsAction(probe, Action::Apply));
    BOOST_TEST(probe.rate_bytes_per_second == 12500000u);
    policy.CommitApplied(probe.rate_bytes_per_second);

    // The granted headroom has now had a full maturity window without extra
    // delivery, but this sample was taken while the carrier was starved: the
    // sender had nothing queued, so the plateau is the offered load, not the
    // capacity above the cap. It must not end discovery and must not drop the
    // cap to the mature rate (that would be the unilateral slack step).
    auto starved = FreshWire(ramp, 2 * Policy::DefaultMaturityMs + 2);
    starved.at_capacity = false;
    const auto withheld = policy.Observe(starved);
    BOOST_TEST(IsAction(withheld, Action::None));
    BOOST_TEST(IsReason(withheld, Reason::StableCapacity));
    BOOST_TEST(withheld.rate_bytes_per_second == probe.rate_bytes_per_second);

    // The same evidence from a carrier that filled its measured BDP does prove
    // the headroom was unused, so discovery completes and the mature cap lands.
    const auto filled = policy.Observe(FreshWire(ramp, 2 * Policy::DefaultMaturityMs + 3));
    BOOST_TEST(IsAction(filled, Action::Apply));
    BOOST_TEST(filled.rate_bytes_per_second ==
        ramp * WireNumerator / WireDenominator);
    BOOST_TEST(filled.rate_bytes_per_second < probe.rate_bytes_per_second);
    BOOST_TEST(policy.CandidateCapacity() == ramp);
}

BOOST_AUTO_TEST_CASE(small_target_changes_do_not_repeat_the_syscall) {
    Policy policy;
    (void)policy.Observe(FreshWire(Megabyte, 1));
    (void)policy.Observe(FreshWire(Megabyte, Policy::DefaultMaturityMs + 1));

    // 1250000 target vs 1240000 applied is a 0.8% move: below the 5% threshold.
    policy.CommitApplied(1240000);
    const auto quiet = policy.Observe(FreshWire(Megabyte, Policy::DefaultMaturityMs + 2));
    BOOST_TEST(IsAction(quiet, Action::None));
    BOOST_TEST(quiet.rate_bytes_per_second == 1240000u);

    // 1250000 vs 1100000 is a 13.6% move: worth one syscall.
    policy.CommitApplied(1100000);
    const auto moving = policy.Observe(FreshWire(Megabyte, Policy::DefaultMaturityMs + 3));
    BOOST_TEST(IsAction(moving, Action::Apply));
    BOOST_TEST(moving.rate_bytes_per_second == 1250000u);
}

BOOST_AUTO_TEST_CASE(failed_rate_is_not_retried_until_evidence_moves) {
    Policy policy;
    (void)policy.Observe(FreshWire(Megabyte, 1));
    const auto decision = policy.Observe(FreshWire(Megabyte, Policy::DefaultMaturityMs + 1));
    BOOST_TEST(IsAction(decision, Action::Apply));
    policy.CommitFailed(decision.rate_bytes_per_second);
    BOOST_TEST(policy.AppliedRate() == 0u);

    const auto retry = policy.Observe(FreshWire(Megabyte, Policy::DefaultMaturityMs + 2));
    BOOST_TEST(IsAction(retry, Action::None));
    BOOST_TEST(IsReason(retry, Reason::SkipFailedRate));

    // 6% more capacity is new evidence, so one fresh attempt is allowed.
    const std::uint64_t higher = Megabyte * 106 / 100;
    (void)policy.Observe(FreshWire(higher, 3000));
    const auto recovered = policy.Observe(FreshWire(higher,
        3000 + Policy::DefaultMaturityMs));
    BOOST_TEST(IsAction(recovered, Action::Apply));
    BOOST_TEST(recovered.rate_bytes_per_second > decision.rate_bytes_per_second);
}

BOOST_AUTO_TEST_CASE(stale_capacity_clears_the_cap_but_keeps_known_history) {
    Policy policy;
    (void)policy.Observe(FreshWire(Megabyte, 1));
    policy.CommitApplied(policy.Observe(FreshWire(Megabyte, Policy::DefaultMaturityMs + 1))
        .rate_bytes_per_second);
    BOOST_TEST(policy.AppliedRate() != 0u);

    const auto stale = policy.Observe({0, 0, Policy::DefaultMaturityMs + 2});
    BOOST_TEST(IsAction(stale, Action::Clear));
    BOOST_TEST(IsReason(stale, Reason::StaleCapacity));
    BOOST_TEST(policy.AppliedRate() == 0u);
    policy.CommitCleared();
    // History is retained only as hysteresis; it is not re-applied while stale.
    BOOST_TEST(policy.CandidateCapacity() == Megabyte);
    BOOST_TEST(IsAction(policy.Observe({0, 0, Policy::DefaultMaturityMs + 3}),
        Action::None));

    // The next genuinely fresh sample re-applies the known target once.
    const auto resumed = policy.Observe(FreshWire(Megabyte,
        Policy::DefaultMaturityMs + 4000));
    BOOST_TEST(IsAction(resumed, Action::Apply));
    // The cleared cap lost its probe: headroom is re-earned from fresh delivery.
    BOOST_TEST(resumed.rate_bytes_per_second ==
        Megabyte * Policy::DiscoverySlackPermille / 1000);
}

BOOST_AUTO_TEST_CASE(unknown_wire_ratio_never_applies_a_payload_cap) {
    Policy policy;
    // This is the production observation: fresh payload capacity, no wire
    // conversion. 96% (or any fraction) of payload would be a guessed wire
    // rate that the kernel then enforces below the carrier's real wire rate,
    // and every later delivery sample would plateau at the cap.
    const auto first = policy.Observe(Fresh(Megabyte, 1));
    BOOST_TEST(IsAction(first, Action::None));
    BOOST_TEST(IsReason(first, Reason::UnknownWireRatio));
    BOOST_TEST(first.rate_bytes_per_second == 0u);

    const auto mature = policy.Observe(Fresh(Megabyte, Policy::DefaultMaturityMs + 1));
    BOOST_TEST(IsAction(mature, Action::None));
    BOOST_TEST(IsReason(mature, Reason::UnknownWireRatio));
    BOOST_TEST(policy.AppliedRate() == 0u);

    // A cap applied while the ratio was known is retracted when the conversion
    // disappears: keeping it would be pacing on stale, unverifiable evidence.
    policy.CommitApplied(Megabyte);
    BOOST_TEST(policy.AppliedRate() == Megabyte);
    const auto retract = policy.Observe(Fresh(Megabyte, 2 * Policy::DefaultMaturityMs));
    BOOST_TEST(IsAction(retract, Action::Clear));
    BOOST_TEST(IsReason(retract, Reason::UnknownWireRatio));
    BOOST_TEST(retract.rate_bytes_per_second == 0u);
    BOOST_TEST(policy.AppliedRate() == 0u);
    policy.CommitCleared();

    // Uncapped stays uncapped: no repeated clear, no probe, no syscall.
    const auto quiet = policy.Observe(Fresh(Megabyte, 2 * Policy::DefaultMaturityMs + 1));
    BOOST_TEST(IsAction(quiet, Action::None));
    BOOST_TEST(IsReason(quiet, Reason::UnknownWireRatio));
    BOOST_TEST(policy.AppliedRate() == 0u);
}

BOOST_AUTO_TEST_CASE(mature_cap_converts_payload_to_wire_when_ratio_known) {
    Policy policy;
    (void)policy.Observe(FreshWire(Megabyte, 1));
    const auto probe = policy.Observe(FreshWire(Megabyte, Policy::DefaultMaturityMs + 1));
    BOOST_TEST(IsAction(probe, Action::Apply));
    policy.CommitApplied(probe.rate_bytes_per_second);

    // The path tops out where the probe saw it: a full maturity window of
    // non-binding headroom ends discovery.
    const auto settled = policy.Observe(FreshWire(Megabyte,
        2 * Policy::DefaultMaturityMs + 1));
    BOOST_TEST(IsAction(settled, Action::Apply));
    // The measured payload rate converts through the ratio, so the wire cap
    // sits above the payload sample that produced it and cannot shape it.
    BOOST_TEST(settled.rate_bytes_per_second ==
        Megabyte * WireNumerator / WireDenominator);
    BOOST_TEST(settled.rate_bytes_per_second > Megabyte);
    policy.CommitApplied(settled.rate_bytes_per_second);

    // A payload-derived fraction of the sample would have been 960000, below
    // the measured payload rate; the applied cap must stay above it.
    BOOST_TEST(policy.AppliedRate() > Megabyte);
    const auto steady = policy.Observe(FreshWire(Megabyte,
        2 * Policy::DefaultMaturityMs + 2));
    BOOST_TEST(IsAction(steady, Action::None));
    BOOST_TEST(policy.AppliedRate() == settled.rate_bytes_per_second);
}

BOOST_AUTO_TEST_CASE(wire_ratio_is_required_for_any_pacing_target) {
    // No usable conversion: no target at all, mature or discovery.
    BOOST_TEST(Policy::DeriveRate(Megabyte, 0, 0) == 0u);
    BOOST_TEST(Policy::DeriveRate(Megabyte, 0, 100) == 0u);
    BOOST_TEST(Policy::DeriveRate(Megabyte, 103, 0) == 0u);
    // A ratio below one cannot describe wire bytes and is rejected.
    BOOST_TEST(Policy::DeriveRate(Megabyte, 100, 103) == 0u);
    BOOST_TEST(Policy::DeriveRate(Megabyte, 99, 100) == 0u);
    // The removed fallback must not resurface as a guessed payload fraction.
    BOOST_TEST(Policy::DeriveRate(Megabyte, 0, 0) != Megabyte * 96 / 100);
    BOOST_TEST(Policy::DeriveRate(Megabyte, 103, 100) == 1030000u);
    BOOST_TEST(Policy::DeriveRate(Megabyte, 100, 100) == Megabyte);
    BOOST_TEST(Policy::DeriveRate(0, 103, 100) == 0u);

    // Discovery keeps the larger of the wire-ratio rate and the headroom, but
    // only once the ratio makes the probe a wire rate rather than a guess.
    BOOST_TEST(Policy::DeriveDiscoveryRate(Megabyte, 0, 0) == 0u);
    BOOST_TEST(Policy::DeriveDiscoveryRate(Megabyte, 0, 100) == 0u);
    BOOST_TEST(Policy::DeriveDiscoveryRate(Megabyte, 100, 103) == 0u);
    BOOST_TEST(Policy::DeriveDiscoveryRate(Megabyte, 103, 100) == 1250000u);
    BOOST_TEST(Policy::DeriveDiscoveryRate(Megabyte, 150, 100) == 1500000u);
    BOOST_TEST(Policy::DeriveDiscoveryRate(0, 0, 0) == 0u);

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    BOOST_TEST(Policy::DeriveRate(maximum, 103, 100) == maximum);
    // Saturation, not truncation, when capacity * ratio overflows.
    BOOST_TEST(Policy::DeriveRate(maximum, 0, 0) == 0u);
    BOOST_TEST(Policy::DeriveDiscoveryRate(maximum, 0, 0) == 0u);
    BOOST_TEST(Policy::DeriveDiscoveryRate(maximum, 103, 100) == maximum);
}

BOOST_AUTO_TEST_CASE(reset_forgets_every_observation) {
    Policy policy;
    (void)policy.Observe(FreshWire(Megabyte, 1));
    policy.CommitApplied(policy.Observe(FreshWire(Megabyte, Policy::DefaultMaturityMs + 1))
        .rate_bytes_per_second);
    policy.Reset();
    BOOST_TEST(policy.AppliedRate() == 0u);
    BOOST_TEST(policy.CandidateCapacity() == 0u);
    const auto restart = policy.Observe(FreshWire(Megabyte, 5000));
    BOOST_TEST(IsAction(restart, Action::None));
    BOOST_TEST(IsReason(restart, Reason::Warming));
    // A new attachment has proven nothing yet: it probes again.
    const auto rearmed = policy.Observe(FreshWire(Megabyte,
        5000 + Policy::DefaultMaturityMs));
    BOOST_TEST(IsAction(rearmed, Action::Apply));
    BOOST_TEST(rearmed.rate_bytes_per_second > Megabyte);
}

BOOST_AUTO_TEST_CASE(carrier_counter_ratio_paces_at_the_measured_wire_rate) {
    // Production conversion: the carrier's cumulative byte counters give the
    // framing ratio (all frames written per unique payload byte) as a fraction
    // in permille, rounded up so it can only pace looser. These are the r40
    // bidirectional counters of one path (tx_bytes / tx_unique_payload_bytes),
    // where the framing overhead was 0.045%.
    const std::uint64_t wire_bytes = 420764012;
    const std::uint64_t payload_bytes = 420574598;
    const std::uint64_t scaled = wire_bytes * 1000;
    const std::uint64_t ratio_permille =
        scaled / payload_bytes + (scaled % payload_bytes != 0 ? 1 : 0);
    BOOST_TEST(ratio_permille == 1001u);

    // A measured framing ratio converts the payload capacity into a wire rate
    // that sits above the payload sample that produced it, so the cap cannot
    // bound the next delivery sample (the invariant the policy relies on).
    constexpr std::uint64_t capacity = 8 * Megabyte;
    BOOST_TEST(Policy::DeriveRate(capacity, ratio_permille, 1000) == 8008000u);
    BOOST_TEST(Policy::DeriveRate(capacity, ratio_permille, 1000) > capacity);

    // Rounding up is what keeps that invariant under a sub-permille framing
    // overhead: truncating 0.045% to 1000/1000 would put the cap exactly at the
    // sample it must stay above.
    BOOST_TEST(Policy::DeriveRate(capacity, 1000, 1000) == capacity);
    BOOST_TEST(Policy::DeriveRate(capacity, ratio_permille, 1000) > capacity);
}

