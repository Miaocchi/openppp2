#define BOOST_TEST_MODULE vmux_wave3_per_link_rtt_test
#include <boost/test/included/unit_test.hpp>

/**
 * @file vmux_wave3_per_link_rtt_test.cpp
 * @brief Wave 3 test: per-link Path State — RTT attribution, EWMA, schedulable.
 * @license GPL-3.0
 *
 * Tests the Wave 3 changes:
 *   - Track() stores link_id; Ack() returns it for per-link RTT attribution
 *   - vmux_linklayer per-link SRTT/RTTVAR/min_rtt fields
 *   - min_rtt ignores the first (possibly queue-inflated) sample and only
 *     accepts a later confirmed floor with bounded downward convergence
 *   - schedulable() unified predicate (handshake_complete && !retiring)
 *   - Per-link PTO derivation (QUIC-style SRTT + max(4*RTTVAR, 1), clamped)
 *
 * These tests exercise MuxRetransmitBuffer and MuxLinkDrainState directly
 * (no vmux_net instance needed) plus a simulated per-link EWMA update path
 * that mirrors packet_input_ack's logic.
 */

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <ppp/app/multipath/Scheduler.h>

#include <ppp/app/mux/MuxRetransmitBuffer.h>
#include <ppp/app/mux/MuxLinkDrainState.h>

#include "support/FakeClock.h"

namespace mux = ppp::app::mux;
using ppp::test::FakeClock;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::shared_ptr<std::uint8_t> make_buffer(int length, std::uint32_t seed) {
    auto buf = std::shared_ptr<std::uint8_t>(
        new std::uint8_t[static_cast<std::size_t>(length)],
        std::default_delete<std::uint8_t[]>());
    for (int i = 0; i < length; ++i) {
        buf.get()[i] = static_cast<std::uint8_t>((seed * 31 + i * 7 + 0x5A) & 0xFF);
    }
    return buf;
}

/**
 * Mirror of the per-link update in note_link_rtt / packet_input_ack.
 * First sample → srtt=sample, rttvar=sample/2, min_rtt stays unknown(0)
 *                because the first ACK RTT may be queue-inflated.
 * Second sample → establishes min_rtt=min(first, second): the candidate
 *                 minimum of the two readings, so neither an inflated first
 *                 sample nor a high second sample sets the floor alone.
 * Later samples → rttvar=(rttvar*3+|srtt-sample|)/4,
 *                 srtt=(srtt*7+sample)/8, and a lower candidate moves the
 *                 floor by at most a quarter of the current floor per sample
 *                 (controlled convergence; a transient low cannot pin it).
 */
struct LinkPathState {
    uint64_t srtt_ms = 0;
    uint64_t rttvar_ms = 0;
    uint64_t min_rtt_ms = 0;
    bool has_sample = false;

    void update(uint64_t sample_ms) noexcept {
        if (!has_sample) {
            srtt_ms = sample_ms;
            rttvar_ms = sample_ms / 2;
            has_sample = true;
        } else {
            const uint64_t previous_srtt = srtt_ms; // one-sample seed only
            uint64_t diff = (srtt_ms >= sample_ms) ? (srtt_ms - sample_ms) : (sample_ms - srtt_ms);
            rttvar_ms = (rttvar_ms * 3 + diff) / 4;
            srtt_ms = (srtt_ms * 7 + sample_ms) / 8;
            if (min_rtt_ms == 0) {
                // Candidate minimum of the two readings seen so far.
                min_rtt_ms = std::min(sample_ms, previous_srtt);
            } else if (sample_ms < min_rtt_ms) {
                const uint64_t step = std::max<std::uint64_t>(1, min_rtt_ms / 4);
                min_rtt_ms = std::max(sample_ms, min_rtt_ms - step);
            }
        }
    }

    /** QUIC-style PTO = SRTT + max(4*RTTVAR, 1), clamped to [200, 3000]. */
    uint64_t pto() const noexcept {
        if (!has_sample) return 0; // caller falls back to session PTO
        uint64_t var = rttvar_ms * 4;
        if (var < 1) var = 1;
        uint64_t pto = srtt_ms + var;
        if (pto < 200) pto = 200;
        if (pto > 3000) pto = 3000;
        return pto;
    }
};

// ---------------------------------------------------------------------------
// 1. Track() stores link_id; Ack() returns it
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(track_stores_link_id_and_ack_returns_it) {
    mux::MuxRetransmitBuffer rtx;
    const std::size_t cap = 1 << 20;

    // Track on link 3.
    BOOST_TEST(rtx.Track(1, 1, make_buffer(100, 0xA01), 100, 1000, cap, 3));

    std::vector<std::uint64_t> fast;
    std::vector<mux::MuxAckRange> ranges = {{1, 1}};
    std::uint16_t out_link_id = 0xFFFF;
    const uint64_t sample = rtx.Ack(1, 1, ranges, 1100, 3, 0, fast, &out_link_id);

    BOOST_TEST(sample == 100u);  // 1100 - 1000
    BOOST_TEST(out_link_id == 3u);
}

BOOST_AUTO_TEST_CASE(track_default_link_id_is_zero) {
    mux::MuxRetransmitBuffer rtx;
    const std::size_t cap = 1 << 20;

    // No link_id specified → defaults to 0.
    BOOST_TEST(rtx.Track(1, 1, make_buffer(100, 0xA02), 100, 1000, cap));

    std::vector<std::uint64_t> fast;
    std::vector<mux::MuxAckRange> ranges = {{1, 1}};
    std::uint16_t out_link_id = 0xFFFF;
    rtx.Ack(1, 1, ranges, 1100, 3, 0, fast, &out_link_id);

    BOOST_TEST(out_link_id == 0u);
}

BOOST_AUTO_TEST_CASE(ack_returns_link_id_of_first_non_retransmitted_acked_frame) {
    // Two frames on different links. seq=1 on link 2, seq=2 on link 5.
    // ACK covers both. The first non-retransmitted entry determines the sample
    // and link_id. The iteration order is map-based, so we verify that the
    // returned link_id corresponds to a valid sample.
    mux::MuxRetransmitBuffer rtx;
    const std::size_t cap = 1 << 20;

    BOOST_TEST(rtx.Track(1, 1, make_buffer(100, 0xA10), 100, 1000, cap, 2));
    BOOST_TEST(rtx.Track(1, 2, make_buffer(100, 0xA11), 100, 1010, cap, 5));

    std::vector<std::uint64_t> fast;
    std::vector<mux::MuxAckRange> ranges = {{1, 2}};
    std::uint16_t out_link_id = 0xFFFF;
    const uint64_t sample = rtx.Ack(1, 2, ranges, 1100, 3, 0, fast, &out_link_id);

    // Sample must be non-zero (both frames were non-retransmitted).
    BOOST_TEST(sample > 0u);
    // link_id must be one of the two links.
    BOOST_TEST((out_link_id == 2u || out_link_id == 5u));
}

BOOST_AUTO_TEST_CASE(ack_returns_zero_link_id_when_all_frames_retransmitted) {
    // Karn's rule: retransmitted frames don't produce RTT samples.
    mux::MuxRetransmitBuffer rtx;
    const std::size_t cap = 1 << 20;

    BOOST_TEST(rtx.Track(1, 1, make_buffer(100, 0xA20), 100, 1000, cap, 7));
    rtx.MarkRetransmitted(mux::MuxRetransmitBuffer::Key(1, 1), 1500, 1);

    std::vector<std::uint64_t> fast;
    std::vector<mux::MuxAckRange> ranges = {{1, 1}};
    std::uint16_t out_link_id = 0xFFFF;
    const uint64_t sample = rtx.Ack(1, 1, ranges, 1600, 3, 0, fast, &out_link_id);

    BOOST_TEST(sample == 0u);
    // out_link_id should be 0 (no sample → not set beyond initial zero).
    BOOST_TEST(out_link_id == 0u);
}

BOOST_AUTO_TEST_CASE(ack_out_link_id_nullptr_is_safe) {
    mux::MuxRetransmitBuffer rtx;
    const std::size_t cap = 1 << 20;

    BOOST_TEST(rtx.Track(1, 1, make_buffer(100, 0xA30), 100, 1000, cap, 4));

    std::vector<std::uint64_t> fast;
    std::vector<mux::MuxAckRange> ranges = {{1, 1}};
    // nullptr out_link_id must not crash.
    const uint64_t sample = rtx.Ack(1, 1, ranges, 1100, 3, 0, fast, nullptr);
    BOOST_TEST(sample == 100u);
}

// ---------------------------------------------------------------------------
// 2. Per-link EWMA (SRTT/RTTVAR + confirmed min_rtt)
// ---------------------------------------------------------------------------

/** Mirror of get_link_telemetry's min-RTT BDP: unknown RTT ⇒ zero BDP. */
static uint64_t BdpFromMinRtt(uint64_t rate_bytes_per_second,
    const LinkPathState& state) noexcept {
    return state.min_rtt_ms != 0
        ? rate_bytes_per_second * state.min_rtt_ms / 1000 : 0;
}

BOOST_AUTO_TEST_CASE(ewma_first_sample_seeds_srtt_but_leaves_min_rtt_unknown) {
    LinkPathState state;
    state.update(80);

    BOOST_TEST(state.has_sample);
    BOOST_TEST(state.srtt_ms == 80u);
    BOOST_TEST(state.rttvar_ms == 40u);  // 80/2
    // The first ACK RTT may include startup/reverse-path queueing and must not
    // become the permanent floor by itself.
    BOOST_TEST(state.min_rtt_ms == 0u);
}

BOOST_AUTO_TEST_CASE(first_queued_sample_374_cannot_become_min_rtt_baseline) {
    LinkPathState state;
    state.update(374);  // upload path1 first sample, true carrier min is 90 ms.

    BOOST_TEST(state.srtt_ms == 374u);
    BOOST_TEST(state.rttvar_ms == 187u);
    BOOST_TEST(state.min_rtt_ms == 0u);
    // No confirmed floor ⇒ no BDP-derived credit yet (conservative startup).
    BOOST_TEST(BdpFromMinRtt(13210000u, state) == 0u);
}

BOOST_AUTO_TEST_CASE(min_rtt_converges_from_first_374_through_122_to_90) {
    LinkPathState state;
    state.update(374);
    BOOST_TEST(state.min_rtt_ms == 0u);

    // Second sample establishes a conservative floor; it cannot remain 374.
    state.update(122);
    BOOST_TEST(state.min_rtt_ms == 122u);

    // A genuine lower reading is accepted through bounded convergence rather
    // than a jump to the raw sample, so one low sample cannot pin the floor.
    state.update(90);
    BOOST_TEST(state.min_rtt_ms < 122u);
    BOOST_TEST(state.min_rtt_ms >= 90u);

    // A second confirming 90 ms sample reaches the true minimum exactly.
    state.update(90);
    BOOST_TEST(state.min_rtt_ms == 90u);
    // Only the floor moves; the SRTT EWMA keeps its own protocol semantics.
    BOOST_TEST(state.srtt_ms == 282u);
}

BOOST_AUTO_TEST_CASE(single_transient_low_sample_cannot_pin_min_rtt) {
    LinkPathState state;
    state.update(374);
    state.update(122);   // confirmed floor
    state.update(1);     // one-off scheduling/timer artifact

    // At most a quarter of the floor is lost: 122 - 122/4 = 92, not 1.
    BOOST_TEST(state.min_rtt_ms == 92u);
    state.update(122);
    BOOST_TEST(state.min_rtt_ms == 92u);
}

BOOST_AUTO_TEST_CASE(bdp_and_credit_follow_confirmed_floor_not_first_sample) {
    using Scheduler = ppp::app::multipath::PathScheduler;
    constexpr uint64_t rate = 13210000u;       // measured path1 capacity B/s
    constexpr uint64_t mib = 1024u * 1024u;
    constexpr uint64_t session_window = 64u * mib;
    constexpr uint64_t startup_window = 256u * 1024u;
    constexpr uint64_t minimum_window = 256u * 1024u;
    constexpr std::size_t live_carriers = 3;

    const auto credit_for = [&](const LinkPathState& state) -> uint64_t {
        const uint64_t bdp = BdpFromMinRtt(rate, state);
        const std::optional<uint64_t> min_rtt = state.min_rtt_ms != 0
            ? std::optional<uint64_t>(state.min_rtt_ms) : std::nullopt;
        const std::optional<uint64_t> observed = state.srtt_ms != 0
            ? std::optional<uint64_t>(state.srtt_ms) : std::nullopt;
        return Scheduler::AdaptiveFlightCredit(
            session_window, live_carriers, bdp, bdp, 100,
            startup_window, minimum_window, 0, min_rtt, observed);
    };

    LinkPathState state;
    // Unknown RTT: both BDP and adaptive credit stay at the startup floor.
    BOOST_TEST(BdpFromMinRtt(rate, state) == 0u);
    BOOST_TEST(credit_for(state) == startup_window);

    // The local write-window policy uses the same min-RTT gate: an unknown
    // floor must not turn a fresh capacity sample into a large queue target.
    ppp::app::multipath::PathTelemetry telemetry;
    telemetry.capacity_rate_bytes_per_second = rate;
    telemetry.capacity_updated_ms = 1000;
    const auto unknown_window = Scheduler::ComputeLinkWriteWindow(
        telemetry, 1000, 0, 0, 0);
    BOOST_TEST(!unknown_window.capacity_known);
    BOOST_TEST(unknown_window.target_bytes ==
        Scheduler::LinkWriteHighWaterFloorBytes);

    state.update(374);
    // First sample alone is still unknown for BDP/credit.
    BOOST_TEST(BdpFromMinRtt(rate, state) == 0u);
    BOOST_TEST(credit_for(state) == startup_window);
    telemetry.min_rtt_ms = state.min_rtt_ms;
    telemetry.srtt_ms = state.srtt_ms;
    const auto first_window = Scheduler::ComputeLinkWriteWindow(
        telemetry, 1000, 0, 0, 0);
    BOOST_TEST(!first_window.capacity_known);
    BOOST_TEST(first_window.target_bytes ==
        Scheduler::LinkWriteHighWaterFloorBytes);

    state.update(122);
    const uint64_t bdp_122 = rate * 122u / 1000u;
    const uint64_t bdp_374 = rate * 374u / 1000u;
    // r61 scale check: the rejected first sample would have sized a ~3.1x
    // larger BDP than the first confirmed 122 ms floor.
    BOOST_TEST(bdp_374 > 3u * bdp_122);
    BOOST_TEST(BdpFromMinRtt(rate, state) == bdp_122);
    BOOST_TEST(BdpFromMinRtt(rate, state) < bdp_374);
    telemetry.min_rtt_ms = state.min_rtt_ms;
    telemetry.srtt_ms = state.srtt_ms;
    const auto confirmed_window = Scheduler::ComputeLinkWriteWindow(
        telemetry, 1000, 0, 0, 0);
    BOOST_TEST(confirmed_window.capacity_known);
    BOOST_TEST(confirmed_window.target_bytes == bdp_122);
    const uint64_t credit_122 = credit_for(state);
    BOOST_TEST(credit_122 >= bdp_122);   // measured flight still credited
    BOOST_TEST(credit_122 < 2u * bdp_374); // never the stuck first-sample grant

    state.update(90);
    state.update(90);
    const uint64_t bdp_90 = rate * 90u / 1000u;
    BOOST_TEST(state.min_rtt_ms == 90u);
    BOOST_TEST(BdpFromMinRtt(rate, state) == bdp_90);
    const uint64_t credit_90 = credit_for(state);
    BOOST_TEST(credit_90 >= bdp_90);
    // Credit follows the confirmed lower floor down instead of remaining
    // pinned to the 122 ms or 374 ms estimate.
    BOOST_TEST(credit_90 < credit_122);
}

BOOST_AUTO_TEST_CASE(ewma_subsequent_updates_converge_to_true_rtt) {
    LinkPathState state;

    // Simulate 20 samples at 100ms RTT.
    for (int i = 0; i < 20; ++i) {
        state.update(100);
    }

    // SRTT should be very close to 100.
    BOOST_TEST(state.srtt_ms >= 98u);
    BOOST_TEST(state.srtt_ms <= 102u);
    // RTTVAR should be very small (stable RTT).
    BOOST_TEST(state.rttvar_ms <= 5u);
    BOOST_TEST(state.min_rtt_ms == 100u);
}

BOOST_AUTO_TEST_CASE(ewma_rttvar_increases_with_jitter) {
    LinkPathState state;

    // Alternating 50/150ms → mean 100, high variance.
    for (int i = 0; i < 20; ++i) {
        state.update(i % 2 == 0 ? 50 : 150);
    }

    // RTTVAR should be significant.
    BOOST_TEST(state.rttvar_ms > 20u);
    BOOST_TEST(state.min_rtt_ms == 50u);
}

BOOST_AUTO_TEST_CASE(ewma_min_rtt_tracks_minimum_not_average) {
    LinkPathState state;

    state.update(100);
    state.update(50);   // new minimum
    state.update(200);
    state.update(80);

    BOOST_TEST(state.min_rtt_ms == 50u);
}

BOOST_AUTO_TEST_CASE(ewma_srtt_weighted_by_7_8_toward_old_value) {
    // After first sample of 100, a second sample of 200 should produce:
    // srtt = (100*7 + 200) / 8 = 900/8 = 112
    LinkPathState state;
    state.update(100);
    state.update(200);

    BOOST_TEST(state.srtt_ms == 112u);
    // rttvar = (50*3 + |100-200|) / 4 = (150+100)/4 = 62
    BOOST_TEST(state.rttvar_ms == 62u);
}

// ---------------------------------------------------------------------------
// 3. Multi-link RTT isolation
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(multi_link_rtt_isolation) {
    // Simulate two links with different RTT profiles.
    // Link A: 20ms stable. Link B: 300ms stable.
    LinkPathState link_a, link_b;

    for (int i = 0; i < 20; ++i) {
        link_a.update(20);
        link_b.update(300);
    }

    // Link A should converge to ~20ms, link B to ~300ms.
    BOOST_TEST(link_a.srtt_ms >= 19u);
    BOOST_TEST(link_a.srtt_ms <= 21u);
    BOOST_TEST(link_b.srtt_ms >= 298u);
    BOOST_TEST(link_b.srtt_ms <= 302u);

    // Min RTTs should reflect the actual minimums.
    BOOST_TEST(link_a.min_rtt_ms == 20u);
    BOOST_TEST(link_b.min_rtt_ms == 300u);

    // Link A PTO should be much smaller than Link B PTO.
    BOOST_TEST(link_a.pto() < link_b.pto());
}

BOOST_AUTO_TEST_CASE(rtt_sample_attribution_uses_correct_link_id) {
    // Verify that when ACK comes back, the RTT sample is attributed to
    // the correct link's path state, not a global SRTT.
    mux::MuxRetransmitBuffer rtx;
    const std::size_t cap = 1 << 20;
    LinkPathState link_a, link_b;

    // Frame 1 sent on link A at t=1000, RTT=20ms → ACK at t=1020.
    rtx.Track(1, 1, make_buffer(100, 0xB01), 100, 1000, cap, 1);

    // Frame 2 sent on link B at t=1000, RTT=300ms → ACK at t=1300.
    rtx.Track(1, 2, make_buffer(100, 0xB02), 100, 1000, cap, 2);

    // ACK for seq=1 at t=1020.
    std::vector<std::uint64_t> fast1;
    std::vector<mux::MuxAckRange> ranges1 = {{1, 1}};
    std::uint16_t link_id1 = 0;
    uint64_t sample1 = rtx.Ack(1, 1, ranges1, 1020, 3, 0, fast1, &link_id1);

    BOOST_TEST(sample1 == 20u);
    BOOST_TEST(link_id1 == 1u);
    link_a.update(sample1);

    // ACK for seq=2 at t=1300.
    std::vector<std::uint64_t> fast2;
    std::vector<mux::MuxAckRange> ranges2 = {{2, 2}};
    std::uint16_t link_id2 = 0;
    uint64_t sample2 = rtx.Ack(1, 2, ranges2, 1300, 3, 0, fast2, &link_id2);

    BOOST_TEST(sample2 == 300u);
    BOOST_TEST(link_id2 == 2u);
    link_b.update(sample2);

    // Verify isolation.
    BOOST_TEST(link_a.srtt_ms == 20u);
    BOOST_TEST(link_b.srtt_ms == 300u);
}

// ---------------------------------------------------------------------------
// 4. Schedulable predicate (handshake_complete && !retiring)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(schedulable_true_when_handshake_done_and_not_retiring) {
    // Directly test the logic: handshake_complete=true, retiring=false → schedulable.
    mux::MuxLinkDrainState drain;
    // drain starts not-retiring.
    BOOST_TEST(!drain.retiring());

    // Simulate handshake_complete=true (we can't set the atomic directly,
    // but we can verify the predicate logic).
    bool handshake_complete = true;
    bool schedulable = handshake_complete && !drain.retiring();
    BOOST_TEST(schedulable);
}

BOOST_AUTO_TEST_CASE(schedulable_false_when_retiring) {
    mux::MuxLinkDrainState drain;
    drain.BeginRetire();
    BOOST_TEST(drain.retiring());

    bool handshake_complete = true;
    bool schedulable = handshake_complete && !drain.retiring();
    BOOST_TEST(!schedulable);
}

BOOST_AUTO_TEST_CASE(schedulable_false_when_handshake_incomplete) {
    mux::MuxLinkDrainState drain;
    // Not retiring, but handshake not done.
    BOOST_TEST(!drain.retiring());

    bool handshake_complete = false;
    bool schedulable = handshake_complete && !drain.retiring();
    BOOST_TEST(!schedulable);
}

BOOST_AUTO_TEST_CASE(schedulable_false_when_both_conditions_fail) {
    mux::MuxLinkDrainState drain;
    drain.BeginRetire();
    BOOST_TEST(drain.retiring());

    bool handshake_complete = false;
    bool schedulable = handshake_complete && !drain.retiring();
    BOOST_TEST(!schedulable);
}

// ---------------------------------------------------------------------------
// 5. Per-link PTO derivation (QUIC-style, clamped)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(pto_first_sample_clamped_to_200ms_minimum) {
    // RTT=5ms → SRTT=5, RTTVAR=2, 4*RTTVAR=8, PTO=13 → clamped to 200.
    LinkPathState state;
    state.update(5);

    BOOST_TEST(state.pto() == 200u);
}

BOOST_AUTO_TEST_CASE(pto_stable_100ms_rtt) {
    // RTT=100ms stable → SRTT≈100, RTTVAR≈0, 4*RTTVAR=1, PTO≈101.
    LinkPathState state;
    for (int i = 0; i < 20; ++i) {
        state.update(100);
    }

    BOOST_TEST(state.pto() >= 200u);  // clamped minimum
    BOOST_TEST(state.pto() <= 250u);
}

BOOST_AUTO_TEST_CASE(pto_high_rtt_clamped_to_3000ms_maximum) {
    // RTT=5000ms → SRTT=5000, PTO would be ~5000+something → clamped to 3000.
    LinkPathState state;
    state.update(5000);

    BOOST_TEST(state.pto() == 3000u);
}

BOOST_AUTO_TEST_CASE(pto_increases_with_rttvar) {
    // Stable 100ms vs jittery 50/150ms.
    LinkPathState stable, jittery;

    for (int i = 0; i < 20; ++i) {
        stable.update(100);
        jittery.update(i % 2 == 0 ? 50 : 150);
    }

    // Jittery link should have higher PTO due to RTTVAR.
    BOOST_TEST(jittery.pto() > stable.pto());
}

BOOST_AUTO_TEST_CASE(pto_zero_when_no_sample) {
    // No sample → PTO returns 0, caller falls back to session PTO.
    LinkPathState state;
    BOOST_TEST(state.pto() == 0u);
    BOOST_TEST(!state.has_sample);
}

// ---------------------------------------------------------------------------
// 6. RTX with per-link attribution end-to-end simulation
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(multi_link_rtx_attribution_and_pto_divergence) {
    // Simulate a scenario:
    // - Link 1 (fast, 20ms RTT): frames 1, 3, 5
    // - Link 2 (slow, 300ms RTT): frames 2, 4, 6
    // - ACKs come back at different times
    // - Verify per-link PTO divergence
    mux::MuxRetransmitBuffer rtx;
    const std::size_t cap = 1 << 20;
    LinkPathState link1_state, link2_state;

    // Send frames at t=1000.
    rtx.Track(1, 1, make_buffer(100, 0xC01), 100, 1000, cap, 1);
    rtx.Track(1, 2, make_buffer(100, 0xC02), 100, 1000, cap, 2);
    rtx.Track(1, 3, make_buffer(100, 0xC03), 100, 1000, cap, 1);
    rtx.Track(1, 4, make_buffer(100, 0xC04), 100, 1000, cap, 2);
    rtx.Track(1, 5, make_buffer(100, 0xC05), 100, 1000, cap, 1);
    rtx.Track(1, 6, make_buffer(100, 0xC06), 100, 1000, cap, 2);

    // ACK for link-1 frames (1,3,5) at t=1020 (20ms RTT).
    std::vector<std::uint64_t> fast1;
    std::vector<mux::MuxAckRange> ranges1 = {{1, 1}, {3, 3}, {5, 5}};
    std::uint16_t lid1 = 0;
    uint64_t s1 = rtx.Ack(1, 5, ranges1, 1020, 3, 0, fast1, &lid1);
    if (s1 > 0) {
        link1_state.update(s1);
    }

    // ACK for link-2 frames (2,4,6) at t=1300 (300ms RTT).
    std::vector<std::uint64_t> fast2;
    std::vector<mux::MuxAckRange> ranges2 = {{2, 2}, {4, 4}, {6, 6}};
    std::uint16_t lid2 = 0;
    uint64_t s2 = rtx.Ack(1, 6, ranges2, 1300, 3, 0, fast2, &lid2);
    if (s2 > 0) {
        link2_state.update(s2);
    }

    // One confirmatory sample per link is required before min_rtt is defined
    // (the first sample may be queue-inflated and is never used alone).
    if (s1 > 0) {
        link1_state.update(s1);
    }
    if (s2 > 0) {
        link2_state.update(s2);
    }

    // All entries should be released.
    BOOST_TEST(rtx.size() == 0u);

    // Per-link states should have divergent PTOs.
    BOOST_TEST(link1_state.has_sample);
    BOOST_TEST(link2_state.has_sample);
    BOOST_TEST(link1_state.pto() < link2_state.pto());
    BOOST_TEST(link1_state.min_rtt_ms <= 20u);
    BOOST_TEST(link2_state.min_rtt_ms >= 300u);
}
