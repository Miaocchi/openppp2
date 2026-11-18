#pragma once

#include <ppp/app/multipath/PathControlTypes.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace ppp::app::multipath {

class PathControl final {
public:
    explicit PathControl(std::string path_id = {}) noexcept;

    PathTransition OnEvent(const PathEventEnvelope& event) noexcept;
    PathControlSnapshot Snapshot() const noexcept;
    void ResetEpoch(std::uint64_t epoch) noexcept;
    bool IsTerminal() const noexcept;

private:
    bool IsCurrentEvent(const PathEventEnvelope& event) const noexcept;
    PathTransition Reject(const PathEventEnvelope& event, FailureClass failure) noexcept;
    PathTransition Accept(PathState next, PathEvent event) noexcept;
    void AddEffect(PathTransition& transition, PathEffectKind kind,
                   std::uint64_t value = 0,
                   FailureClass failure = FailureClass::None) noexcept;
    void ScheduleBackoff(FailureClass failure, std::uint64_t now_ms,
                         PathTransition& transition,
                         std::uint64_t retry_at_ms = 0) noexcept;

    mutable std::mutex mutex_;
    PathControlSnapshot snapshot_;
};

/**
 * Bounded serialized owner for one path control state machine. Producers may
 * enqueue immutable events from transport executors; only Pump mutates the
 * state machine and executes no transport side effects itself.
 */
class PathControlOwner final {
public:
    static constexpr std::size_t MaxQueuedEvents = 128;

    explicit PathControlOwner(std::string path_id = {}) noexcept
        : control_(std::move(path_id)) {}

    bool Enqueue(const PathEventEnvelope& event) noexcept;
    std::size_t Pump(std::size_t max_events,
                     std::vector<PathTransition>& transitions) noexcept;
    PathControlSnapshot Snapshot() const noexcept { return control_.Snapshot(); }
    void ResetEpoch(std::uint64_t epoch) noexcept { control_.ResetEpoch(epoch); }
    std::size_t QueuedEvents() const noexcept;

private:
    static bool IsCoalescible(PathEvent kind) noexcept;

    mutable std::mutex queue_mutex_;
    std::deque<PathEventEnvelope> queue_;
    PathControl control_;
};

} // namespace ppp::app::multipath
