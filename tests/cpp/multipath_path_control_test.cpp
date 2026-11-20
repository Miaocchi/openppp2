#define BOOST_TEST_MODULE multipath_path_control_test

#include <boost/test/included/unit_test.hpp>

#include <ppp/app/multipath/PathAdapter.h>

namespace {
using namespace ppp::app::multipath;

PathEventEnvelope Event(PathEvent kind, std::uint64_t epoch = 1,
                        std::uint64_t generation = 1, std::uint64_t now = 0,
                        PathEventData data = {}) {
    PathEventEnvelope event;
    event.path_id = "path-a";
    event.epoch = epoch;
    event.generation = generation;
    event.monotonic_ms = now;
    event.kind = kind;
    event.data = std::move(data);
    return event;
}

bool HasEffect(const PathTransition& transition, PathEffectKind kind) {
    for (const auto& effect : transition.effects) {
        if (effect.kind == kind) return true;
    }
    return false;
}
}

BOOST_AUTO_TEST_CASE(path_control_accepts_normal_lifecycle) {
    PathControl control("path-a");
    control.ResetEpoch(1);

    auto transition = control.OnEvent(Event(PathEvent::Enable));
    BOOST_TEST(transition.accepted);
    BOOST_TEST(static_cast<int>(transition.next) == static_cast<int>(PathState::Probing));
    BOOST_TEST(HasEffect(transition, PathEffectKind::StartDial));

    BOOST_TEST(control.OnEvent(Event(PathEvent::DialStarted)).accepted);
    BOOST_TEST(static_cast<int>(control.OnEvent(Event(PathEvent::Authenticated, 1, 2, 0,
        AuthData{true})).next) == static_cast<int>(PathState::Warming));

    PathTelemetry telemetry;
    telemetry.has_sample = true;
    telemetry.confidence = 200;
    transition = control.OnEvent(Event(PathEvent::SampleUpdated, 1, 3, 10,
        SampleData{telemetry}));
    BOOST_TEST(transition.accepted);
    BOOST_TEST(static_cast<int>(transition.next) == static_cast<int>(PathState::Active));
}

BOOST_AUTO_TEST_CASE(path_control_rejects_stale_events) {
    PathControl control("path-a");
    control.ResetEpoch(4);
    BOOST_TEST(control.OnEvent(Event(PathEvent::Enable, 4, 1)).accepted);

    const auto stale_epoch = control.OnEvent(Event(PathEvent::DialStarted, 3, 2));
    BOOST_TEST(!stale_epoch.accepted);
    BOOST_TEST(stale_epoch.stale);

    const auto stale_generation = control.OnEvent(Event(PathEvent::DialStarted, 4, 0));
    BOOST_TEST(!stale_generation.accepted);
    BOOST_TEST(stale_generation.stale);
    BOOST_TEST(control.Snapshot().stale_events == 2u);
}

BOOST_AUTO_TEST_CASE(path_control_epoch_change_resets_state) {
    PathControl control("path-a");
    control.ResetEpoch(1);
    BOOST_REQUIRE(control.OnEvent(Event(PathEvent::Enable, 1, 1)).accepted);

    const auto changed = control.OnEvent(Event(PathEvent::EpochChanged, 2, 0, 30));
    BOOST_TEST(changed.accepted);
    BOOST_TEST(static_cast<int>(changed.next) == static_cast<int>(PathState::Disabled));
    BOOST_TEST(control.Snapshot().epoch == 2u);
    BOOST_TEST(static_cast<int>(control.Snapshot().state) ==
        static_cast<int>(PathState::Disabled));

    const auto old_event = control.OnEvent(Event(PathEvent::Enable, 1, 2, 40));
    BOOST_TEST(!old_event.accepted);
    BOOST_TEST(old_event.stale);
    BOOST_TEST(static_cast<int>(control.Snapshot().state) ==
        static_cast<int>(PathState::Disabled));
}

BOOST_AUTO_TEST_CASE(path_control_reset_epoch_is_monotonic) {
    PathControl control("path-a");
    control.ResetEpoch(4);
    BOOST_REQUIRE(control.OnEvent(Event(PathEvent::Enable, 4, 1)).accepted);
    control.ResetEpoch(3);
    BOOST_TEST(control.Snapshot().epoch == 4u);
    BOOST_TEST(static_cast<int>(control.Snapshot().state) ==
        static_cast<int>(PathState::Probing));
}

BOOST_AUTO_TEST_CASE(path_control_adapter_rejects_stale_intents_and_reports_unsupported) {
    PathControlAdapter adapter("path-a");
    adapter.ResetEpoch(1);
    PathControlIntent intent;
    intent.epoch = 1;
    intent.generation = 0;
    intent.kind = PathEffectKind::PublishSnapshot;
    BOOST_TEST(adapter.Supports(intent.kind));
    BOOST_TEST(static_cast<int>(adapter.Apply(intent)) ==
        static_cast<int>(PathAdapterResult::Noop));

    intent.kind = PathEffectKind::StartDial;
    BOOST_TEST(!adapter.Supports(intent.kind));
    BOOST_TEST(static_cast<int>(adapter.Apply(intent)) ==
        static_cast<int>(PathAdapterResult::Unsupported));
    intent.kind = PathEffectKind::DrainCarrier;
    BOOST_TEST(!adapter.Supports(intent.kind));
    BOOST_TEST(static_cast<int>(adapter.Apply(intent)) ==
        static_cast<int>(PathAdapterResult::Unsupported));
    intent.epoch = 0;
    BOOST_TEST(static_cast<int>(adapter.Apply(intent)) ==
        static_cast<int>(PathAdapterResult::Rejected));
    intent.epoch = 1;
    intent.generation = 1;
    BOOST_TEST(static_cast<int>(adapter.Apply(intent)) ==
        static_cast<int>(PathAdapterResult::Rejected));
}

BOOST_AUTO_TEST_CASE(path_control_backoff_and_retry_create_new_generation) {
    PathControl control("path-a");
    control.ResetEpoch(1);
    BOOST_TEST(control.OnEvent(Event(PathEvent::Enable)).accepted);

    auto transition = control.OnEvent(Event(PathEvent::CarrierLost, 1, 2, 100));
    BOOST_TEST(transition.accepted);
    BOOST_TEST(static_cast<int>(transition.next) == static_cast<int>(PathState::Backoff));
    BOOST_TEST(HasEffect(transition, PathEffectKind::ScheduleRetry));

    const auto before_retry = control.OnEvent(Event(PathEvent::RetryDue, 1, 3, 500));
    BOOST_TEST(!before_retry.accepted);
    const auto retry = control.OnEvent(Event(PathEvent::RetryDue, 1, 4, 1200));
    BOOST_TEST(retry.accepted);
    BOOST_TEST(static_cast<int>(retry.next) == static_cast<int>(PathState::Probing));
    BOOST_TEST(control.Snapshot().generation == 5u);
}

BOOST_AUTO_TEST_CASE(path_control_preserves_explicit_failure_class) {
    PathControl control("path-a");
    control.ResetEpoch(1);
    BOOST_REQUIRE(control.OnEvent(Event(PathEvent::Enable, 1, 1)).accepted);
    const auto transition = control.OnEvent(Event(PathEvent::CarrierLost, 1, 1, 50,
        FailureData{FailureClass::Authentication}));
    BOOST_TEST(static_cast<int>(transition.failure) ==
        static_cast<int>(FailureClass::Authentication));
    BOOST_TEST(static_cast<int>(control.Snapshot().last_failure) ==
        static_cast<int>(FailureClass::Authentication));
}

BOOST_AUTO_TEST_CASE(path_control_preserves_adapter_retry_deadline) {
    PathControl control("path-a");
    control.ResetEpoch(1);
    BOOST_REQUIRE(control.OnEvent(Event(PathEvent::Enable, 1, 1)).accepted);
    const auto transition = control.OnEvent(Event(PathEvent::CarrierLost, 1, 2, 50,
        FailureData{FailureClass::Network, 9050}));
    BOOST_TEST(transition.accepted);
    BOOST_TEST(control.Snapshot().retry_at_ms == 9050u);
    BOOST_TEST(static_cast<int>(transition.effects.front().kind) ==
        static_cast<int>(PathEffectKind::ScheduleRetry));
    BOOST_TEST(transition.effects.front().value == 9050u);
}

BOOST_AUTO_TEST_CASE(path_control_rejected_authentication_can_be_reenabled) {
    PathControl control("path-a");
    control.ResetEpoch(1);
    BOOST_REQUIRE(control.OnEvent(Event(PathEvent::Enable, 1, 1)).accepted);
    const auto rejected = control.OnEvent(Event(PathEvent::Authenticated, 1, 2, 0,
        AuthData{false}));
    BOOST_TEST(rejected.accepted);
    BOOST_TEST(static_cast<int>(rejected.next) == static_cast<int>(PathState::Rejected));
    BOOST_TEST(static_cast<int>(rejected.failure) == static_cast<int>(FailureClass::Authentication));

    const auto enabled = control.OnEvent(Event(PathEvent::Enable, 1, 3));
    BOOST_TEST(enabled.accepted);
    BOOST_TEST(static_cast<int>(enabled.next) == static_cast<int>(PathState::Probing));
}

BOOST_AUTO_TEST_CASE(path_control_drain_timer_retires_and_timer_expires_backoff) {
    PathControl control("path-a");
    control.ResetEpoch(1);
    BOOST_REQUIRE(control.OnEvent(Event(PathEvent::Enable, 1, 1)).accepted);
    BOOST_REQUIRE(control.OnEvent(Event(PathEvent::DialStarted, 1, 2)).accepted);
    BOOST_REQUIRE(control.OnEvent(Event(PathEvent::Authenticated, 1, 3, 0,
        AuthData{true})).accepted);
    const auto drained = control.OnEvent(Event(PathEvent::Drain, 1, 4));
    BOOST_TEST(drained.accepted);
    BOOST_TEST(static_cast<int>(drained.next) == static_cast<int>(PathState::Draining));
    const auto retired = control.OnEvent(Event(PathEvent::TimerExpired, 1, 5, 100));
    BOOST_TEST(retired.accepted);
    BOOST_TEST(static_cast<int>(retired.next) == static_cast<int>(PathState::Retiring));

    PathControl probing("path-a");
    probing.ResetEpoch(1);
    BOOST_REQUIRE(probing.OnEvent(Event(PathEvent::Enable, 1, 1)).accepted);
    const auto timeout = probing.OnEvent(Event(PathEvent::TimerExpired, 1, 2, 200));
    BOOST_TEST(timeout.accepted);
    BOOST_TEST(static_cast<int>(timeout.next) == static_cast<int>(PathState::Backoff));
    BOOST_TEST(static_cast<int>(timeout.failure) == static_cast<int>(FailureClass::Timeout));
}

BOOST_AUTO_TEST_CASE(path_control_stop_is_terminal_and_disable_is_idempotent) {
    PathControl control("path-a");
    control.ResetEpoch(1);
    auto transition = control.OnEvent(Event(PathEvent::Disable));
    BOOST_TEST(transition.accepted);
    BOOST_TEST(static_cast<int>(transition.next) == static_cast<int>(PathState::Disabled));

    transition = control.OnEvent(Event(PathEvent::Stop, 1, 2));
    BOOST_TEST(transition.accepted);
    BOOST_TEST(static_cast<int>(transition.next) == static_cast<int>(PathState::Stopped));
    BOOST_TEST(HasEffect(transition, PathEffectKind::CancelPending));

    transition = control.OnEvent(Event(PathEvent::Stop, 1, 2));
    BOOST_TEST(transition.accepted);
    BOOST_TEST(static_cast<int>(transition.next) == static_cast<int>(PathState::Stopped));
    BOOST_TEST(transition.effects.empty());

    transition = control.OnEvent(Event(PathEvent::Enable, 1, 3));
    BOOST_TEST(!transition.accepted);
    BOOST_TEST(control.IsTerminal());

    transition = control.OnEvent(Event(PathEvent::EpochChanged, 2, 0));
    BOOST_TEST(!transition.accepted);
    BOOST_TEST(transition.stale);
    BOOST_TEST(control.Snapshot().epoch == 1u);
}

BOOST_AUTO_TEST_CASE(path_control_owner_pumps_in_order_and_bounds_queue) {
    PathControlOwner owner("path-a");
    owner.ResetEpoch(1);
    for (std::size_t i = 0; i < PathControlOwner::MaxQueuedEvents; ++i) {
        BOOST_TEST(owner.Enqueue(Event(PathEvent::Enable, 1, i + 1)));
    }
    BOOST_TEST(!owner.Enqueue(Event(PathEvent::Enable, 1, 1000)));
    BOOST_TEST(owner.QueuedEvents() == PathControlOwner::MaxQueuedEvents);

    std::vector<PathTransition> transitions;
    BOOST_TEST(owner.Pump(2, transitions) == 2u);
    BOOST_TEST(transitions.size() == 2u);
    BOOST_TEST(owner.QueuedEvents() == PathControlOwner::MaxQueuedEvents - 2);
}

BOOST_AUTO_TEST_CASE(path_control_owner_preserves_lifecycle_events_when_full) {
    PathControlOwner owner("path-a");
    owner.ResetEpoch(1);
    for (std::size_t i = 0; i + 1 < PathControlOwner::MaxQueuedEvents; ++i) {
        BOOST_TEST(owner.Enqueue(Event(PathEvent::Enable, 1, i + 1, i + 1)));
    }

    PathTelemetry telemetry;
    telemetry.updated_ms = 999;
    BOOST_REQUIRE(owner.Enqueue(Event(PathEvent::SampleUpdated, 1, 999, 999,
        SampleData{telemetry})));
    BOOST_TEST(owner.Enqueue(Event(PathEvent::Stop, 1, 1000, 1000)));
    BOOST_TEST(owner.QueuedEvents() == PathControlOwner::MaxQueuedEvents);
    std::vector<PathTransition> transitions;
    owner.Pump(PathControlOwner::MaxQueuedEvents, transitions);
    BOOST_TEST(!transitions.empty());
    BOOST_TEST(static_cast<int>(transitions.back().next) ==
        static_cast<int>(PathState::Stopped));
}

BOOST_AUTO_TEST_CASE(path_control_owner_coalescing_keeps_newest_telemetry) {
    PathControlOwner owner("path-a");
    owner.ResetEpoch(1);
    BOOST_REQUIRE(owner.Enqueue(Event(PathEvent::Enable, 1, 1)));
    BOOST_REQUIRE(owner.Enqueue(Event(PathEvent::DialStarted, 1, 2)));
    BOOST_REQUIRE(owner.Enqueue(Event(PathEvent::Authenticated, 1, 3, 0, AuthData{true})));
    std::vector<PathTransition> lifecycle;
    owner.Pump(3, lifecycle);
    PathTelemetry newer;
    newer.has_sample = true;
    newer.sample_count = 9;
    BOOST_REQUIRE(owner.Enqueue(Event(PathEvent::SampleUpdated, 1, 9, 90,
        SampleData{newer})));
    PathTelemetry older;
    older.has_sample = true;
    older.sample_count = 2;
    BOOST_REQUIRE(owner.Enqueue(Event(PathEvent::SampleUpdated, 1, 2, 20,
        SampleData{older})));
    BOOST_TEST(owner.QueuedEvents() == 1u);
    std::vector<PathTransition> transitions;
    owner.Pump(1, transitions);
    BOOST_TEST(owner.Snapshot().telemetry.sample_count == 9u);

    PathTelemetry old_epoch;
    old_epoch.sample_count = 1;
    BOOST_REQUIRE(owner.Enqueue(Event(PathEvent::SampleUpdated, 1, 10, 100,
        SampleData{newer})));
    BOOST_REQUIRE(owner.Enqueue(Event(PathEvent::SampleUpdated, 0, 100, 100,
        SampleData{old_epoch})));
    BOOST_TEST(owner.QueuedEvents() == 1u);
    BOOST_REQUIRE(owner.Pump(1, transitions) == 1u);
    BOOST_TEST(owner.Snapshot().telemetry.sample_count == 9u);
}
