#define BOOST_TEST_MODULE runtime_snapshot_listener_reentrancy_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/runtime/RuntimeSnapshot.h>
#include <ppp/app/runtime/RuntimeSnapshotPublisher.h>

#include <cstdint>

namespace {

using ppp::app::runtime::RuntimeSnapshot;
using ppp::app::runtime::RuntimeSnapshotPublisher;
using ppp::app::runtime::RuntimePhase;

RuntimeSnapshot MakeSnapshot(RuntimePhase phase) {
    RuntimeSnapshot snapshot;
    snapshot.phase = phase;
    return snapshot;
}

}

BOOST_AUTO_TEST_CASE(callback_reentrancy_isolation) {
    RuntimeSnapshotPublisher publisher;
    int outer_calls = 0;
    int inner_calls = 0;
    std::uint64_t outer_token = 0;

    outer_token = publisher.Subscribe([&](const RuntimeSnapshot& snapshot) noexcept {
        ++outer_calls;
        BOOST_TEST(static_cast<int>(snapshot.phase) == static_cast<int>(RuntimePhase::Starting));

        auto inner_token = publisher.Subscribe(
            [&](const RuntimeSnapshot& inner_snapshot) noexcept {
                BOOST_TEST(
                    static_cast<int>(inner_snapshot.phase) ==
                    static_cast<int>(RuntimePhase::Connected));
                ++inner_calls;
            });

        publisher.Unsubscribe(outer_token);
        publisher.Publish(MakeSnapshot(RuntimePhase::Connected));
        publisher.Unsubscribe(inner_token);
    });

    publisher.Publish(MakeSnapshot(RuntimePhase::Starting));

    BOOST_TEST(outer_calls == 1);
    BOOST_TEST(inner_calls == 1);

    publisher.Publish(MakeSnapshot(RuntimePhase::Reconnecting));
    BOOST_TEST(outer_calls == 1);
    BOOST_TEST(inner_calls == 1);
}
