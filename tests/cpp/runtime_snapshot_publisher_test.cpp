#define BOOST_TEST_MODULE runtime_snapshot_publisher_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/runtime/RuntimeSnapshot.h>
#include <ppp/app/runtime/RuntimeSnapshotPublisher.h>

namespace {

using ppp::app::runtime::RuntimeSnapshot;
using ppp::app::runtime::RuntimeSnapshotPublisher;
using ppp::app::runtime::RuntimePhase;

void PopulateSnapshot(RuntimeSnapshot& snapshot, RuntimePhase phase, std::uint64_t generation) {
    snapshot = RuntimeSnapshot();
    snapshot.generation = generation;
    snapshot.phase = phase;
}

}

BOOST_AUTO_TEST_CASE(publisher_subscribe_and_publish_notifies_all) {
    RuntimeSnapshotPublisher publisher;
    int callback_count = 0;

    auto token_a = publisher.Subscribe([&](const RuntimeSnapshot& snapshot) noexcept {
        BOOST_TEST(static_cast<int>(snapshot.phase) == static_cast<int>(RuntimePhase::Starting));
        ++callback_count;
    });
    auto token_b = publisher.Subscribe([&](const RuntimeSnapshot& snapshot) noexcept {
        BOOST_TEST(static_cast<int>(snapshot.phase) == static_cast<int>(RuntimePhase::Starting));
        ++callback_count;
    });

    BOOST_TEST(token_a != 0u);
    BOOST_TEST(token_b != 0u);
    BOOST_TEST(token_a != token_b);

    RuntimeSnapshot snapshot;
    PopulateSnapshot(snapshot, RuntimePhase::Starting, 1);
    publisher.Publish(snapshot);
    BOOST_TEST(callback_count == 2);
}

BOOST_AUTO_TEST_CASE(publisher_unsubscribe_stops_future_notifications) {
    RuntimeSnapshotPublisher publisher;
    int callback_count = 0;

    auto token = publisher.Subscribe([&](const RuntimeSnapshot& snapshot) noexcept {
        BOOST_TEST(static_cast<int>(snapshot.phase) == static_cast<int>(RuntimePhase::Connected));
        ++callback_count;
    });
    RuntimeSnapshot snapshot;
    PopulateSnapshot(snapshot, RuntimePhase::Connected, 1);
    publisher.Publish(snapshot);

    publisher.Unsubscribe(token);
    publisher.Publish(snapshot);

    BOOST_TEST(callback_count == 1);
}

BOOST_AUTO_TEST_CASE(latest_is_persisted_after_publish) {
    RuntimeSnapshotPublisher publisher;
    RuntimeSnapshot first;
    PopulateSnapshot(first, RuntimePhase::Starting, 1);
    publisher.Publish(first);

    RuntimeSnapshot second;
    PopulateSnapshot(second, RuntimePhase::Connected, 2);
    publisher.Publish(second);

    const RuntimeSnapshot latest = publisher.GetLatest();
    BOOST_TEST(latest.generation == 2u);
    BOOST_TEST(static_cast<int>(latest.phase) == static_cast<int>(RuntimePhase::Connected));
}

BOOST_AUTO_TEST_CASE(callback_receives_value_copy_not_live_mutex_state) {
    RuntimeSnapshotPublisher publisher;
    RuntimeSnapshot seen;
    bool nested_publish_ok = false;
    std::uint64_t token = 0;

    token = publisher.Subscribe([&](const RuntimeSnapshot& snapshot) noexcept {
        seen = snapshot;
        publisher.Unsubscribe(token);
        // Must be able to re-enter Publish without deadlocking (mutex released).
        RuntimeSnapshot nested;
        PopulateSnapshot(nested, RuntimePhase::Connected, 2);
        publisher.Publish(nested);
        nested_publish_ok = true;
    });

    RuntimeSnapshot first;
    PopulateSnapshot(first, RuntimePhase::Starting, 1);
    publisher.Publish(first);

    BOOST_TEST(nested_publish_ok);
    BOOST_TEST(static_cast<int>(seen.phase) == static_cast<int>(RuntimePhase::Starting));
    BOOST_TEST(static_cast<int>(publisher.GetLatest().phase) == static_cast<int>(RuntimePhase::Connected));
}
