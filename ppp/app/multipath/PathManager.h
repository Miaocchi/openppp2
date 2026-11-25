#pragma once

#include <ppp/configurations/MultipathConfiguration.h>
#include <ppp/app/multipath/PathAdapter.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace ppp::app::multipath {
    class PathManager final {
    public:
        enum class State : std::uint8_t {
            Waiting,
            Connecting,
            Authenticating,
            Joined,
            Backoff,
            Stopped,
        };
        enum class Failure : std::uint8_t {
            None,
            Network,
            Timeout,
            Authentication,
            Configuration,
            Capacity,
        };

        struct Attempt final {
            std::uint32_t generation = 0;
            std::uint32_t slot_generation = 0;
            std::uint16_t slot = 0;
            std::uint16_t path_index = 0;
            bool valid = false;
        };

        struct SlotSnapshot final {
            State state = State::Waiting;
            std::uint32_t generation = 0;
            std::uint32_t failures = 0;
            std::uint64_t retry_at_ms = 0;
            std::uint16_t path_index = 0;
            Failure failure = Failure::None;
        };

        bool Start(const ppp::configurations::MultipathConfiguration& configuration) noexcept;
        void Stop() noexcept;
        bool BeginAttempt(std::uint64_t now_ms, Attempt& attempt, bool pool_trial = false) noexcept;
        bool BeginAttempt(std::uint64_t now_ms, Attempt& attempt, bool prefer_unused_path,
                          std::optional<std::uint16_t> preferred_path) noexcept;
        bool BeginAttempt(std::uint64_t now_ms, Attempt& attempt, bool pool_trial,
                          bool prefer_unused_path, std::optional<std::uint16_t> preferred_path) noexcept;
        bool MarkAuthenticating(const Attempt& attempt) noexcept;
        bool MarkJoined(const Attempt& attempt, const std::shared_ptr<void>& lifetime = nullptr) noexcept;
        bool Fail(const Attempt& attempt, std::uint64_t now_ms, Failure failure) noexcept;
        bool CarrierLost(const Attempt& attempt, std::uint64_t now_ms) noexcept;
        bool CarrierRetired(const Attempt& attempt) noexcept;
        bool IsCurrent(const Attempt& attempt) const noexcept;
        bool IsRunning() const noexcept;
        std::size_t ActiveAttempts() const noexcept;
        std::size_t JoinedCarriers() const noexcept;
        std::size_t SlotCount() const noexcept;
        bool GetPath(std::size_t index, ppp::configurations::MultipathConfiguration::Path& path) const noexcept;
        bool GetSlot(std::size_t index, SlotSnapshot& slot) const noexcept;
        // The PathControl state machine is authoritative; this returns its live
        // snapshot with the current manager epoch.
        bool GetPathControlSnapshot(std::size_t index,
            PathControlSnapshot& snapshot) const noexcept;

    private:
        struct Slot final {
            SlotSnapshot snapshot;
            std::uint32_t retry_round = 0;
            std::uint32_t attempt_generation = 0;
            std::shared_ptr<void> lifetime;
            std::unique_ptr<PathControlAdapter> control;
        };

        // Backoff policy input: jittered delay in milliseconds. The resulting
        // retry deadline lives only in the PathControl snapshot.
        std::uint32_t ComputeRetryDelay(const Slot& slot, Failure failure) const noexcept;
        void ProjectSlot(Slot& slot) const noexcept;
        bool ApplyControlEvent(Slot& slot, PathEvent kind, std::uint64_t now_ms,
                               std::uint64_t generation,
                               PathTransition& transition,
                               PathEventData data = {}) noexcept;
        static Failure FromControlFailure(FailureClass failure) noexcept;
        static FailureClass ToControlFailure(Failure failure) noexcept;
        static std::shared_ptr<void> JoinedPlaceholder() noexcept;
        bool IsCurrentLocked(const Attempt& attempt) const noexcept;
        static std::uint32_t Jitter(std::uint32_t delay_ms, std::uint32_t seed) noexcept;

        ppp::configurations::MultipathConfiguration configuration_;
        std::vector<Slot> slots_;
        mutable std::mutex mutex_;
        std::uint32_t generation_ = 0;
        std::size_t active_attempts_ = 0;
        std::size_t joined_carriers_ = 0;
        std::size_t next_trial_path_ = 0;
        bool running_ = false;
    };
}
