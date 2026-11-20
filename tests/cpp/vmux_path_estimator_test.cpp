#define BOOST_TEST_MODULE vmux_path_estimator_test
#include <boost/test/included/unit_test.hpp>
#include <ppp/app/multipath/VmuxPathEstimator.h>

using namespace ppp::app::multipath;

BOOST_AUTO_TEST_CASE(ewma_filters_low_application_limited_samples) {
    auto estimator = MakePathEstimator(PathEstimatorMode::Ewma);
    estimator->OnSample({1000, 10000, 0, 1000, false}, 10);
    estimator->OnSample({100, 10000, 1000, 1100, true}, 20);
    const auto limited = estimator->GetTelemetry(20);
    BOOST_TEST(limited.current_rate_bytes_per_second == 10000u);
    BOOST_TEST(limited.capacity_rate_bytes_per_second == 100000u);
    estimator->OnSample({200, 10000, 1100, 1300, false}, 30);
    BOOST_TEST(estimator->GetTelemetry(30).capacity_rate_bytes_per_second == 90000u);
}

BOOST_AUTO_TEST_CASE(bbr_max_expires_by_delivery_round_not_ack_count) {
    auto estimator = MakePathEstimator(PathEstimatorMode::Bbr);
    estimator->OnSample({1000, 10000, 0, 1000, false}, 10);
    for (std::uint64_t i = 1; i <= 15; ++i) {
        estimator->OnSample({100, 10000, 0, 1000 + i * 100, false}, 10 + i);
    }
    BOOST_TEST(estimator->GetTelemetry(25).capacity_rate_bytes_per_second == 100000u);
    for (std::uint64_t i = 1; i <= 10; ++i) {
        const auto total = 2500 + i * 100;
        estimator->OnSample({100, 10000, total - 100, total, false}, 25 + i);
        BOOST_TEST(estimator->GetTelemetry(25 + i).capacity_rate_bytes_per_second ==
            (i < 10 ? 100000u : 10000u));
    }
}

BOOST_AUTO_TEST_CASE(kcc_detects_full_bandwidth_only_on_non_limited_rounds) {
    auto estimator = MakePathEstimator(PathEstimatorMode::Kcc);
    for (std::uint64_t i = 0; i < 4; ++i) {
        estimator->OnSample({1000, 10000, i * 1000, (i + 1) * 1000, true}, 10 + i);
    }
    BOOST_TEST(!estimator->FullBandwidthReached());
    for (std::uint64_t i = 4; i < 8; ++i) {
        estimator->OnSample({1000, 10000, i * 1000, (i + 1) * 1000, false}, 10 + i);
        BOOST_TEST(estimator->FullBandwidthReached() == (i == 7));
    }
    estimator->Reset(2);
    BOOST_TEST(!estimator->FullBandwidthReached());
    BOOST_TEST(!estimator->GetTelemetry(17).has_sample);
}

BOOST_AUTO_TEST_CASE(invalid_duplicate_and_stale_samples_do_not_refresh_metrics) {
    auto estimator = MakePathEstimator(PathEstimatorMode::Bbr);
    estimator->OnSample({1000, 10000, 0, 1000, false}, 100);
    estimator->OnSample({1000, 0, 1000, 2000, false}, 101);
    estimator->OnSample({1000, 10000, 0, 1000, false}, 102);
    estimator->OnSample({1000, 10000, 1000, 2000, false}, 99);
    BOOST_TEST(estimator->GetTelemetry(103).sample_count == 1u);
    BOOST_TEST(!estimator->GetTelemetry(5101).has_sample);
    estimator->OnSample({100, 10000, 1000, 1100, false}, 5101);
    BOOST_TEST(estimator->GetTelemetry(5101).capacity_rate_bytes_per_second == 10000u);
}

BOOST_AUTO_TEST_CASE(low_application_limited_samples_do_not_reseed_expired_capacity) {
    for (const auto mode : {PathEstimatorMode::Ewma, PathEstimatorMode::Bbr, PathEstimatorMode::Kcc}) {
        auto estimator = MakePathEstimator(mode);
        estimator->OnSample({1000, 10000, 0, 1000, false}, 100);
        estimator->OnSample({100, 10000, 1000, 1100, true}, 5101);
        const auto limited = estimator->GetTelemetry(5101);
        BOOST_TEST(limited.current_rate_bytes_per_second == 10000u);
        BOOST_TEST(limited.capacity_rate_bytes_per_second == 0u);
        estimator->OnSample({200, 10000, 1100, 1300, false}, 5102);
        BOOST_TEST(estimator->GetTelemetry(5102).capacity_rate_bytes_per_second == 20000u);
    }
}

BOOST_AUTO_TEST_CASE(filtered_application_limited_samples_advance_delivery_rounds) {
    for (const auto mode : {PathEstimatorMode::Bbr, PathEstimatorMode::Kcc}) {
        auto estimator = MakePathEstimator(mode);
        estimator->OnSample({1000, 10000, 0, 1000, false}, 10);
        for (std::uint64_t i = 0; i < 9; ++i) {
            estimator->OnSample({100, 10000, 1000 + i * 100, 1100 + i * 100, true}, 11 + i);
        }
        BOOST_TEST(estimator->GetTelemetry(19).capacity_rate_bytes_per_second == 100000u);
        estimator->OnSample({200, 10000, 1900, 2100, false}, 20);
        BOOST_TEST(estimator->GetTelemetry(20).capacity_rate_bytes_per_second == 20000u);
    }
}
