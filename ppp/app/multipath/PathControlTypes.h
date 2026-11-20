#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ppp::app::multipath {

enum class PathState : std::uint8_t {
    Disabled,
    Probing,
    Warming,
    Active,
    Degraded,
    Draining,
    Retiring,
    Backoff,
    Rejected,
    Stopped,
};

inline const char* PathStateName(PathState state) noexcept {
    switch (state) {
    case PathState::Disabled: return "disabled";
    case PathState::Probing: return "probing";
    case PathState::Warming: return "warming";
    case PathState::Active: return "active";
    case PathState::Degraded: return "degraded";
    case PathState::Draining: return "draining";
    case PathState::Retiring: return "retiring";
    case PathState::Backoff: return "backoff";
    case PathState::Rejected: return "rejected";
    case PathState::Stopped: return "stopped";
    }
    return "stopped";
}

enum class PathEvent : std::uint8_t {
    Enable,
    Disable,
    DialStarted,
    Authenticated,
    Joined,
    SampleUpdated,
    PressureChanged,
    CarrierLost,
    Drain,
    RetryDue,
    EpochChanged,
    Stop,
    TimerExpired,
};

enum class FailureClass : std::uint8_t {
    None,
    Network,
    Timeout,
    Authentication,
    Configuration,
    Capacity,
    StaleEvent,
    InvalidTransition,
};

struct PathCapabilities final {
    bool pacing = false;
    bool target_inflight = false;
    bool reliable_loss = false;
    bool ecn = false;
    bool active_probe = false;
    bool payload_delivery_rate = false;
};

struct PathTelemetry final {
    std::uint64_t sample_count = 0;
    std::uint64_t updated_ms = 0;
    std::uint64_t current_rate_bytes_per_second = 0;
    std::uint64_t capacity_rate_bytes_per_second = 0;
    std::uint64_t min_rtt_ms = 0;
    std::uint64_t srtt_ms = 0;
    std::uint64_t queue_delay_ms = 0;
    std::uint64_t bdp_bytes = 0;
    std::uint64_t wire_inflight_bytes = 0;
    std::uint64_t local_queue_bytes = 0;
    std::uint64_t retransmission_bytes = 0;
    std::uint8_t confidence = 0;
    bool at_capacity = false;
    bool has_sample = false;
};

struct DialData final {};
struct AuthData final {
    bool accepted = false;
};
struct SampleData final {
    PathTelemetry telemetry;
};
struct PressureData final {
    bool degraded = false;
};
struct FailureData final {
    FailureClass failure = FailureClass::None;
    std::uint64_t retry_at_ms = 0;
};
struct TimerData final {
    std::uint64_t deadline_ms = 0;
};

using PathEventData = std::variant<std::monostate, DialData, AuthData, SampleData,
    PressureData, FailureData, TimerData>;

struct PathEventEnvelope final {
    std::string path_id;
    std::uint64_t epoch = 0;
    std::uint64_t generation = 0;
    std::uint64_t monotonic_ms = 0;
    PathEvent kind = PathEvent::Stop;
    PathEventData data;
};

class IPathControlEventSink {
public:
    virtual ~IPathControlEventSink() = default;
    virtual bool Publish(const PathEventEnvelope& event) noexcept = 0;
};

struct SessionPathBinding final {
    std::string path_id;
    std::uint64_t epoch = 0;
    std::uint64_t generation = 0;
    std::uint64_t monotonic_ms = 0;
    IPathControlEventSink* sink = nullptr;
};

enum class PathEffectKind : std::uint8_t {
    PublishSnapshot,
    StartDial,
    SendBudget,
    Probe,
    JoinCarrier,
    DrainCarrier,
    ScheduleRetry,
    CancelPending,
};

struct PathEffect final {
    PathEffectKind kind = PathEffectKind::PublishSnapshot;
    std::uint64_t value = 0;
    FailureClass failure = FailureClass::None;
};

struct PathControlSnapshot final {
    std::string path_id;
    PathState state = PathState::Disabled;
    std::uint64_t epoch = 0;
    std::uint64_t generation = 0;
    PathTelemetry telemetry;
    PathCapabilities capabilities;
    std::uint64_t retry_at_ms = 0;
    std::uint64_t deadline_ms = 0;
    FailureClass last_failure = FailureClass::None;
    std::uint64_t stale_events = 0;
    std::uint64_t rejected_events = 0;
};

struct PathTransition final {
    bool accepted = false;
    bool stale = false;
    PathState previous = PathState::Disabled;
    PathState next = PathState::Disabled;
    FailureClass failure = FailureClass::None;
    std::vector<PathEffect> effects;
};

} // namespace ppp::app::multipath
