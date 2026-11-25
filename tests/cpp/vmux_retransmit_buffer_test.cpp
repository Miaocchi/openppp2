#define BOOST_TEST_MODULE vmux_retransmit_buffer_test
#include <boost/test/included/unit_test.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <ppp/app/mux/MuxRetransmitBuffer.h>

namespace mux = ppp::app::mux;

namespace {
    std::shared_ptr<std::uint8_t> make_frame(int value) {
        auto buffer = std::make_shared<std::uint8_t>(static_cast<std::uint8_t>(value));
        return buffer;
    }
}

BOOST_AUTO_TEST_CASE(track_retain_and_release_on_ack) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_TEST(rtx.Track(7, 1, make_frame(1), 100, 1000, 1 << 20));
    BOOST_TEST(rtx.Track(7, 2, make_frame(2), 100, 1010, 1 << 20));
    BOOST_TEST(rtx.size() == 2u);
    BOOST_TEST(rtx.bytes() == 200u);

    std::vector<std::uint64_t> fast;
    std::vector<mux::MuxAckRange> ranges = { {1, 1} };
    const std::uint64_t sample = rtx.Ack(7, 1, ranges, 1100, 3, 0, fast);

    BOOST_TEST(sample == 100u); // 1100 - 1000, entry never retransmitted.
    BOOST_TEST(rtx.size() == 1u);
    BOOST_TEST(rtx.bytes() == 100u);
    BOOST_TEST(fast.empty());
}

BOOST_AUTO_TEST_CASE(orphaned_carrier_cannot_measure_a_replacement_with_the_same_id) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_REQUIRE(rtx.Track(7, 1, make_frame(1), 100, 1000, 1024, 3));
    std::vector<std::uint64_t> pending;
    rtx.TakeLinkInflight(3, pending);
    BOOST_REQUIRE_EQUAL(pending.size(), 1u);
    BOOST_TEST(rtx.LinkBytes(3) == 0u);
    BOOST_REQUIRE(rtx.Track(7, 2, make_frame(2), 50, 1050, 1024, 3));
    std::vector<std::uint64_t> fast;
    std::uint16_t sample_link = 9;
    std::unordered_map<std::uint16_t, std::size_t> acknowledged;
    rtx.Ack(7, 1, {{1, 1}}, 1100, 3, 0, fast, &sample_link, &acknowledged);
    BOOST_TEST(sample_link == 0u);
    BOOST_TEST(acknowledged.empty());
    BOOST_TEST(rtx.LinkBytes(3) == 50u);
}

BOOST_AUTO_TEST_CASE(track_refuses_to_exceed_the_byte_cap) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_TEST(rtx.Track(0, 1, make_frame(1), 400, 1000, 500));
    BOOST_TEST(!rtx.Track(0, 2, make_frame(2), 400, 1000, 500));
    BOOST_TEST(!rtx.Track(1, 1, make_frame(3), 0, 1000, 0));
    BOOST_TEST(!rtx.Track(1, 1, make_frame(3), -1, 1000, 0));
    BOOST_TEST(rtx.size() == 1u);
    BOOST_TEST(rtx.bytes() == 400u);
}

BOOST_AUTO_TEST_CASE(disabled_fast_retransmit_releases_acked_frames_without_marking_holes) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_REQUIRE(rtx.Track(7, 1, make_frame(1), 100, 1000, 1024, 2));
    BOOST_REQUIRE(rtx.Track(7, 4, make_frame(4), 100, 1010, 1024, 3));
    BOOST_REQUIRE(rtx.Track(9, 4, make_frame(4), 40, 1020, 1024, 1));

    std::vector<std::uint64_t> fast;
    std::uint16_t link = 0;
    std::unordered_map<std::uint16_t, std::size_t> acked;
    const auto sample = rtx.Ack(7, 4, {{4, 4}}, 1100, 3, 0, fast, &link, &acked,
        nullptr, nullptr, false);
    BOOST_TEST(sample == 90u);
    BOOST_TEST(link == 3u);
    BOOST_TEST(acked.at(3) == 100u);
    BOOST_TEST(rtx.CidBytes(7) == 100u);
    BOOST_TEST(rtx.CidBytes(9) == 40u);
    BOOST_TEST(fast.empty());

    // A duplicate ACK can still discover the hole if collection is requested.
    rtx.Ack(7, 4, {{4, 4}}, 1101, 3, 0, fast, &link, &acked);
    BOOST_REQUIRE_EQUAL(fast.size(), 1u);
    BOOST_TEST(fast.front() == mux::MuxRetransmitBuffer::Key(7, 1));
    BOOST_TEST(acked.empty());

    rtx.EraseCid(7);
    link = 9;
    acked[3] = 100;
    BOOST_TEST(rtx.Ack(7, 4, {{1, 4}}, 1102, 3, 0, fast, &link, &acked) == 0u);
    BOOST_TEST(link == 0u);
    BOOST_TEST(acked.empty());
    BOOST_TEST(fast.size() == 1u);
    BOOST_TEST(rtx.bytes() == 40u);
}

BOOST_AUTO_TEST_CASE(fast_retransmit_candidates_follow_the_distance_rule) {
    mux::MuxRetransmitBuffer rtx;
    for (std::uint32_t seq = 1; seq <= 6; ++seq) {
        BOOST_TEST(rtx.Track(3, seq, make_frame((int)seq), 10, 1000, 1 << 20));
    }

    // ACK covers 4..6 with largest 6: seq 1..3 sit at least 3 below largest.
    std::vector<std::uint64_t> fast;
    std::vector<mux::MuxAckRange> ranges = { {4, 6} };
    rtx.Ack(3, 6, ranges, 2000, 3, 0, fast);

    BOOST_TEST(rtx.size() == 3u); // 4..6 released.
    BOOST_REQUIRE_EQUAL(fast.size(), 3u);
    for (std::uint64_t key : fast) {
        const std::uint32_t seq = static_cast<std::uint32_t>(key & 0xFFFFFFFFu);
        BOOST_TEST(seq <= 3u);
    }

    // Same largest again: no duplicate candidates (fast_rtx_mark dedup).
    std::vector<std::uint64_t> fast_again;
    std::vector<mux::MuxAckRange> ranges_again = { {7, 7} };
    rtx.Track(3, 7, make_frame(7), 10, 1000, 1 << 20);
    rtx.Ack(3, 6, ranges_again, 2100, 3, 0, fast_again); // largest unchanged at 6
    BOOST_TEST(fast_again.empty());
}

BOOST_AUTO_TEST_CASE(no_rtt_sample_from_retransmitted_frames) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_TEST(rtx.Track(0, 1, make_frame(1), 10, 1000, 1 << 20));
    rtx.MarkRetransmitted(mux::MuxRetransmitBuffer::Key(0, 1), 1500);

    std::vector<std::uint64_t> fast;
    std::vector<mux::MuxAckRange> ranges = { {1, 1} };
    const std::uint64_t sample = rtx.Ack(0, 1, ranges, 2000, 3, 0, fast);
    BOOST_TEST(sample == 0u); // Karn's rule: retransmitted frames give no sample.
}

BOOST_AUTO_TEST_CASE(advancing_acks_do_not_retry_a_recent_replacement) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_REQUIRE(rtx.Track(9, 1, make_frame(1), 100, 1000, 1024, 1));
    const auto key = rtx.Key(9, 1);
    std::vector<std::uint64_t> fast;
    rtx.Ack(9, 8, {{8, 8}}, 1200, 3, 50, fast);
    BOOST_REQUIRE_EQUAL(fast.size(), 1u);
    rtx.MarkRetransmitted(key, 1200, 2);

    // Delayed ACKs for later original packets arrive while the replacement
    // is still on its way. Acting on each hint used to spend all eight retries.
    for (std::uint32_t i = 1; i <= 10; ++i) {
        fast.clear();
        rtx.Ack(9, 8 + i, {{8 + i, 8 + i}}, 1200 + i, 3, 50, fast);
        if (!fast.empty()) rtx.MarkRetransmitted(key, 1200 + i, 2);
    }
    BOOST_TEST(rtx.Find(key)->attempts == 1u);
    fast.clear();
    BOOST_REQUIRE(rtx.Track(9, 22, make_frame(22), 100, 1201, 1024, 2));
    rtx.Ack(9, 22, {{22, 22}}, 1250, 3, 50, fast);
    BOOST_REQUIRE_EQUAL(fast.size(), 1u);
    BOOST_TEST(fast.front() == key);
}

BOOST_AUTO_TEST_CASE(delayed_original_acks_cannot_spend_the_replacements_retry_budget) {
    mux::MuxRetransmitBuffer rtx;
    for (unsigned seq = 1; seq <= 12; ++seq) {
        BOOST_REQUIRE(rtx.Track(9, seq, make_frame(seq), 100, 1000, 4096, 1));
    }
    const auto key = rtx.Key(9, 1);
    std::vector<std::uint64_t> fast;
    rtx.Ack(9, 4, {{2, 4}}, 1200, 3, 25, fast);
    BOOST_REQUIRE_EQUAL(fast.size(), 1u);
    rtx.MarkRetransmitted(key, 1200, 2);
    for (unsigned seq = 5; seq <= 12; ++seq) {
        fast.clear();
        const auto now = 1200 + 125 * (seq - 4);
        rtx.Ack(9, seq, {{2, seq}}, now, 3, 25, fast);
        if (!fast.empty()) rtx.MarkRetransmitted(key, now, 2);
    }
    BOOST_TEST(rtx.Find(key)->attempts == 1u);
    BOOST_REQUIRE(rtx.Track(9, 13, make_frame(13), 100, 2210, 4096, 1));
    fast.clear();
    rtx.Ack(9, 13, {{2, 13}}, 2300, 3, 25, fast);
    BOOST_REQUIRE_EQUAL(fast.size(), 1u);
    BOOST_TEST(fast.front() == key);
}

BOOST_AUTO_TEST_CASE(pending_hole_has_a_single_retry_across_ack_pto_and_carrier_exit) {
    mux::MuxRetransmitBuffer rtx;
    const auto key = rtx.Key(9, 1);
    BOOST_REQUIRE(rtx.Track(9, 1, make_frame(1), 100, 1000, 1024, 1));
    std::vector<std::uint64_t> pending, fast, expired;
    BOOST_REQUIRE(rtx.QueueRetry(key, pending, "fast"));
    rtx.Ack(9, 9, {{9, 9}}, 2000, 3, 50, fast);
    rtx.CollectExpired(2000, 100, 60000, 32, expired);
    BOOST_TEST(fast.empty());
    BOOST_TEST(expired.empty());
    rtx.TakeLinkInflight(1, pending);
    BOOST_REQUIRE_EQUAL(pending.size(), 1u);
    BOOST_TEST(rtx.Find(key)->last_link_id == 0u);
    BOOST_TEST(rtx.LinkBytes(1) == 0u);
    rtx.MarkRetransmitted(key, 2000, 2);
    BOOST_TEST(!rtx.Find(key)->retry_source);
    rtx.TakeLinkInflight(2, fast);
    BOOST_REQUIRE_EQUAL(fast.size(), 1u);
    BOOST_TEST(fast.front() == key);
}

BOOST_AUTO_TEST_CASE(pto_tracks_the_current_carrier_and_falls_back_for_unknown_links) {
    mux::MuxRetransmitBuffer rtx;
    const std::unordered_map<std::uint16_t, std::uint64_t> ptos = {{1, 900}, {2, 100}};
    for (std::uint16_t link = 1; link <= 3; ++link) {
        BOOST_REQUIRE(rtx.Track(9, link, make_frame(link), 100, 1000, 1024, link));
    }
    std::vector<std::uint64_t> expired;
    rtx.CollectExpired(1300, 200, 60000, 32, expired, 0, &ptos);
    BOOST_REQUIRE_EQUAL(expired.size(), 2u);
    BOOST_TEST(expired[0] == rtx.Key(9, 2));
    BOOST_TEST(expired[1] == rtx.Key(9, 3));
    rtx.MarkRetransmitted(expired[0], 1300, 1);
    expired.clear();
    rtx.CollectExpired(3099, 200, 60000, 32, expired, 1, &ptos);
    BOOST_REQUIRE_EQUAL(expired.size(), 1u);
    BOOST_TEST(expired[0] == rtx.Key(9, 1));
    expired.clear();
    rtx.CollectExpired(3100, 200, 60000, 32, expired, 1, &ptos);
    BOOST_REQUIRE_EQUAL(expired.size(), 2u);
    BOOST_TEST(expired[1] == rtx.Key(9, 2));
}

BOOST_AUTO_TEST_CASE(collect_expired_respects_pto_and_count_bound) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_TEST(rtx.Track(0, 1, make_frame(1), 10, 1000, 1 << 20));
    BOOST_TEST(rtx.Track(0, 2, make_frame(2), 10, 1500, 1 << 20));
    BOOST_TEST(rtx.Track(0, 3, make_frame(3), 10, 1900, 1 << 20));

    std::vector<std::uint64_t> expired;
    rtx.CollectExpired(2000, 400, 60000, 32, expired); // entries idle >= 400ms: seq 1 and 2
    BOOST_REQUIRE_EQUAL(expired.size(), 2u);

    std::vector<std::uint64_t> bounded;
    rtx.CollectExpired(2000, 400, 60000, 1, bounded);
    BOOST_REQUIRE_EQUAL(bounded.size(), 1u);

    std::vector<std::uint64_t> none;
    rtx.CollectExpired(1100, 400, 60000, 32, none);
    BOOST_TEST(none.empty());
}

BOOST_AUTO_TEST_CASE(erase_cid_drops_only_that_space) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_TEST(rtx.Track(1, 1, make_frame(1), 10, 1000, 1 << 20));
    BOOST_TEST(rtx.Track(2, 1, make_frame(2), 20, 1000, 1 << 20));

    rtx.EraseCid(1);
    BOOST_TEST(rtx.size() == 1u);
    BOOST_TEST(rtx.bytes() == 20u);
    BOOST_TEST(rtx.Find(mux::MuxRetransmitBuffer::Key(2, 1)) != nullptr);

    rtx.Clear();
    BOOST_TEST(rtx.size() == 0u);
    BOOST_TEST(rtx.bytes() == 0u);
}

BOOST_AUTO_TEST_CASE(duplicate_track_is_a_noop) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_TEST(rtx.Track(0, 5, make_frame(5), 10, 1000, 1 << 20));
    BOOST_TEST(rtx.Track(0, 5, make_frame(5), 10, 2000, 1 << 20));
    BOOST_TEST(rtx.size() == 1u);
    BOOST_TEST(rtx.bytes() == 10u);
}

BOOST_AUTO_TEST_CASE(take_link_inflight_returns_only_that_carriers_unacked_frames) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_TEST(rtx.Track(0, 1, make_frame(1), 10, 1000, 1 << 20, /*link_id=*/3));
    BOOST_TEST(rtx.Track(0, 2, make_frame(2), 10, 1001, 1 << 20, /*link_id=*/4));
    BOOST_TEST(rtx.Track(0, 3, make_frame(3), 10, 1002, 1 << 20, /*link_id=*/3));
    BOOST_TEST(rtx.Track(0, 4, make_frame(4), 10, 1003, 1 << 20, /*link_id=*/3));

    // seq 3 was acked; seq 4 was already re-sent on carrier 4.
    std::vector<std::uint64_t> fast;
    std::vector<mux::MuxAckRange> ranges = { {3, 3} };
    rtx.Ack(0, 3, ranges, 1100, 1000, 0, fast);
    rtx.MarkRetransmitted(mux::MuxRetransmitBuffer::Key(0, 4), 1101, 4);

    std::vector<std::uint64_t> lost;
    rtx.TakeLinkInflight(3, lost);
    BOOST_REQUIRE_EQUAL(lost.size(), 1u);
    BOOST_TEST(lost[0] == mux::MuxRetransmitBuffer::Key(0, 1));

    // Orphaned: the dead carrier's id no longer owns it, so a carrier that later
    // reuses id 3 cannot re-claim it, and a re-send is not suppressed by the
    // "already sent this tick" guard.
    const mux::MuxRtxEntry* entry = rtx.Find(lost[0]);
    BOOST_REQUIRE(entry != nullptr);
    BOOST_TEST(entry->last_link_id == 0u);
    BOOST_TEST(entry->last_sent_tick == 0u);
    std::vector<std::uint64_t> again;
    rtx.TakeLinkInflight(3, again);
    BOOST_TEST(again.empty());
}

BOOST_AUTO_TEST_CASE(take_link_inflight_preserves_send_order_and_ignores_unknown_link) {
    mux::MuxRetransmitBuffer rtx;
    for (std::uint32_t seq = 10; seq < 15; ++seq) {
        BOOST_TEST(rtx.Track(7, seq, make_frame((int)seq), 10, 1000 + seq, 1 << 20, /*link_id=*/2));
    }

    std::vector<std::uint64_t> none;
    rtx.TakeLinkInflight(0, none);
    BOOST_TEST(none.empty());

    std::vector<std::uint64_t> lost;
    rtx.TakeLinkInflight(2, lost);
    BOOST_REQUIRE_EQUAL(lost.size(), 5u);
    for (std::size_t i = 0; i < lost.size(); ++i) {
        BOOST_TEST(lost[i] == mux::MuxRetransmitBuffer::Key(7, (std::uint32_t)(10 + i)));
    }
}

BOOST_AUTO_TEST_CASE(cid_bytes_follow_track_ack_and_erase) {
    // The per-flow send window reads CidBytes(): it must shrink on ACK, or a
    // long-lived flow_v2 flow would hit its 2 MiB budget after 2 MiB total
    // sent and be failed even though nothing is outstanding.
    mux::MuxRetransmitBuffer rtx;
    BOOST_TEST(rtx.Track(7, 1, make_frame(1), 100, 1000, 1 << 20));
    BOOST_TEST(rtx.Track(7, 2, make_frame(2), 100, 1000, 1 << 20));
    BOOST_TEST(rtx.Track(9, 1, make_frame(3), 50, 1000, 1 << 20));
    BOOST_TEST(rtx.CidBytes(7) == 200u);
    BOOST_TEST(rtx.CidBytes(9) == 50u);
    BOOST_TEST(rtx.CidBytes(42) == 0u);

    std::vector<std::uint64_t> fast;
    std::vector<mux::MuxAckRange> ranges = { {1, 2} };
    rtx.Ack(7, 2, ranges, 1100, 3, 0, fast);
    BOOST_TEST(rtx.CidBytes(7) == 0u);
    BOOST_TEST(rtx.CidBytes(9) == 50u);
    BOOST_TEST(rtx.bytes() == 50u);

    BOOST_TEST(rtx.Track(7, 3, make_frame(4), 100, 1200, 1 << 20));
    rtx.EraseCid(9);
    BOOST_TEST(rtx.CidBytes(9) == 0u);
    BOOST_TEST(rtx.CidBytes(7) == 100u);

    rtx.Clear();
    BOOST_TEST(rtx.CidBytes(7) == 0u);
}

BOOST_AUTO_TEST_CASE(link_bytes_follow_ack_and_report_new_bytes_by_original_link) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_TEST(rtx.Track(1, 1, make_frame(1), 100, 1000, 1 << 20, 2));
    BOOST_TEST(rtx.Track(1, 2, make_frame(2), 200, 1010, 1 << 20, 3));
    BOOST_TEST(rtx.Track(1, 3, make_frame(3), 50, 1020, 1 << 20, 2));
    BOOST_TEST(rtx.LinkBytes(2) == 150u);
    BOOST_TEST(rtx.LinkBytes(3) == 200u);
    BOOST_TEST(rtx.LinkBytes(0) == 0u);

    std::vector<std::uint64_t> fast;
    std::vector<mux::MuxAckRange> ranges = { {1, 2} };
    std::unordered_map<std::uint16_t, std::size_t> newly_acked = { {99, 1} };
    rtx.Ack(1, 2, ranges, 1100, 1000, 0, fast, nullptr, &newly_acked);
    BOOST_REQUIRE_EQUAL(newly_acked.size(), 2u);
    BOOST_TEST(newly_acked[2] == 100u);
    BOOST_TEST(newly_acked[3] == 200u);
    BOOST_TEST(rtx.LinkBytes(2) == 50u);
    BOOST_TEST(rtx.LinkBytes(3) == 0u);

    newly_acked[99] = 9;
    rtx.Ack(1, 2, ranges, 1200, 1000, 0, fast, nullptr, &newly_acked);
    BOOST_TEST(newly_acked.empty()); // repeated ACK produces no new bytes
    BOOST_TEST(rtx.LinkBytes(2) == 50u);
}

BOOST_AUTO_TEST_CASE(link_bytes_and_ack_credit_follow_the_final_transmission) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_TEST(rtx.Track(4, 1, make_frame(1), 120, 1000, 1 << 20, 2));
    BOOST_TEST(rtx.Track(4, 2, make_frame(2), 80, 1010, 1 << 20, 2));
    rtx.MarkRetransmitted(mux::MuxRetransmitBuffer::Key(4, 1), 1100, 3);
    BOOST_TEST(rtx.LinkBytes(2) == 80u);
    BOOST_TEST(rtx.LinkBytes(3) == 120u);

    std::vector<std::uint64_t> fast;
    std::vector<mux::MuxAckRange> ranges = { {1, 2} };
    std::unordered_map<std::uint16_t, std::size_t> newly_acked;
    rtx.Ack(4, 2, ranges, 1200, 1000, 0, fast, nullptr, &newly_acked);
    BOOST_REQUIRE_EQUAL(newly_acked.size(), 2u);
    BOOST_TEST(newly_acked[2] == 80u);
    BOOST_TEST(newly_acked[3] == 120u);
    BOOST_TEST(rtx.LinkBytes(2) == 0u);
    BOOST_TEST(rtx.LinkBytes(3) == 0u);
}

BOOST_AUTO_TEST_CASE(cumulative_payload_counts_retransmitted_and_orphaned_frames_once) {
    mux::MuxRetransmitBuffer rtx;
    BOOST_REQUIRE(rtx.Track(4, 1, make_frame(1), 100, 1000, 1024, 2, {}, 80));
    BOOST_REQUIRE(rtx.Track(4, 2, make_frame(2), 60, 1010, 1024, 2, {}, 40));
    rtx.MarkRetransmitted(mux::MuxRetransmitBuffer::Key(4, 1), 1100, 3);
    std::vector<std::uint64_t> orphaned;
    rtx.TakeLinkInflight(2, orphaned);
    BOOST_REQUIRE_EQUAL(orphaned.size(), 1u);

    std::uint64_t total = UINT64_MAX - 110;
    std::vector<std::uint64_t> fast;
    rtx.Ack(4, 2, {{2, 2}}, 1200, 3, 0, fast, nullptr, nullptr, nullptr, &total);
    BOOST_TEST(total == UINT64_MAX - 70);
    rtx.Ack(4, 2, {{2, 2}}, 1210, 3, 0, fast, nullptr, nullptr, nullptr, &total);
    BOOST_TEST(total == UINT64_MAX - 70);
    rtx.Ack(4, 2, {{1, 2}}, 1220, 3, 0, fast, nullptr, nullptr, nullptr, &total);
    BOOST_TEST(total == UINT64_MAX);
}

BOOST_AUTO_TEST_CASE(link_exit_and_flow_clear_keep_reused_id_accounting_exact) {
    mux::MuxRetransmitBuffer rtx;
    const auto old_key = mux::MuxRetransmitBuffer::Key(10, 1);
    const auto new_key = mux::MuxRetransmitBuffer::Key(11, 1);
    BOOST_TEST(rtx.Track(10, 1, make_frame(1), 10, 1000, 1 << 20, 5));
    std::vector<std::uint64_t> lost;
    rtx.TakeLinkInflight(5, lost);
    BOOST_REQUIRE_EQUAL(lost.size(), 1u);
    BOOST_TEST(lost[0] == old_key);
    BOOST_TEST(rtx.LinkBytes(5) == 0u);

    BOOST_TEST(rtx.Track(11, 1, make_frame(2), 20, 1100, 1 << 20, 5));
    BOOST_TEST(rtx.LinkBytes(5) == 20u);
    std::vector<std::uint64_t> reused;
    rtx.TakeLinkInflight(5, reused);
    BOOST_REQUIRE_EQUAL(reused.size(), 1u);
    BOOST_TEST(reused[0] == new_key); // old orphan cannot be claimed by reused id
    BOOST_TEST(rtx.LinkBytes(5) == 0u);

    BOOST_TEST(rtx.Track(10, 2, make_frame(3), 30, 1200, 1 << 20, 5));
    BOOST_TEST(rtx.Track(11, 2, make_frame(4), 40, 1201, 1 << 20, 5));
    rtx.EraseCid(10);
    BOOST_TEST(rtx.LinkBytes(5) == 40u);
    rtx.EraseCid(11);
    BOOST_TEST(rtx.LinkBytes(5) == 0u);
    BOOST_TEST(rtx.LinkBytes(2) == 0u);
    BOOST_TEST(rtx.LinkBytes(3) == 0u);

    BOOST_TEST(rtx.Track(12, 1, make_frame(5), 50, 1300, 1 << 20, 5));
    rtx.Clear();
    BOOST_TEST(rtx.LinkBytes(5) == 0u);
}
