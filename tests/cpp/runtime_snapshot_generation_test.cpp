#define BOOST_TEST_MODULE runtime_snapshot_generation_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/runtime/RuntimeReadiness.h>
#include <ppp/app/runtime/RuntimeSnapshot.h>
#include <ppp/app/runtime/RuntimeSnapshotPublisher.h>

#include <cstdint>

namespace {

using ppp::app::runtime::GateConnectedPhase;
using ppp::app::runtime::RuntimePhase;
using ppp::app::runtime::RuntimeReadiness;
using ppp::app::runtime::RuntimeSnapshot;
using ppp::app::runtime::RuntimeSnapshotPublisher;

class GenerationTracker final {
public:
    std::uint64_t BeginGeneration() noexcept {
        ++generation_;
        if (0 == generation_) {
            generation_ = 1;
        }
        return generation_;
    }

    void UpdatePhase(RuntimePhase phase) noexcept {
        RuntimeSnapshot snapshot;
        snapshot.schema_version = RuntimeSnapshot::SchemaVersion;
        snapshot.generation = generation_;
        snapshot.monotonic_ms = ++monotonic_ms_;
        snapshot.phase = phase;
        publisher_.Publish(std::move(snapshot));
    }

    RuntimeSnapshot GetSnapshot() const noexcept {
        return publisher_.GetLatest();
    }

    RuntimeSnapshotPublisher& Publisher() noexcept {
        return publisher_;
    }

private:
    RuntimeSnapshotPublisher publisher_;
    std::uint64_t generation_ = 0;
    std::uint64_t monotonic_ms_ = 0;
};

}

BOOST_AUTO_TEST_CASE(begin_generation_is_monotonic) {
    GenerationTracker tracker;
    BOOST_TEST(tracker.BeginGeneration() == 1u);
    BOOST_TEST(tracker.BeginGeneration() == 2u);
}

BOOST_AUTO_TEST_CASE(update_phase_publishes_immutable_generation) {
    GenerationTracker tracker;
    RuntimeSnapshot observed;
    tracker.Publisher().Subscribe([&](const RuntimeSnapshot& snapshot) noexcept {
        observed = snapshot;
    });

    BOOST_TEST(tracker.BeginGeneration() == 1u);
    tracker.UpdatePhase(RuntimePhase::Starting);
    BOOST_TEST(observed.generation == 1u);
    BOOST_TEST(static_cast<int>(observed.phase) == static_cast<int>(RuntimePhase::Starting));

    BOOST_TEST(tracker.BeginGeneration() == 2u);
    tracker.UpdatePhase(RuntimePhase::Connecting);
    BOOST_TEST(tracker.GetSnapshot().generation == 2u);
    BOOST_TEST(static_cast<int>(tracker.GetSnapshot().phase) == static_cast<int>(RuntimePhase::Connecting));
}

BOOST_AUTO_TEST_CASE(connected_requires_full_readiness) {
    RuntimeReadiness incomplete;
    incomplete.session = true;
    incomplete.adapter = true;
    // route/dns/policy still false

    BOOST_TEST(
        static_cast<int>(GateConnectedPhase(RuntimePhase::Connected, incomplete)) ==
        static_cast<int>(RuntimePhase::ApplyingPolicy));

    RuntimeReadiness ready;
    ready.session = true;
    ready.adapter = true;
    ready.route = true;
    ready.dns = true;
    ready.policy = true;
    BOOST_TEST(
        static_cast<int>(GateConnectedPhase(RuntimePhase::Connected, ready)) ==
        static_cast<int>(RuntimePhase::Connected));
    BOOST_TEST(
        static_cast<int>(GateConnectedPhase(RuntimePhase::Starting, incomplete)) ==
        static_cast<int>(RuntimePhase::Starting));
}
