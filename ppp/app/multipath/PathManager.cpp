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
                PathTransition transition;
                ApplyControlEvent(slot, PathEvent::Stop, 0,
                    slot.control->Snapshot().generation, transition);
            }
        }
        running_ = false;
        ++generation_;
        active_attempts_ = 0;
        joined_carriers_ = 0;
        for (Slot& slot : slots_) {
            slot.snapshot.generation = generation_;
            slot.lifetime.reset();
            ProjectSlot(slot);
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
            if (!slot.control) continue;
            const PathControlSnapshot control = slot.control->Snapshot();
            if (control.state != PathState::Disabled && control.state != PathState::Backoff) {
                continue;
            }
            if (control.state == PathState::Backoff && control.retry_at_ms > now_ms) {
                continue;
            }

            const std::uint32_t previous_attempt_generation = slot.attempt_generation;
            ++slot.attempt_generation;
            ++active_attempts_;

            PathTransition transition;
            const bool armed = control.state == PathState::Backoff
                ? ApplyControlEvent(slot, PathEvent::RetryDue,
                    std::max(now_ms, control.retry_at_ms), slot.attempt_generation, transition)
                : ApplyControlEvent(slot, PathEvent::Enable, now_ms,
                    slot.attempt_generation, transition);
            const std::uint64_t control_generation = slot.control->Snapshot().generation;
            if (!armed || !transition.accepted ||
                !ApplyControlEvent(slot, PathEvent::DialStarted, now_ms,
                    control_generation, transition) ||
                !transition.accepted) {
                slot.attempt_generation = previous_attempt_generation;
                --active_attempts_;
                continue;
            }

            slot.snapshot.generation = generation_;
            ProjectSlot(slot);
            attempt.slot_generation = slot.attempt_generation;
            attempt.generation = generation_;
            attempt.slot = static_cast<std::uint16_t>(i);
            attempt.path_index = slot.snapshot.path_index;
            attempt.valid = true;
            return true;
        }
        return false;
    }

    bool PathManager::MarkAuthenticating(const Attempt& attempt) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsCurrentLocked(attempt)) return false;
        Slot& slot = slots_[attempt.slot];
        if (!slot.control) return false;
        const PathControlSnapshot control = slot.control->Snapshot();
        if (control.state != PathState::Probing) return false;
        PathTransition transition;
        if (!ApplyControlEvent(slot, PathEvent::Authenticated, 0, control.generation,
                transition, AuthData{true}) ||
            !transition.accepted) {
            return false;
        }
        ProjectSlot(slot);
        return true;
    }

    bool PathManager::MarkJoined(const Attempt& attempt, const std::shared_ptr<void>& lifetime) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsCurrentLocked(attempt)) {
            return false;
        }
        Slot& slot = slots_[attempt.slot];
        if (!slot.control || slot.lifetime != nullptr) return false;
        const PathControlSnapshot control = slot.control->Snapshot();
        if (control.state != PathState::Probing && control.state != PathState::Warming) {
            return false;
        }
        PathTransition transition;
        if (!ApplyControlEvent(slot, PathEvent::Joined, 0, control.generation, transition) ||
            !transition.accepted) {
            return false;
        }
        slot.lifetime = lifetime ? lifetime : JoinedPlaceholder();
        if (active_attempts_ > 0) {
            --active_attempts_;
        }
        ++joined_carriers_;
        slot.snapshot.failures = 0;
        slot.retry_round = 0;
        ProjectSlot(slot);
        return true;
    }

    bool PathManager::Fail(const Attempt& attempt, std::uint64_t now_ms, Failure failure) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsCurrentLocked(attempt)) {
            return false;
        }
        Slot& slot = slots_[attempt.slot];
        if (!slot.control) return false;
        const PathControlSnapshot control = slot.control->Snapshot();
        if (slot.lifetime != nullptr ||
            (control.state != PathState::Probing && control.state != PathState::Warming)) {
            return false;
        }
        const std::uint32_t previous_failures = slot.snapshot.failures;
        const std::uint32_t previous_retry_round = slot.retry_round;
        ++slot.snapshot.failures;
        ++slot.retry_round;
        const std::uint64_t retry_at_ms = now_ms + ComputeRetryDelay(slot, failure);
        PathTransition transition;
        if (!ApplyControlEvent(slot, PathEvent::CarrierLost, now_ms, control.generation,
                transition, FailureData{ToControlFailure(failure), retry_at_ms}) ||
            !transition.accepted) {
            slot.snapshot.failures = previous_failures;
            slot.retry_round = previous_retry_round;
            return false;
        }
        if (active_attempts_ > 0) {
            --active_attempts_;
        }
        ProjectSlot(slot);
        return true;
    }

    bool PathManager::CarrierLost(const Attempt& attempt, std::uint64_t now_ms) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsCurrentLocked(attempt)) return false;
        Slot& slot = slots_[attempt.slot];
        if (!slot.control || slot.lifetime == nullptr) return false;
        const PathControlSnapshot control = slot.control->Snapshot();
        const std::uint32_t previous_failures = slot.snapshot.failures;
        const std::uint32_t previous_retry_round = slot.retry_round;
        ++slot.snapshot.failures;
        ++slot.retry_round;
        const std::uint64_t retry_at_ms = now_ms + ComputeRetryDelay(slot, Failure::Network);
        PathTransition transition;
        if (!ApplyControlEvent(slot, PathEvent::CarrierLost, now_ms, control.generation,
                transition, FailureData{FailureClass::Network, retry_at_ms}) ||
            !transition.accepted) {
            slot.snapshot.failures = previous_failures;
            slot.retry_round = previous_retry_round;
            return false;
        }
        if (joined_carriers_ > 0) {
            --joined_carriers_;
        }
        slot.lifetime.reset();
        ProjectSlot(slot);
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
        if (index >= slots_.size() || !slots_[index].control) return false;
        snapshot = slots_[index].control->Snapshot();
        snapshot.epoch = generation_;
        return true;
    }

    bool PathManager::ApplyControlEvent(Slot& slot, PathEvent kind,
        std::uint64_t now_ms, std::uint64_t generation,
        PathTransition& transition, PathEventData data) noexcept {
        transition = PathTransition();
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
            if (transitions.empty()) return false;
            transition = std::move(transitions.back());
            return true;
        }
        catch (...) {
            return false;
        }
    }

    void PathManager::ProjectSlot(Slot& slot) const noexcept {
        if (!slot.control) return;
        const PathControlSnapshot control = slot.control->Snapshot();
        const bool joined = slot.lifetime != nullptr;
        switch (control.state) {
        case PathState::Disabled: slot.snapshot.state = State::Waiting; break;
        case PathState::Probing: slot.snapshot.state = State::Connecting; break;
        case PathState::Warming:
            slot.snapshot.state = joined ? State::Joined : State::Authenticating;
            break;
        case PathState::Active:
        case PathState::Degraded:
        case PathState::Draining:
        case PathState::Retiring: slot.snapshot.state = State::Joined; break;
        case PathState::Backoff:
        case PathState::Rejected: slot.snapshot.state = State::Backoff; break;
        case PathState::Stopped: slot.snapshot.state = State::Stopped; break;
        }
        slot.snapshot.retry_at_ms = control.retry_at_ms;
        slot.snapshot.failure = FromControlFailure(control.last_failure);
    }

    PathManager::Failure PathManager::FromControlFailure(FailureClass failure) noexcept {
        switch (failure) {
        case FailureClass::Network: return Failure::Network;
        case FailureClass::Timeout: return Failure::Timeout;
        case FailureClass::Authentication: return Failure::Authentication;
        case FailureClass::Configuration: return Failure::Configuration;
        case FailureClass::Capacity: return Failure::Capacity;
        case FailureClass::None:
        case FailureClass::StaleEvent:
        case FailureClass::InvalidTransition: break;
        }
        return Failure::None;
    }

    FailureClass PathManager::ToControlFailure(Failure failure) noexcept {
        switch (failure) {
        case Failure::Network: return FailureClass::Network;
        case Failure::Timeout: return FailureClass::Timeout;
        case Failure::Authentication: return FailureClass::Authentication;
        case Failure::Configuration: return FailureClass::Configuration;
        case Failure::Capacity: return FailureClass::Capacity;
        case Failure::None: return FailureClass::None;
        }
        return FailureClass::None;
    }

    std::shared_ptr<void> PathManager::JoinedPlaceholder() noexcept {
        // MarkJoined may legitimately carry no carrier lease; the placeholder
        // keeps slot.lifetime the only source of the joined flag.
        static const std::shared_ptr<void> placeholder = []() noexcept {
            try { return std::shared_ptr<void>(std::make_shared<std::uint8_t>(0)); }
            catch (...) { return std::shared_ptr<void>(); }
        }();
        return placeholder;
    }

    std::uint32_t PathManager::ComputeRetryDelay(const Slot& slot, Failure failure) const noexcept {
        const std::uint32_t exponent = std::min<std::uint32_t>(slot.retry_round - 1, 5);
        const bool configuration_error = failure == Failure::Authentication ||
            failure == Failure::Configuration || failure == Failure::Capacity;
        const std::uint32_t delay_ms = configuration_error
            ? 30000U : std::min<std::uint32_t>(30000U, 1000U << exponent);
        return configuration_error ? delay_ms :
            Jitter(delay_ms, generation_ ^ slot.snapshot.failures ^ slot.snapshot.path_index);
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
