#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace ppp::app::mux {

// A peer that has never returned a probe can be running an older version.
// Only a missed reply after prior support is evidence of path failure.
class MuxIntentPathProbe final {
public:
    static constexpr std::uint64_t IntervalMs = 500;

    bool Due(std::uint64_t now) const noexcept {
        return sent_at_ == 0 || now - sent_at_ >= IntervalMs;
    }

    void Sent(std::uint64_t now) noexcept {
        sent_at_ = now;
        requests_[next_request_] = now;
        next_request_ = (next_request_ + 1) % requests_.size();
        if (pending_since_ == 0) pending_since_ = now;
    }

    bool Acknowledge(std::uint32_t token, std::uint64_t now) noexcept {
        const auto request = std::find_if(requests_.begin(), requests_.end(), [&](std::uint64_t sent) {
            return sent != 0 && sent > acknowledged_through_ && sent < now &&
                static_cast<std::uint32_t>(sent) == token;
        });
        if (request == requests_.end()) return false;

        rtt_ms_ = now - *request;
        min_rtt_ms_ = min_rtt_ms_ == 0 ? rtt_ms_ : std::min(min_rtt_ms_, rtt_ms_);
        acknowledged_through_ = *request;
        pending_since_ = 0;
        for (const auto sent : requests_) {
            if (sent > acknowledged_through_ &&
                (pending_since_ == 0 || sent < pending_since_)) {
                pending_since_ = sent;
            }
        }
        samples_[next_sample_] = {now, rtt_ms_};
        next_sample_ = (next_sample_ + 1) % samples_.size();
        last_reply_at_ = now;
        timeout_reported_ = false;
        return true;
    }

    bool TimedOut(std::uint64_t now, std::uint64_t delivered_at = 0) const noexcept {
        // An ACK for payload sent on this carrier also proves forward progress.
        const auto progress = std::max({pending_since_, last_reply_at_, delivered_at});
        return rtt_ms_ != 0 && pending_since_ != 0 && now >= progress &&
            now - progress >= std::max<std::uint64_t>(500, 3 * rtt_ms_);
    }

    std::uint64_t RttMs() const noexcept { return rtt_ms_; }
    std::uint64_t MinRttMs() const noexcept { return min_rtt_ms_; }

    std::uint64_t RecentRttMs(std::uint64_t now) const noexcept {
        auto best = rtt_ms_;
        for (const auto& sample : samples_) {
            if (sample.at != 0 && now >= sample.at && now - sample.at <= 2000) {
                best = std::min(best, sample.rtt);
            }
        }
        return best;
    }

    bool ConsumeTimeout(std::uint64_t now, std::uint64_t delivered_at = 0) noexcept {
        if (timeout_reported_ || !TimedOut(now, delivered_at)) return false;
        timeout_reported_ = true;
        return true;
    }

    bool ShouldReplace(std::uint64_t now, std::uint64_t delivered_at,
        bool healthy_sibling = false) const noexcept {
        // A responsive sibling keeps this path available while a stalled TCP
        // socket is replaced. The last carrier gets more time to recover.
        const std::uint64_t grace_ms = healthy_sibling ? 1200 : 3000;
        return timeout_reported_ && TimedOut(now, delivered_at) &&
            now - std::max({pending_since_, last_reply_at_, delivered_at}) >= grace_ms;
    }

private:
    struct Sample { std::uint64_t at = 0, rtt = 0; };
    std::array<Sample, 3> samples_{};
    std::array<std::uint64_t, 8> requests_{};
    std::size_t next_sample_ = 0;
    std::size_t next_request_ = 0;
    std::uint64_t sent_at_ = 0;
    std::uint64_t pending_since_ = 0;
    std::uint64_t acknowledged_through_ = 0;
    std::uint64_t last_reply_at_ = 0;
    std::uint64_t rtt_ms_ = 0;
    std::uint64_t min_rtt_ms_ = 0;
    bool timeout_reported_ = false;
};

}
