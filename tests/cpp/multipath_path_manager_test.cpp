#define BOOST_TEST_MODULE multipath_path_manager_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/multipath/PathManager.h>

#include <chrono>
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mp = ppp::app::multipath;
namespace cfg = ppp::configurations;

namespace {
cfg::MultipathConfiguration MakeConfig(int paths, int connections_per_path,
    int max_connections, int max_concurrent) {
    cfg::MultipathConfiguration config;
    config.enabled = true;
    config.max_connections = max_connections;
    config.max_concurrent_connects = max_concurrent;
    for (int i = 0; i < paths; ++i) {
        cfg::MultipathConfiguration::Path path;
        path.id = "path-" + std::to_string(i);
        path.transport = "tcp";
        path.connect_host = "path.example.test";
        path.port = 443;
        path.connections = connections_per_path;
        config.paths.emplace_back(std::move(path));
    }
    return config;
}
}

BOOST_AUTO_TEST_CASE(attempts_are_bounded_by_concurrency_and_connection_budget) {
    mp::PathManager manager;
    BOOST_REQUIRE(manager.Start(MakeConfig(8, 4, 32, 4)));
    BOOST_TEST(manager.SlotCount() == 32u);

    mp::PathManager::Attempt attempts[4];
    for (auto& attempt : attempts) BOOST_REQUIRE(manager.BeginAttempt(0, attempt));
    mp::PathManager::Attempt overflow;
    BOOST_TEST(!manager.BeginAttempt(0, overflow));
    BOOST_TEST(manager.ActiveAttempts() == 4u);

    for (const auto& attempt : attempts) BOOST_REQUIRE(manager.MarkJoined(attempt));
    BOOST_TEST(manager.ActiveAttempts() == 0u);
    BOOST_TEST(manager.JoinedCarriers() == 4u);
}

BOOST_AUTO_TEST_CASE(partial_path_failure_does_not_withdraw_joined_carrier) {
    mp::PathManager manager;
    BOOST_REQUIRE(manager.Start(MakeConfig(2, 1, 2, 2)));
    mp::PathManager::Attempt first;
    mp::PathManager::Attempt second;
    BOOST_REQUIRE(manager.BeginAttempt(10, first));
    BOOST_REQUIRE(manager.BeginAttempt(10, second));
    BOOST_REQUIRE(manager.MarkJoined(first));
    BOOST_REQUIRE(manager.Fail(second, 10, mp::PathManager::Failure::Network));
    BOOST_TEST(manager.JoinedCarriers() == 1u);
    BOOST_TEST(manager.ActiveAttempts() == 0u);
    mp::PathManager::SlotSnapshot second_state;
    BOOST_REQUIRE(manager.GetSlot(second.slot, second_state));
    BOOST_CHECK(second_state.state == mp::PathManager::State::Backoff);
}

BOOST_AUTO_TEST_CASE(retry_backoff_is_jittered_bounded_and_stale_attempts_cannot_complete_retry) {
    mp::PathManager manager;
    BOOST_REQUIRE(manager.Start(MakeConfig(1, 1, 1, 1)));
    mp::PathManager::Attempt stale;
    BOOST_REQUIRE(manager.BeginAttempt(1000, stale));
    BOOST_REQUIRE(manager.Fail(stale, 1000, mp::PathManager::Failure::Network));
    mp::PathManager::SlotSnapshot first_state;
    BOOST_REQUIRE(manager.GetSlot(stale.slot, first_state));
    BOOST_TEST(first_state.retry_at_ms >= 2000u);
    BOOST_TEST(first_state.retry_at_ms <= 2200u);
    mp::PathManager::Attempt too_early;
    BOOST_TEST(!manager.BeginAttempt(first_state.retry_at_ms - 1, too_early));

    mp::PathManager::Attempt retry;
    BOOST_REQUIRE(manager.BeginAttempt(first_state.retry_at_ms, retry));
    BOOST_TEST(!manager.MarkJoined(stale));
    BOOST_TEST(!manager.Fail(stale, first_state.retry_at_ms, mp::PathManager::Failure::Network));
    BOOST_TEST(manager.IsCurrent(retry));
    BOOST_REQUIRE(manager.MarkAuthenticating(retry));
    BOOST_REQUIRE(manager.MarkJoined(retry));
    BOOST_TEST(manager.JoinedCarriers() == 1u);
}

BOOST_AUTO_TEST_CASE(repeated_retry_backoff_caps_at_thirty_seconds) {
    mp::PathManager manager;
    BOOST_REQUIRE(manager.Start(MakeConfig(1, 1, 1, 1)));
    std::uint64_t now = 0;
    for (int i = 0; i < 9; ++i) {
        mp::PathManager::Attempt attempt;
        BOOST_REQUIRE(manager.BeginAttempt(now, attempt));
        BOOST_REQUIRE(manager.Fail(attempt, now, mp::PathManager::Failure::Network));
        mp::PathManager::SlotSnapshot slot;
        BOOST_REQUIRE(manager.GetSlot(0, slot));
        const std::uint64_t delay = slot.retry_at_ms - now;
        BOOST_TEST(delay >= 1000u);
        BOOST_TEST(delay <= 30000u);
        now = slot.retry_at_ms;
    }
}

BOOST_AUTO_TEST_CASE(authentication_failure_is_reported_and_rate_limited) {
    mp::PathManager manager;
    BOOST_REQUIRE(manager.Start(MakeConfig(1, 1, 1, 1)));
    mp::PathManager::Attempt attempt;
    BOOST_REQUIRE(manager.BeginAttempt(50, attempt));
    BOOST_REQUIRE(manager.Fail(attempt, 50, mp::PathManager::Failure::Authentication));
    mp::PathManager::SlotSnapshot slot;
    BOOST_REQUIRE(manager.GetSlot(0, slot));
    BOOST_CHECK(slot.failure == mp::PathManager::Failure::Authentication);
    BOOST_TEST(slot.retry_at_ms - 50 == 30000u);
}

BOOST_AUTO_TEST_CASE(stop_invalidates_tokens_and_resets_attempt_accounting) {
    mp::PathManager manager;
    BOOST_REQUIRE(manager.Start(MakeConfig(1, 2, 2, 2)));
    mp::PathManager::Attempt attempt;
    BOOST_REQUIRE(manager.BeginAttempt(0, attempt));
    manager.Stop();
    BOOST_TEST(manager.ActiveAttempts() == 0u);
    BOOST_TEST(manager.JoinedCarriers() == 0u);
    BOOST_TEST(!manager.IsCurrent(attempt));
    BOOST_TEST(!manager.Fail(attempt, 1, mp::PathManager::Failure::Network));
    BOOST_TEST(!manager.BeginAttempt(1, attempt));
}

BOOST_AUTO_TEST_CASE(stop_transitions_path_control_to_stopped) {
    mp::PathManager manager;
    BOOST_REQUIRE(manager.Start(MakeConfig(1, 1, 1, 1)));
    mp::PathManager::Attempt attempt;
    BOOST_REQUIRE(manager.BeginAttempt(0, attempt));

    manager.Stop();
    mp::PathControlSnapshot snapshot;
    BOOST_REQUIRE(manager.GetPathControlSnapshot(0, snapshot));
    BOOST_TEST(static_cast<int>(snapshot.state) == static_cast<int>(mp::PathState::Stopped));
}

BOOST_AUTO_TEST_CASE(path_control_snapshot_projects_legacy_slot_state) {
    mp::PathManager manager;
    BOOST_REQUIRE(manager.Start(MakeConfig(1, 1, 1, 1)));

    mp::PathControlSnapshot snapshot;
    BOOST_REQUIRE(manager.GetPathControlSnapshot(0, snapshot));
    BOOST_TEST(snapshot.path_id == "path-0");
    BOOST_TEST(static_cast<int>(snapshot.state) == static_cast<int>(mp::PathState::Disabled));
    BOOST_TEST(static_cast<int>(snapshot.last_failure) == static_cast<int>(mp::FailureClass::None));
    BOOST_TEST(snapshot.deadline_ms == 0u);
    BOOST_TEST(snapshot.telemetry.sample_count == 0u);
    BOOST_TEST(!snapshot.capabilities.pacing);

    mp::PathManager::Attempt attempt;
    BOOST_REQUIRE(manager.BeginAttempt(100, attempt));
    BOOST_REQUIRE(manager.Fail(attempt, 100, mp::PathManager::Failure::Authentication));
    BOOST_REQUIRE(manager.GetPathControlSnapshot(0, snapshot));
    BOOST_TEST(static_cast<int>(snapshot.state) == static_cast<int>(mp::PathState::Backoff));
    BOOST_TEST(static_cast<int>(snapshot.last_failure) == static_cast<int>(mp::FailureClass::Authentication));
    BOOST_TEST(snapshot.retry_at_ms == 30100u);
}

BOOST_AUTO_TEST_CASE(path_control_owner_tracks_legacy_lifecycle) {
    mp::PathManager manager;
    BOOST_REQUIRE(manager.Start(MakeConfig(1, 1, 1, 1)));

    mp::PathManager::Attempt attempt;
    BOOST_REQUIRE(manager.BeginAttempt(100, attempt));
    mp::PathControlSnapshot snapshot;
    BOOST_REQUIRE(manager.GetPathControlSnapshot(0, snapshot));
    BOOST_TEST(static_cast<int>(snapshot.state) == static_cast<int>(mp::PathState::Probing));

    BOOST_REQUIRE(manager.MarkAuthenticating(attempt));
    BOOST_REQUIRE(manager.GetPathControlSnapshot(0, snapshot));
    BOOST_TEST(static_cast<int>(snapshot.state) == static_cast<int>(mp::PathState::Warming));

    BOOST_REQUIRE(manager.MarkJoined(attempt));
    BOOST_REQUIRE(manager.GetPathControlSnapshot(0, snapshot));
    BOOST_TEST(static_cast<int>(snapshot.state) == static_cast<int>(mp::PathState::Warming));

    BOOST_REQUIRE(manager.CarrierLost(attempt, 200));
    BOOST_REQUIRE(manager.GetPathControlSnapshot(0, snapshot));
    BOOST_TEST(static_cast<int>(snapshot.state) == static_cast<int>(mp::PathState::Backoff));

    mp::PathManager::Attempt retry;
    mp::PathManager::SlotSnapshot slot;
    BOOST_REQUIRE(manager.GetSlot(0, slot));
    BOOST_REQUIRE(manager.BeginAttempt(slot.retry_at_ms, retry));
    BOOST_REQUIRE(manager.GetPathControlSnapshot(0, snapshot));
    BOOST_TEST(static_cast<int>(snapshot.state) == static_cast<int>(mp::PathState::Probing));
}

BOOST_AUTO_TEST_CASE(concurrent_snapshots_and_late_attempts_survive_stop_restart) {
    mp::PathManager manager;
    const auto config = MakeConfig(4, 2, 8, 4);
    BOOST_REQUIRE(manager.Start(config));
    mp::PathManager::Attempt stale;
    BOOST_REQUIRE(manager.BeginAttempt(0, stale));
    manager.Stop();
    BOOST_REQUIRE(manager.Start(config));
    BOOST_TEST(!manager.MarkJoined(stale));
    BOOST_TEST(!manager.Fail(stale, 0, mp::PathManager::Failure::Network));

    std::atomic_bool done{false};
    std::atomic_int errors{0};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 4; ++reader) {
        readers.emplace_back([&]() {
            while (!done.load(std::memory_order_acquire)) {
                const std::size_t count = manager.SlotCount();
                if (count != 0 && count != 8) ++errors;
                for (std::size_t i = 0; i < count; ++i) {
                    mp::PathManager::SlotSnapshot snapshot;
                    if (!manager.GetSlot(i, snapshot) || snapshot.path_index >= 4) ++errors;
                }
            }
        });
    }
    std::thread lifecycle([&]() {
        for (int i = 0; i < 500; ++i) {
            mp::PathManager::Attempt attempt;
            if (manager.BeginAttempt(static_cast<std::uint64_t>(i) * 30000, attempt)) {
                if ((i % 2) == 0) manager.MarkJoined(attempt);
                else manager.Fail(attempt, static_cast<std::uint64_t>(i) * 30000,
                    mp::PathManager::Failure::Network);
            }
            if ((i % 17) == 0) {
                manager.Stop();
                manager.Start(config);
            }
        }
        manager.Stop();
        done.store(true, std::memory_order_release);
    });
    lifecycle.join();
    for (auto& reader : readers) reader.join();
    BOOST_TEST(errors.load() == 0);
    BOOST_TEST(manager.ActiveAttempts() == 0u);
    BOOST_TEST(manager.JoinedCarriers() == 0u);
}
