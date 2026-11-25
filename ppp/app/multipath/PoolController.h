#pragma once

#include <cstdint>
#include <algorithm>

namespace ppp::app::multipath {

// All counters are per mux, per sending direction. A controller lives on its mux strand.
struct PoolSample final {
    std::uint64_t now = 0, offered = 0, acked = 0, retransmitted = 0;
    std::uint64_t rtt_ms = 0, queue_delay_ms = 0, topology = 0;
    unsigned live = 0, occupied = 0, floor = 1, limit = 1;
    bool reliable = false, pressure = false, idle = false;
    bool trial_attached = false, trial_ready = false;
    bool reprobe_after_idle = false;
};

enum class PoolAction { Hold, Dial, Keep, Reject, Inconclusive, Shrink };

class PoolController final {
public:
    void DialFailed(std::uint64_t now) noexcept { Finish(now, Cooldown); }

    PoolAction Step(const PoolSample& s) noexcept {
        if (s.reprobe_after_idle) {
            if (s.idle) {
                if (idle_since_ == 0) idle_since_ = s.now;
            } else {
                // A completed idle interval separates independent demands.
                // A rejected trial from the earlier load cannot describe the
                // capacity needed by the next one.
                if (idle_since_ != 0 && s.now - idle_since_ >= IdleShrinkMs) {
                    next_ = std::min(next_, s.now);
                }
                idle_since_ = 0;
            }
        }
        if (phase_ == Phase::Dial || phase_ == Phase::Warm || phase_ == Phase::Measure) {
            // Only the requested attach may change membership inside a paired window.
            if (s.now - requested_ >= TrialTimeout || s.topology != before_.topology +
                (s.trial_attached ? 1u : 0u) || s.live != before_.live + (s.trial_ready ? 1u : 0u))
                return Complete(s.now, PoolAction::Inconclusive);
            if (!s.trial_ready) return PoolAction::Hold;
            // No VMUX ACK protocol: preserve backlog-based growth without claiming measured gain.
            if (!s.reliable) return Complete(s.now, PoolAction::Keep);
            if (phase_ == Phase::Dial) {
                phase_ = Phase::Warm;
                mark_ = s;
            } else if (phase_ == Phase::Warm && s.now - mark_.now >= window_) {
                phase_ = Phase::Measure;
                mark_ = s;
            } else if (phase_ == Phase::Measure && s.now - mark_.now >= window_) {
                const auto offer = Rate(s.offered, mark_.offered, s.now - mark_.now);
                const auto rate = Rate(s.acked, mark_.acked, s.now - mark_.now);
                // Backpressured admission can rise when service improves. A drained queue
                // without comparable arrivals, or a window without ACKs, is not evidence.
                if (offer == 0 || rate == 0 || (!s.pressure &&
                    (offer < base_offer_ * 0.8 || offer > base_offer_ * 1.25)))
                    return Complete(s.now, PoolAction::Inconclusive);
                const auto cost = Rate(s.retransmitted, mark_.retransmitted, s.now - mark_.now) / rate;
                const bool useful = rate >= base_rate_ + std::max(1024.0, base_rate_ * 0.05);
                const bool affordable = s.queue_delay_ms <= before_.queue_delay_ms +
                    std::max<std::uint64_t>(10, before_.rtt_ms / 4) && cost <= base_cost_ + 0.05;
                return Complete(s.now, useful && affordable ? PoolAction::Keep : PoolAction::Reject);
            }
            return PoolAction::Hold;
        }
        if ((s.now < next_ && !(s.reprobe_after_idle && s.idle)) || !s.live || s.occupied != s.live) {
            phase_ = Phase::Idle;
            return PoolAction::Hold;
        }
        if (s.pressure && s.occupied < s.limit) {
            if (phase_ != Phase::Baseline || mark_.topology != s.topology) {
                mark_ = s;
                window_ = std::clamp<std::uint64_t>(s.rtt_ms, 500, 2500) * 4;
                phase_ = Phase::Baseline;
            } else if (s.now - mark_.now >= window_) {
                const auto dt = s.now - mark_.now;
                base_rate_ = Rate(s.acked, mark_.acked, dt);
                base_offer_ = Rate(s.offered, mark_.offered, dt);
                base_cost_ = base_rate_ ? Rate(s.retransmitted, mark_.retransmitted, dt) / base_rate_ : 0;
                mark_ = s;
                if (!s.reliable || (base_rate_ > 0 && base_offer_ > 0)) {
                    before_ = s;
                    phase_ = Phase::Dial;
                    requested_ = s.now;
                    return PoolAction::Dial;
                }
            }
        } else if (s.idle && s.live > s.floor) {
            if (phase_ != Phase::Low || mark_.topology != s.topology || mark_.offered != s.offered) {
                mark_ = s;
                phase_ = Phase::Low;
            } else if (s.now - mark_.now >= IdleShrinkMs) return Complete(s.now, PoolAction::Shrink);
        } else phase_ = Phase::Idle;
        return PoolAction::Hold;
    }

private:
    enum class Phase { Idle, Baseline, Dial, Warm, Measure, Low } phase_ = Phase::Idle;
    static constexpr std::uint64_t Cooldown = 3000, TrialTimeout = 60000, IdleShrinkMs = 5000;
    PoolSample before_, mark_;
    std::uint64_t next_ = 0, requested_ = 0, window_ = 0;
    std::uint64_t idle_since_ = 0;
    double base_offer_ = 0, base_rate_ = 0, base_cost_ = 0;

    static double Rate(std::uint64_t end, std::uint64_t begin, std::uint64_t ms) noexcept {
        return end > begin && ms ? static_cast<double>(end - begin) * 1000.0 / ms : 0;
    }
    void Finish(std::uint64_t now, std::uint64_t delay) noexcept {
        phase_ = Phase::Idle;
        next_ = now + delay;
    }
    PoolAction Complete(std::uint64_t now, PoolAction action) noexcept {
        // Freeze a measured plateau longer than an inconclusive or failed probe.
        Finish(now, action == PoolAction::Reject ? TrialTimeout : Cooldown);
        return action;
    }
};

} // namespace ppp::app::multipath
