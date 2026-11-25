#define BOOST_TEST_MODULE vmux_intent_classifier_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/mux/TcpIntentClassifier.h>
#include <ppp/app/mux/MuxIntentPathProbe.h>

namespace mux = ppp::app::mux;

BOOST_AUTO_TEST_CASE(disabled_classifier_stays_unknown) {
    mux::TcpIntentClassifier classifier;
    BOOST_TEST(static_cast<int>(classifier.Observe(1, 1024, 1024, true).intent) ==
        static_cast<int>(mux::TcpIntent::Unknown));
    BOOST_TEST(!classifier.Seed("example.com", 443, {{mux::TcpIntent::Latency, 443, 443, ""}}));
}

BOOST_AUTO_TEST_CASE(rule_seed_has_priority_and_supports_host_suffix) {
    mux::TcpIntentConfig config;
    config.enabled = true;
    config.mode = "auto";
    mux::TcpIntentClassifier classifier(config);
    const std::vector<mux::TcpIntentRule> rules = {
        {mux::TcpIntent::Latency, 443, 443, ".interactive.example"},
        {mux::TcpIntent::Bandwidth, 10000, 10000, ""},
    };
    BOOST_TEST(classifier.Seed("api.interactive.example", 443, rules));
    BOOST_TEST(static_cast<int>(classifier.Intent()) == static_cast<int>(mux::TcpIntent::Latency));
    BOOST_TEST(static_cast<int>(classifier.Result().source) == static_cast<int>(mux::TcpIntentSource::Rule));
    BOOST_TEST(classifier.Result().confidence == 1.0);
}

BOOST_AUTO_TEST_CASE(short_stream_remains_unknown) {
    mux::TcpIntentConfig config;
    config.enabled = true;
    mux::TcpIntentClassifier classifier(config);
    classifier.Observe(1, 4096, 4096, true);
    const auto result = classifier.Observe(1000, 4096, 4096, false);
    BOOST_TEST(static_cast<int>(result.intent) == static_cast<int>(mux::TcpIntent::Unknown));
}

BOOST_AUTO_TEST_CASE(intermittent_medium_chunks_remain_unknown) {
    mux::TcpIntentConfig config;
    config.enabled = true;
    mux::TcpIntentClassifier classifier(config);
    for (std::uint64_t tick = 1; tick <= 3001; tick += 300) {
        for (bool outbound : {true, false}) {
            const auto result = classifier.Observe(tick, 8192, 8192, outbound);
            BOOST_TEST(static_cast<int>(result.intent) == static_cast<int>(mux::TcpIntent::Unknown));
            BOOST_TEST(!result.changed);
        }
    }
}

BOOST_AUTO_TEST_CASE(sustained_large_chunks_classify_as_bandwidth) {
    mux::TcpIntentConfig config;
    config.enabled = true;
    mux::TcpIntentClassifier classifier(config);
    for (std::uint64_t tick = 1; tick <= 1001; tick += 100) {
        classifier.Observe(tick, 128 * 1024, 128 * 1024, true);
    }
    BOOST_TEST(static_cast<int>(classifier.Intent()) == static_cast<int>(mux::TcpIntent::Bandwidth));
    BOOST_TEST(static_cast<int>(classifier.Result().source) == static_cast<int>(mux::TcpIntentSource::Behavior));
}

BOOST_AUTO_TEST_CASE(first_full_size_chunk_classifies_as_bandwidth) {
    mux::TcpIntentConfig config;
    config.enabled = true;
    mux::TcpIntentClassifier classifier(config);
    const auto result = classifier.Observe(10, 32 * 1024, 32 * 1024, false);
    BOOST_TEST(static_cast<int>(result.intent) == static_cast<int>(mux::TcpIntent::Bandwidth));
    BOOST_TEST(static_cast<int>(result.source) == static_cast<int>(mux::TcpIntentSource::Behavior));
}

BOOST_AUTO_TEST_CASE(small_bidirectional_bursts_classify_as_latency) {
    mux::TcpIntentConfig config;
    config.enabled = true;
    mux::TcpIntentClassifier classifier(config);
    for (std::uint64_t tick = 1; tick <= 1001; tick += 100) {
        classifier.Observe(tick, 2048, 2048, (tick / 100) % 2 == 0);
    }
    BOOST_TEST(static_cast<int>(classifier.Intent()) == static_cast<int>(mux::TcpIntent::Latency));
    BOOST_TEST(static_cast<int>(classifier.Result().source) == static_cast<int>(mux::TcpIntentSource::Behavior));
}

BOOST_AUTO_TEST_CASE(classification_is_sticky_after_first_decision) {
    mux::TcpIntentConfig config;
    config.enabled = true;
    mux::TcpIntentClassifier classifier(config);
    for (std::uint64_t tick = 1; tick <= 1001; tick += 100) {
        classifier.Observe(tick, 128 * 1024, 128 * 1024, true);
    }
    BOOST_TEST(static_cast<int>(classifier.Intent()) == static_cast<int>(mux::TcpIntent::Bandwidth));
    const auto before = classifier.Result().sample_bytes;
    classifier.Observe(2001, 512, 512, false);
    BOOST_TEST(static_cast<int>(classifier.Intent()) == static_cast<int>(mux::TcpIntent::Bandwidth));
    BOOST_TEST(classifier.Result().sample_bytes == before);
    BOOST_TEST(!classifier.Seed("example.com", 443, {{mux::TcpIntent::Latency, 443, 443, ""}}));
    BOOST_TEST(static_cast<int>(classifier.Intent()) == static_cast<int>(mux::TcpIntent::Bandwidth));
}
BOOST_AUTO_TEST_CASE(intent_path_probe_times_out_only_after_a_confirmed_reply) {
    ppp::app::mux::MuxIntentPathProbe probe;
    BOOST_TEST(probe.Due(100));
    probe.Sent(100);
    BOOST_TEST(!probe.TimedOut(1000));
    BOOST_TEST(!probe.Acknowledge(99, 150));
    BOOST_TEST(probe.Acknowledge(100, 150));
    BOOST_TEST(probe.RttMs() == 50u);
    BOOST_TEST(!probe.TimedOut(1200));
    BOOST_TEST(probe.Due(1200));
    probe.Sent(1200);
    BOOST_TEST(!probe.TimedOut(1699));
    BOOST_TEST(probe.TimedOut(1700));
    BOOST_TEST(probe.ConsumeTimeout(1700));
    BOOST_TEST(!probe.ConsumeTimeout(1701));
    probe.Sent(2200);
    BOOST_TEST(probe.TimedOut(2200));
    BOOST_TEST(probe.Acknowledge(2200, 2280));
    BOOST_TEST(probe.RttMs() == 80u);
    BOOST_TEST(!probe.TimedOut(2280));
    probe.Sent(3200);
    BOOST_TEST(probe.ConsumeTimeout(3700));
}

BOOST_AUTO_TEST_CASE(intent_probe_rtt_ages_out_transient_latency) {
    ppp::app::mux::MuxIntentPathProbe probe;
    probe.Sent(1000);
    BOOST_TEST(probe.Acknowledge(1000, 1050));
    probe.Sent(2000);
    BOOST_TEST(probe.Acknowledge(2000, 2280));
    BOOST_TEST(probe.RecentRttMs(2280) == 50u);
    probe.Sent(2400);
    BOOST_TEST(!probe.TimedOut(2950));
    BOOST_TEST(probe.TimedOut(3240));
    BOOST_TEST(probe.Acknowledge(2400, 3240));
    probe.Sent(3400);
    BOOST_TEST(probe.Acknowledge(3400, 3540));
    BOOST_TEST(probe.RecentRttMs(3540) == 140u);
    BOOST_TEST(probe.MinRttMs() == 50u);
}

BOOST_AUTO_TEST_CASE(intent_probe_accepts_a_delayed_reply_and_checks_payload_progress) {
    mux::MuxIntentPathProbe probe;
    probe.Sent(1000);
    BOOST_TEST(probe.Acknowledge(1000, 1050));
    BOOST_TEST(!probe.Due(1499));
    BOOST_TEST(probe.Due(1500));
    probe.Sent(1500);
    probe.Sent(2000);
    BOOST_TEST(!probe.ConsumeTimeout(2000, 1800));
    BOOST_TEST(probe.Acknowledge(1500, 2050));
    BOOST_TEST(!probe.Acknowledge(1500, 2060));
    BOOST_TEST(probe.RttMs() == 550u);
    BOOST_TEST(!probe.TimedOut(3699));
    BOOST_TEST(probe.ConsumeTimeout(3700));
    BOOST_TEST(probe.Acknowledge(2000, 3750));
    BOOST_TEST(!probe.TimedOut(3750));
}

BOOST_AUTO_TEST_CASE(intent_probe_replaces_only_sustained_stalls_after_recovery_was_attempted) {
    mux::MuxIntentPathProbe probe;
    probe.Sent(1000);
    BOOST_TEST(probe.Acknowledge(1000, 1050));
    probe.Sent(1500);
    BOOST_TEST(!probe.ShouldReplace(2700, 0, true));
    BOOST_TEST(probe.ConsumeTimeout(2000));
    BOOST_TEST(!probe.ShouldReplace(2699, 0, true));
    BOOST_TEST(probe.ShouldReplace(2700, 0, true));
    BOOST_TEST(!probe.ShouldReplace(2700, 2400, true));
    BOOST_TEST(!probe.ShouldReplace(4499, 0));
    BOOST_TEST(probe.ShouldReplace(4500, 0));
    BOOST_TEST(!probe.ShouldReplace(4500, 4000));
    probe.Sent(4500);
    BOOST_TEST(probe.Acknowledge(4500, 4550));
    BOOST_TEST(!probe.ShouldReplace(4550, 0));
}
