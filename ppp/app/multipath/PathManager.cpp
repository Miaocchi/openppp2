#include <ppp/app/multipath/PathManager.h>

#include <algorithm>
#include <limits>

namespace ppp::app::multipath {
    bool PathManager::Start(
        const ppp::configurations::MultipathConfiguration& configuration) noexcept {
        ppp::string error;
        if (!configuration.enabled || !configuration.Validate(error)) {
            Stop();
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        try {
            configuration_ = configuration;
            slots_.clear();
            std::size_t slot_count = 0;
            for (const auto& path : configuration_.paths) {
                slot_count += static_cast<std::size_t>(path.connections);
            }
            slots_.reserve(slot_count);
            for (std::size_t path = 0; path < configuration_.paths.size(); ++path) {
                const auto& entry = configuration_.paths[path];
                for (int connection = 0; connection < entry.connections; ++connection) {
                    slots_.emplace_back();
                    slots_.back().snapshot.path_index = static_cast<std::uint16_t>(path);
                    slots_.back().control = std::make_unique<PathControlAdapter>(
                        std::string(entry.id));
                }
            }
        }
        catch (...) {
            slots_.clear();
            running_ = false;
            ++generation_;
            active_attempts_ = 0;
            joined_carriers_ = 0;
            return false;
        }

        ++generation_;
        for (Slot& slot : slots_) {
            if (slot.control) slot.control->ResetEpoch(generation_);
        }
        active_attempts_ = 0;
        joined_carriers_ = 0;
        running_ = !slots_.empty();
        return running_;
    }

    void PathManager::Stop() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Slot& slot : slots_) {
            if (slot.control) {
                const std::uint64_t control_generation = slot.control->Snapshot().generation;
                ApplyControlEvent(slot, PathEvent::Stop, 0, control_generation);
            }
        }
        running_ = false;
        ++generation_;
        active_attempts_ = 0;
        joined_carriers_ = 0;
        for (Slot& slot : slots_) {
            slot.snapshot.state = State::Stopped;
            slot.snapshot.generation = generation_;
            slot.lifetime.reset();
        }
    }

    bool PathManager::BeginAttempt(std::uint64_t now_ms, Attempt& attempt) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        attempt = Attempt();
        if (!running_ || active_attempts_ >= static_cast<std::size_t>(configuration_.max_concurrent_connects)) {
            return false;
        }

        for (std::size_t i = 0; i < slots_.size(); ++i) {
            Slot& slot = slots_[i];
            const State state = slot.snapshot.state;
            if ((state != State::Waiting && state != State::Backoff) ||
                slot.snapshot.retry_at_ms > now_ms) {
                continue;
            }

            slot.snapshot.state = State::Connecting;
            slot.snapshot.generation = generation_;
            ++slot.attempt_generation;
            attempt.slot_generation = slot.attempt_generation;
            ++active_attempts_;
            attempt.generation = generation_;
            attempt.slot = static_cast<std::uint16_t>(i);
            attempt.path_index = slot.snapshot.path_index;
            attempt.valid = true;
            if (slot.control) {
                const PathControlSnapshot control = slot.control->Snapshot();
                std::uint64_t control_generation = attempt.slot_generation;
                if (control.state == PathState::Backoff) {
                    ApplyControlEvent(slot, PathEvent::RetryDue,
                        std::max(now_ms, control.retry_at_ms), slot.attempt_generation);
                    control_generation = slot.control->Snapshot().generation;
                }
                else if (control.state == PathState::Disabled) {
                    ApplyControlEvent(slot, PathEvent::Enable, now_ms, slot.attempt_generation);
                }
                ApplyControlEvent(slot, PathEvent::DialStarted, now_ms, control_generation);
            }
            return true;
        }
        return false;
    }

    bool PathManager::MarkAuthenticating(const Attempt& attempt) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsCurrentLocked(attempt) || slots_[attempt.slot].snapshot.state != State::Connecting) {
            return false;
        }
        slots_[attempt.slot].snapshot.state = State::Authenticating;
        const std::uint64_t control_generation = slots_[attempt.slot].control
            ? slots_[attempt.slot].control->Snapshot().generation : attempt.slot_generation;
        ApplyControlEvent(slots_[attempt.slot], PathEvent::Authenticated,
            0, control_generation, AuthData{true});
        return true;
    }

    bool PathManager::MarkJoined(const Attempt& attempt, const std::shared_ptr<void>& lifetime) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsCurrentLocked(attempt)) {
            return false;
        }
        Slot& slot = slots_[attempt.slot];
        if (slot.snapshot.state != State::Connecting && slot.snapshot.state != State::Authenticating) {
            return false;
        }
        slot.snapshot.state = State::Joined;
        slot.lifetime = lifetime;
        slot.snapshot.retry_at_ms = 0;
        slot.snapshot.failures = 0;
        slot.snapshot.failure = Failure::None;
        slot.retry_round = 0;
        if (active_attempts_ > 0) {
            --active_attempts_;
        }
        ++joined_carriers_;
        const std::uint64_t control_generation = slot.control
            ? slot.control->Snapshot().generation : attempt.slot_generation;
        ApplyControlEvent(slot, PathEvent::Joined, 0, control_generation);
        return true;
    }

    bool PathManager::Fail(const Attempt& attempt, std::uint64_t now_ms, Failure failure) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsCurrentLocked(attempt)) {
            return false;
        }
        Slot& slot = slots_[attempt.slot];
        if (slot.snapshot.state == State::Joined || slot.snapshot.state == State::Stopped ||
            slot.snapshot.state == State::Backoff) {
            return false;
        }
        if (active_attempts_ > 0) {
            --active_attempts_;
        }
        ScheduleRetry(slot, now_ms, failure);
        const std::uint64_t control_generation = slot.control
            ? slot.control->Snapshot().generation : attempt.slot_generation;
        ApplyControlEvent(slot, PathEvent::CarrierLost, now_ms, control_generation,
            FailureData{[&]() noexcept {
                switch (failure) {
                case Failure::Network: return FailureClass::Network;
                case Failure::Timeout: return FailureClass::Timeout;
                case Failure::Authentication: return FailureClass::Authentication;
                case Failure::Configuration: return FailureClass::Configuration;
                case Failure::Capacity: return FailureClass::Capacity;
                case Failure::None: return FailureClass::None;
                }
                return FailureClass::InvalidTransition;
            }(), slot.snapshot.retry_at_ms});
        return true;
    }

    bool PathManager::CarrierLost(const Attempt& attempt, std::uint64_t now_ms) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsCurrentLocked(attempt) || slots_[attempt.slot].snapshot.state != State::Joined) {
            return false;
        }
        if (joined_carriers_ > 0) {
            --joined_carriers_;
        }
        slots_[attempt.slot].lifetime.reset();
        ScheduleRetry(slots_[attempt.slot], now_ms, Failure::Network);
        const std::uint64_t control_generation = slots_[attempt.slot].control
            ? slots_[attempt.slot].control->Snapshot().generation : attempt.slot_generation;
        ApplyControlEvent(slots_[attempt.slot], PathEvent::CarrierLost, now_ms,
            control_generation, FailureData{FailureClass::Network,
                slots_[attempt.slot].snapshot.retry_at_ms});
        return true;
    }

    bool PathManager::IsCurrent(const Attempt& attempt) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return IsCurrentLocked(attempt);
    }

    bool PathManager::IsCurrentLocked(const Attempt& attempt) const noexcept {
        return running_ && attempt.valid && attempt.generation == generation_ &&
            attempt.slot < slots_.size() && attempt.path_index == slots_[attempt.slot].snapshot.path_index &&
            attempt.slot_generation == slots_[attempt.slot].attempt_generation;
    }

    bool PathManager::IsRunning() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    std::size_t PathManager::ActiveAttempts() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_attempts_;
    }

    std::size_t PathManager::JoinedCarriers() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return joined_carriers_;
    }

    std::size_t PathManager::SlotCount() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return slots_.size();
    }

    bool PathManager::GetPath(std::size_t index,
        ppp::configurations::MultipathConfiguration::Path& path) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= configuration_.paths.size()) return false;
        try { path = configuration_.paths[index]; }
        catch (...) { return false; }
        return true;
    }

    bool PathManager::GetSlot(std::size_t index, SlotSnapshot& slot) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= slots_.size()) return false;
        slot = slots_[index].snapshot;
        return true;
    }

    bool PathManager::GetPathControlSnapshot(std::size_t index,
        PathControlSnapshot& snapshot) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= slots_.size() || slots_[index].snapshot.path_index >= configuration_.paths.size()) {
            return false;
        }

        const SlotSnapshot& source = slots_[index].snapshot;
        const auto& path = configuration_.paths[source.path_index];
        snapshot = PathControlSnapshot();
        try {
            snapshot.path_id = path.id;
        } catch (...) {
            return false;
        }
        snapshot.epoch = generation_;
        snapshot.generation = source.generation;
        snapshot.retry_at_ms = source.retry_at_ms;
        snapshot.last_failure = [&]() noexcept {
            switch (source.failure) {
            case Failure::Network: return FailureClass::Network;
            case Failure::Timeout: return FailureClass::Timeout;
            case Failure::Authentication: return FailureClass::Authentication;
            case Failure::Configuration: return FailureClass::Configuration;
            case Failure::Capacity: return FailureClass::Capacity;
            case Failure::None: return FailureClass::None;
            }
            return FailureClass::InvalidTransition;
        }();
        switch (source.state) {
        case State::Waiting: snapshot.state = PathState::Disabled; break;
        case State::Connecting: snapshot.state = PathState::Probing; break;
        case State::Authenticating: snapshot.state = PathState::Warming; break;
        case State::Joined: snapshot.state = PathState::Active; break;
        case State::Backoff: snapshot.state = PathState::Backoff; break;
        case State::Stopped: snapshot.state = PathState::Stopped; break;
        }
        if (slots_[index].control) {
            const PathControlSnapshot control = slots_[index].control->Snapshot();
            snapshot.state = control.state;
            snapshot.generation = control.generation;
            snapshot.telemetry = control.telemetry;
            snapshot.capabilities = control.capabilities;
            snapshot.deadline_ms = control.deadline_ms;
            snapshot.stale_events = control.stale_events;
            snapshot.rejected_events = control.rejected_events;
        }
        return true;
    }

    bool PathManager::ApplyControlEvent(Slot& slot, PathEvent kind,
        std::uint64_t now_ms, std::uint64_t generation, PathEventData data) noexcept {
        if (!slot.control) return true;
        try {
            PathEventEnvelope event;
            event.path_id = std::string(configuration_.paths[slot.snapshot.path_index].id);
            event.epoch = generation_;
            event.generation = generation;
            event.monotonic_ms = now_ms;
            event.kind = kind;
            event.data = std::move(data);
            if (!slot.control->Enqueue(event)) return false;
            std::vector<PathTransition> transitions;
            slot.control->Pump(1, transitions);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    void PathManager::ScheduleRetry(Slot& slot, std::uint64_t now_ms, Failure failure) noexcept {
        slot.snapshot.state = State::Backoff;
        slot.snapshot.failure = failure;
        ++slot.snapshot.failures;
        ++slot.retry_round;
        const std::uint32_t exponent = std::min<std::uint32_t>(slot.retry_round - 1, 5);
        const bool configuration_error = failure == Failure::Authentication ||
            failure == Failure::Configuration || failure == Failure::Capacity;
        const std::uint32_t delay_ms = configuration_error
            ? 30000U : std::min<std::uint32_t>(30000U, 1000U << exponent);
        const std::uint32_t jittered = configuration_error ? delay_ms :
            Jitter(delay_ms, generation_ ^ slot.snapshot.failures ^ slot.snapshot.path_index);
        slot.snapshot.retry_at_ms = now_ms + jittered;
    }

    std::uint32_t PathManager::Jitter(std::uint32_t delay_ms, std::uint32_t seed) noexcept {
        // Bounded deterministic jitter avoids synchronized retry waves without requiring a global RNG.
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        const std::uint32_t spread = std::max<std::uint32_t>(1, delay_ms / 5);
        const std::int64_t offset = static_cast<std::int64_t>(seed % (spread * 2 + 1)) - spread;
        return static_cast<std::uint32_t>(std::min<std::int64_t>(30000,
            std::max<std::int64_t>(1000, static_cast<std::int64_t>(delay_ms) + offset)));
    }
}
