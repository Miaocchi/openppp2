#define BOOST_TEST_MODULE vmux_path_estimator_test
#include <boost/test/included/unit_test.hpp>
#include <ppp/app/multipath/VmuxPathEstimator.h>

using namespace ppp::app::multipath;

BOOST_AUTO_TEST_CASE(estimates_bdp_queue_and_capacity) {
    VmuxPathEstimator e;
    VmuxPathSample s;
    s.epoch = 1; s.timestamp_ms = 100;
    s.delivery_rate_bytes_per_second = 100000;
    s.srtt_ms = 50; s.min_rtt_ms = 20; s.wire_inflight_bytes = 5000;
    auto t = e.Observe(s);
    BOOST_TEST(t.has_sample); BOOST_TEST(t.bdp_bytes == 5000u);
    BOOST_TEST(t.queue_delay_ms == 30u); BOOST_TEST(t.at_capacity);
}

BOOST_AUTO_TEST_CASE(epoch_resets_and_unknown_is_not_zero_sample) {
    VmuxPathEstimator e;
    VmuxPathSample s; s.epoch = 1; s.timestamp_ms = 1; s.delivery_rate_bytes_per_second = 42;
    BOOST_TEST(e.Observe(s).has_sample);
    s = {}; s.epoch = 2; s.timestamp_ms = 2;
    auto t = e.Observe(s);
    BOOST_TEST(!t.has_sample); BOOST_TEST(t.current_rate_bytes_per_second == 0u);
}

BOOST_AUTO_TEST_CASE(max_filter_and_expiry) {
    VmuxPathEstimator e({2, 10});
    VmuxPathSample s; s.epoch = 1; s.timestamp_ms = 1; s.delivery_rate_bytes_per_second = 100;
    e.Observe(s); s.timestamp_ms = 2; s.delivery_rate_bytes_per_second = 50; BOOST_TEST(e.Observe(s).current_rate_bytes_per_second == 100u);
    s.timestamp_ms = 20; s.delivery_rate_bytes_per_second.reset(); auto t = e.Observe(s); BOOST_TEST(!t.has_sample);
}

BOOST_AUTO_TEST_CASE(late_sample_does_not_erase_current_metrics) {
    VmuxPathEstimator e;
    VmuxPathSample current;
    current.epoch = 1; current.timestamp_ms = 10;
    current.delivery_rate_bytes_per_second = 1000;
    BOOST_REQUIRE(e.Observe(current).has_sample);

    VmuxPathSample late;
    late.epoch = 1; late.timestamp_ms = 9;
    late.delivery_rate_bytes_per_second = 1;
    const auto telemetry = e.Observe(late);
    BOOST_TEST(telemetry.has_sample);
    BOOST_TEST(telemetry.current_rate_bytes_per_second == 1000u);
}
