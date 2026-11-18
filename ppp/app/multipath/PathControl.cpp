#include <ppp/app/multipath/PathControl.h>

#include <algorithm>
#include <utility>

namespace ppp::app::multipath {

PathControl::PathControl(std::string path_id) noexcept {
    snapshot_.path_id = std::move(path_id);
}

bool PathControl::IsCurrentEvent(const PathEventEnvelope& event) const noexcept {
    return event.path_id == snapshot_.path_id && event.epoch == snapshot_.epoch &&
        event.generation >= snapshot_.generation;
}

void PathControl::AddEffect(PathTransition& transition, PathEffectKind kind,
                            std::uint64_t value, FailureClass failure) noexcept {
    try {
        transition.effects.push_back(PathEffect{kind, value, failure});
    } catch (...) {
        transition.accepted = false;
        transition.failure = FailureClass::Capacity;
        snapshot_.last_failure = FailureClass::Capacity;
    }
}

PathTransition PathControl::Reject(const PathEventEnvelope& event,
                                   FailureClass failure) noexcept {
    PathTransition transition;
    transition.previous = snapshot_.state;
    transition.next = snapshot_.state;
    transition.failure = failure;
    transition.stale = failure == FailureClass::StaleEvent;
    if (transition.stale) {
        ++snapshot_.stale_events;
    } else {
        ++snapshot_.rejected_events;
    }
    snapshot_.last_failure = failure;
    (void)event;
    return transition;
}

PathTransition PathControl::Accept(PathState next, PathEvent event) noexcept {
    PathTransition transition;
    transition.accepted = true;
    transition.previous = snapshot_.state;
    transition.next = next;
    snapshot_.state = next;
    snapshot_.last_failure = FailureClass::None;
    AddEffect(transition, PathEffectKind::PublishSnapshot);
    if (event == PathEvent::Disable || event == PathEvent::Stop) {
        AddEffect(transition, PathEffectKind::CancelPending);
    }
    return transition;
}

void PathControl::ScheduleBackoff(FailureClass failure, std::uint64_t now_ms,
                                  PathTransition& transition,
                                  std::uint64_t retry_at_ms) noexcept {
    transition.failure = failure;
    snapshot_.last_failure = failure;
    snapshot_.retry_at_ms = retry_at_ms > now_ms ? retry_at_ms : now_ms + 1000;
    transition.next = PathState::Backoff;
    snapshot_.state = PathState::Backoff;
    AddEffect(transition, PathEffectKind::ScheduleRetry, snapshot_.retry_at_ms, failure);
    AddEffect(transition, PathEffectKind::PublishSnapshot);
}

PathTransition PathControl::OnEvent(const PathEventEnvelope& event) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event.path_id != snapshot_.path_id) {
        return Reject(event, FailureClass::StaleEvent);
    }
    if (snapshot_.state == PathState::Stopped) {
        if (event.epoch == snapshot_.epoch && event.kind == PathEvent::Stop) {
            PathTransition transition;
            transition.accepted = true;
            transition.previous = PathState::Stopped;
            transition.next = PathState::Stopped;
            return transition;
        }
        return Reject(event, FailureClass::StaleEvent);
    }
    if (event.kind == PathEvent::EpochChanged && event.epoch > snapshot_.epoch) {
        snapshot_.epoch = event.epoch;
        snapshot_.generation = event.generation;
        snapshot_.state = PathState::Disabled;
        snapshot_.retry_at_ms = 0;
        snapshot_.deadline_ms = 0;
        snapshot_.telemetry = PathTelemetry();
        snapshot_.last_failure = FailureClass::None;
        PathTransition transition = Accept(PathState::Disabled, event.kind);
        AddEffect(transition, PathEffectKind::CancelPending);
        AddEffect(transition, PathEffectKind::PublishSnapshot);
        return transition;
    }
    if (event.epoch != snapshot_.epoch) {
        return Reject(event, FailureClass::StaleEvent);
    }
    if (event.generation < snapshot_.generation) {
        return Reject(event, FailureClass::StaleEvent);
    }
    snapshot_.generation = event.generation;

    switch (event.kind) {
    case PathEvent::Enable:
        if (snapshot_.state == PathState::Disabled || snapshot_.state == PathState::Rejected) {
            PathTransition transition = Accept(PathState::Probing, event.kind);
            AddEffect(transition, PathEffectKind::StartDial, snapshot_.epoch);
            return transition;
        }
        return Reject(event, FailureClass::InvalidTransition);
    case PathEvent::Disable:
        if (snapshot_.state == PathState::Disabled) {
            return Accept(PathState::Disabled, event.kind);
        }
        return Accept(PathState::Disabled, event.kind);
    case PathEvent::DialStarted:
        if (snapshot_.state == PathState::Probing) {
            return Accept(PathState::Probing, event.kind);
        }
        return Reject(event, FailureClass::InvalidTransition);
    case PathEvent::Authenticated: {
        const auto* auth = std::get_if<AuthData>(&event.data);
        if (snapshot_.state != PathState::Probing || auth == nullptr || !auth->accepted) {
            if (snapshot_.state == PathState::Probing && auth != nullptr && !auth->accepted) {
                PathTransition transition = Accept(PathState::Rejected, event.kind);
                transition.failure = FailureClass::Authentication;
                snapshot_.last_failure = FailureClass::Authentication;
                AddEffect(transition, PathEffectKind::CancelPending);
                return transition;
            }
            return Reject(event, FailureClass::InvalidTransition);
        }
        return Accept(PathState::Warming, event.kind);
    }
    case PathEvent::Joined:
        if (snapshot_.state == PathState::Probing || snapshot_.state == PathState::Warming) {
            return Accept(PathState::Warming, event.kind);
        }
        if (snapshot_.state == PathState::Active) {
            return Accept(PathState::Active, event.kind);
        }
        return Reject(event, FailureClass::InvalidTransition);
    case PathEvent::SampleUpdated: {
        const auto* sample = std::get_if<SampleData>(&event.data);
        if (sample == nullptr || (snapshot_.state != PathState::Warming &&
            snapshot_.state != PathState::Active && snapshot_.state != PathState::Degraded)) {
            return Reject(event, FailureClass::InvalidTransition);
        }
        snapshot_.telemetry = sample->telemetry;
        const bool fresh = sample->telemetry.has_sample && sample->telemetry.confidence >= 128;
        return Accept(fresh ? PathState::Active : PathState::Warming, event.kind);
    }
    case PathEvent::PressureChanged: {
        const auto* pressure = std::get_if<PressureData>(&event.data);
        if (pressure == nullptr || (snapshot_.state != PathState::Warming &&
            snapshot_.state != PathState::Active && snapshot_.state != PathState::Degraded)) {
            return Reject(event, FailureClass::InvalidTransition);
        }
        return Accept(pressure->degraded ? PathState::Degraded : PathState::Warming, event.kind);
    }
    case PathEvent::CarrierLost:
        if (snapshot_.state == PathState::Probing || snapshot_.state == PathState::Warming ||
            snapshot_.state == PathState::Active || snapshot_.state == PathState::Degraded) {
            PathTransition transition;
            transition.accepted = true;
            transition.previous = snapshot_.state;
            FailureClass failure = FailureClass::Network;
            if (const auto* failure_data = std::get_if<FailureData>(&event.data)) {
                if (failure_data->failure != FailureClass::None) {
                    failure = failure_data->failure;
                }
                ScheduleBackoff(failure, event.monotonic_ms, transition,
                    failure_data->retry_at_ms);
                return transition;
            }
            transition.failure = failure;
            ScheduleBackoff(failure, event.monotonic_ms, transition);
            return transition;
        }
        if (snapshot_.state == PathState::Draining || snapshot_.state == PathState::Retiring) {
            return Accept(PathState::Retiring, event.kind);
        }
        return Reject(event, FailureClass::InvalidTransition);
    case PathEvent::Drain:
        if (snapshot_.state == PathState::Active || snapshot_.state == PathState::Degraded ||
            snapshot_.state == PathState::Warming) {
            return Accept(PathState::Draining, event.kind);
        }
        if (snapshot_.state == PathState::Draining) {
            return Accept(PathState::Draining, event.kind);
        }
        return Reject(event, FailureClass::InvalidTransition);
    case PathEvent::RetryDue:
        if (snapshot_.state == PathState::Backoff && event.monotonic_ms >= snapshot_.retry_at_ms) {
            PathTransition transition = Accept(PathState::Probing, event.kind);
            ++snapshot_.generation;
            AddEffect(transition, PathEffectKind::StartDial, snapshot_.epoch);
            return transition;
        }
        return Reject(event, FailureClass::InvalidTransition);
    case PathEvent::EpochChanged:
        return Reject(event, FailureClass::InvalidTransition);
    case PathEvent::TimerExpired:
        if (snapshot_.state == PathState::Draining || snapshot_.state == PathState::Retiring) {
            return Accept(PathState::Retiring, event.kind);
        }
        if (snapshot_.state == PathState::Probing || snapshot_.state == PathState::Warming) {
            PathTransition transition;
            transition.accepted = true;
            transition.previous = snapshot_.state;
            ScheduleBackoff(FailureClass::Timeout, event.monotonic_ms, transition);
            return transition;
        }
        return Reject(event, FailureClass::InvalidTransition);
    case PathEvent::Stop:
        return Accept(PathState::Stopped, event.kind);
    }
    return Reject(event, FailureClass::InvalidTransition);
}

PathControlSnapshot PathControl::Snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void PathControl::ResetEpoch(std::uint64_t epoch) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.state == PathState::Stopped || epoch <= snapshot_.epoch) {
        return;
    }
    snapshot_.epoch = epoch;
    snapshot_.generation = 0;
    snapshot_.state = PathState::Disabled;
    snapshot_.retry_at_ms = 0;
    snapshot_.deadline_ms = 0;
    snapshot_.last_failure = FailureClass::None;
    snapshot_.telemetry = PathTelemetry();
}

bool PathControl::IsTerminal() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_.state == PathState::Stopped;
}

bool PathControlOwner::Enqueue(const PathEventEnvelope& event) noexcept {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    try {
        if (IsCoalescible(event.kind)) {
            for (auto it = queue_.rbegin(); it != queue_.rend(); ++it) {
                if (it->kind == event.kind && it->path_id == event.path_id) {
                    if (event.epoch < it->epoch ||
                        (event.epoch == it->epoch && event.generation < it->generation)) {
                        return true;
                    }
                    *it = event;
                    return true;
                }
            }
        }
        if (queue_.size() >= MaxQueuedEvents) {
            auto telemetry = std::find_if(queue_.begin(), queue_.end(),
                [](const PathEventEnvelope& queued) {
                    return IsCoalescible(queued.kind);
                });
            if (telemetry == queue_.end()) {
                return false;
            }
            queue_.erase(telemetry);
        }
        queue_.push_back(event);
        return true;
    } catch (...) {
        return false;
    }
}

bool PathControlOwner::IsCoalescible(PathEvent kind) noexcept {
    return kind == PathEvent::SampleUpdated || kind == PathEvent::PressureChanged;
}

std::size_t PathControlOwner::Pump(std::size_t max_events,
                                   std::vector<PathTransition>& transitions) noexcept {
    std::size_t processed = 0;
    while (processed < max_events) {
        PathEventEnvelope event;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (queue_.empty()) break;
            event = std::move(queue_.front());
            queue_.pop_front();
        }
        const PathTransition transition = control_.OnEvent(event);
        try {
            transitions.push_back(transition);
        } catch (...) {
            // The state transition has already been applied. The caller can
            // observe the resulting snapshot even if result collection is full.
        }
        ++processed;
    }
    return processed;
}

std::size_t PathControlOwner::QueuedEvents() const noexcept {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return queue_.size();
}

} // namespace ppp::app::multipath
