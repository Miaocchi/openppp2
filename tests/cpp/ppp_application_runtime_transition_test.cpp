#define BOOST_TEST_MODULE ppp_application_runtime_transition_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/runtime/RuntimePhase.h>
#include <ppp/app/runtime/RuntimeReadiness.h>

#include <string>

namespace {

using ppp::app::runtime::GateConnectedPhase;
using ppp::app::runtime::ParseRuntimePhase;
using ppp::app::runtime::RuntimePhase;
using ppp::app::runtime::RuntimeReadiness;
using ppp::app::runtime::ToString;

RuntimePhase ResolveClientPhase(
    bool has_client,
    bool has_exchanger,
    bool established,
    bool reconnecting,
    const RuntimeReadiness& readiness) noexcept {
    if (!has_client) {
        return RuntimePhase::Starting;
    }
    if (!has_exchanger) {
        return RuntimePhase::Connecting;
    }
    if (reconnecting) {
        return RuntimePhase::Reconnecting;
    }
    if (established) {
        return GateConnectedPhase(RuntimePhase::Connected, readiness);
    }
    return readiness.policy ? RuntimePhase::Handshaking : RuntimePhase::Connecting;
}

}

BOOST_AUTO_TEST_CASE(runtime_transition_phase_round_trip) {
    const RuntimePhase ordered_phases[] = {
        RuntimePhase::Idle,
        RuntimePhase::Starting,
        RuntimePhase::PreparingHost,
        RuntimePhase::Connecting,
        RuntimePhase::Handshaking,
        RuntimePhase::ApplyingPolicy,
        RuntimePhase::Connected,
        RuntimePhase::Reconnecting,
        RuntimePhase::Stopping,
        RuntimePhase::Failed,
        RuntimePhase::Unknown,
    };

    for (RuntimePhase phase : ordered_phases) {
        const std::string phase_text = ToString(phase);
        if (phase == RuntimePhase::Unknown) {
            BOOST_TEST(phase_text == "unknown");
            continue;
        }

        BOOST_TEST(static_cast<int>(ParseRuntimePhase(phase_text)) == static_cast<int>(phase));
    }
}

BOOST_AUTO_TEST_CASE(established_without_readiness_stays_applying_policy) {
    RuntimeReadiness incomplete;
    incomplete.session = true;
    incomplete.adapter = true;

    const RuntimePhase phase = ResolveClientPhase(
        true, true, true, false, incomplete);
    BOOST_TEST(static_cast<int>(phase) == static_cast<int>(RuntimePhase::ApplyingPolicy));
}

BOOST_AUTO_TEST_CASE(established_with_full_readiness_is_connected) {
    RuntimeReadiness ready;
    ready.session = true;
    ready.adapter = true;
    ready.route = true;
    ready.dns = true;
    ready.policy = true;

    const RuntimePhase phase = ResolveClientPhase(
        true, true, true, false, ready);
    BOOST_TEST(static_cast<int>(phase) == static_cast<int>(RuntimePhase::Connected));
}

BOOST_AUTO_TEST_CASE(reconnect_overrides_connected) {
    RuntimeReadiness ready;
    ready.session = true;
    ready.adapter = true;
    ready.route = true;
    ready.dns = true;
    ready.policy = true;

    const RuntimePhase phase = ResolveClientPhase(
        true, true, true, true, ready);
    BOOST_TEST(static_cast<int>(phase) == static_cast<int>(RuntimePhase::Reconnecting));
}
