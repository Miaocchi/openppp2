#define BOOST_TEST_MODULE p2p_replay_window_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/p2p/P2PReplayWindow.h>

using ppp::p2p::P2PReplayWindow;
using ppp::p2p::REPLAY_WINDOW_SIZE;

BOOST_AUTO_TEST_CASE(replay_empty_window_accepts_first) {
    P2PReplayWindow window;
    BOOST_TEST(!window.IsDuplicate(42));
    BOOST_TEST(window.Accept(42));
    BOOST_TEST(window.IsDuplicate(42));
}

BOOST_AUTO_TEST_CASE(replay_rejects_duplicate_inside_window) {
    P2PReplayWindow window;
    BOOST_REQUIRE(window.Accept(100));
    BOOST_TEST(!window.Accept(100));
    BOOST_TEST(window.IsDuplicate(100));
}

BOOST_AUTO_TEST_CASE(replay_rejects_seq_older_than_window) {
    P2PReplayWindow window;
    BOOST_REQUIRE(window.Accept(5000));
    const uint32_t stale = 5000 - static_cast<uint32_t>(REPLAY_WINDOW_SIZE);
    BOOST_TEST(window.IsDuplicate(stale));
    BOOST_TEST(!window.Accept(stale));
}

BOOST_AUTO_TEST_CASE(replay_advances_base_on_newer_seq) {
    P2PReplayWindow window;
    BOOST_REQUIRE(window.Accept(10));
    BOOST_REQUIRE(window.Accept(20));
    BOOST_TEST(window.IsDuplicate(20));
    BOOST_TEST(!window.IsDuplicate(21));
    BOOST_TEST(window.IsDuplicate(10));
}

BOOST_AUTO_TEST_CASE(replay_does_not_accept_stale_seq) {
    P2PReplayWindow window;
    BOOST_REQUIRE(window.Accept(5000));
    const uint32_t stale = 5000 - static_cast<uint32_t>(REPLAY_WINDOW_SIZE);
    BOOST_TEST(!window.Accept(stale));
}
