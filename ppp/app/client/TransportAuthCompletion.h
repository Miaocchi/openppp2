#pragma once

#include <ppp/diagnostics/Error.h>

#include <atomic>

namespace ppp::app::client {

class TransportAuthCompletion final {
public:
    enum class State : int {
        Pending,
        Succeeded,
        Failed,
        TimedOut,
    };

    bool TryComplete(bool success, ppp::diagnostics::ErrorCode error) noexcept {
        error_.store(static_cast<int>(error), std::memory_order_relaxed);
        State expected = State::Pending;
        return state_.compare_exchange_strong(expected,
            success ? State::Succeeded : State::Failed,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool TryTimeout() noexcept {
        State expected = State::Pending;
        return state_.compare_exchange_strong(expected, State::TimedOut,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    State GetState() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    ppp::diagnostics::ErrorCode GetError() const noexcept {
        return static_cast<ppp::diagnostics::ErrorCode>(
            error_.load(std::memory_order_relaxed));
    }

private:
    std::atomic<State> state_{ State::Pending };
    std::atomic<int> error_{
        static_cast<int>(ppp::diagnostics::ErrorCode::SessionAuthFailed) };
};

} // namespace ppp::app::client
