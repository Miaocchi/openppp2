#pragma once

#include "PathControlTypes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>

namespace ppp::app::multipath {

enum class PathEstimatorMode { Ewma, Bbr, Kcc };

struct DeliverySample final {
    std::uint64_t delivered_bytes = 0;
    std::uint64_t interval_us = 0;
    std::uint64_t prior_delivered = 0;
    std::uint64_t total_delivered = 0;
    bool is_app_limited = false;
};

class IPathEstimator {
public:
    virtual ~IPathEstimator() = default;
    virtual void OnSample(const DeliverySample& sample, std::uint64_t now_ms) noexcept = 0;
    virtual PathTelemetry GetTelemetry(std::uint64_t now_ms) const noexcept = 0;
    virtual void Reset(std::uint64_t epoch) noexcept = 0;
    virtual bool FullBandwidthReached() const noexcept = 0;
};

class VmuxPathEstimator final : public IPathEstimator {
public:
    explicit VmuxPathEstimator(PathEstimatorMode mode) noexcept : mode_(mode) {}

    void OnSample(const DeliverySample& sample, std::uint64_t now_ms) noexcept override {
        if (sample.interval_us == 0 || sample.delivered_bytes == 0 ||
            sample.prior_delivered >= sample.total_delivered ||
            (sample_count_ != 0 && (now_ms < updated_ms_ ||
                sample.total_delivered <= total_delivered_))) return;

        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        const long double rate = static_cast<long double>(sample.delivered_bytes) *
            1000000.0L / sample.interval_us;
        const auto measured = rate >= static_cast<long double>(maximum)
            ? maximum : static_cast<std::uint64_t>(rate);
        if (measured == 0) return;

        updated_ms_ = now_ms;
        total_delivered_ = sample.total_delivered;
        latest_ = measured;
        ++sample_count_;
        const bool low_app_limited = sample.is_app_limited && measured < capacity_;
        if (!low_app_limited && capacity_ != 0 && now_ms - capacity_updated_ms_ > MetricExpiryMs) {
            ClearModel();
        }
        if (mode_ == PathEstimatorMode::Ewma) {
            if (low_app_limited) return;
            if (capacity_ == 0) {
                capacity_ = measured;
            } else if (measured > capacity_) {
                capacity_ += (measured - capacity_) / 8;
            } else {
                const auto delta = capacity_ - measured;
                capacity_ -= delta / 8 + (delta % 8 != 0);
            }
        } else {
            const bool round_start = sample.prior_delivered >= next_round_delivered_;
            if (round_start) {
                next_round_delivered_ = sample.total_delivered;
                ++round_;
            }
            // Advance the delivery clock even when this ACK contributes no capacity evidence.
            if (low_app_limited) return;
            auto& peak = round_max_[round_ % round_max_.size()];
            if (peak.round != round_) peak = {round_, 0};
            peak.rate = std::max(peak.rate, measured);
            capacity_ = 0;
            for (const auto& entry : round_max_) {
                if (round_ - entry.round < round_max_.size()) {
                    capacity_ = std::max(capacity_, entry.rate);
                }
            }
            if (mode_ == PathEstimatorMode::Kcc && round_start &&
                !sample.is_app_limited && !FullBandwidthReached()) {
                // KCC declares the pipe full after three rounds below 25% growth.
                if (full_bandwidth_ == 0 || (capacity_ >= full_bandwidth_ &&
                    capacity_ - full_bandwidth_ >= std::max<std::uint64_t>(1, full_bandwidth_ / 4))) {
                    full_bandwidth_ = capacity_;
                    plateau_rounds_ = 0;
                } else {
                    ++plateau_rounds_;
                }
            }
        }
        capacity_updated_ms_ = now_ms;
    }

    PathTelemetry GetTelemetry(std::uint64_t now_ms) const noexcept override {
        PathTelemetry telemetry;
        if (sample_count_ == 0 || now_ms < updated_ms_ ||
            now_ms - updated_ms_ > MetricExpiryMs) return telemetry;
        telemetry.has_sample = true;
        telemetry.updated_ms = updated_ms_;
        telemetry.sample_count = sample_count_;
        telemetry.current_rate_bytes_per_second = latest_;
        if (capacity_ != 0 && now_ms >= capacity_updated_ms_ &&
            now_ms - capacity_updated_ms_ <= MetricExpiryMs) {
            telemetry.capacity_rate_bytes_per_second = capacity_;
            telemetry.capacity_updated_ms = capacity_updated_ms_;
        }
        telemetry.confidence = 100;
        return telemetry;
    }

    void Reset(std::uint64_t) noexcept override {
        ClearModel();
        updated_ms_ = 0;
        total_delivered_ = 0;
        latest_ = 0;
        sample_count_ = 0;
    }

    bool FullBandwidthReached() const noexcept override { return plateau_rounds_ >= 3; }

private:
    struct RoundPeak {
        std::uint64_t round = 0;
        std::uint64_t rate = 0;
    };

    void ClearModel() noexcept {
        capacity_ = 0;
        capacity_updated_ms_ = 0;
        next_round_delivered_ = 0;
        round_ = 0;
        round_max_.fill({});
        full_bandwidth_ = 0;
        plateau_rounds_ = 0;
    }

    static constexpr std::uint64_t MetricExpiryMs = 5000;
    PathEstimatorMode mode_;
    std::array<RoundPeak, 10> round_max_{};
    std::uint64_t round_ = 0;
    std::uint64_t next_round_delivered_ = 0;
    std::uint64_t full_bandwidth_ = 0;
    unsigned plateau_rounds_ = 0;
    std::uint64_t updated_ms_ = 0;
    std::uint64_t capacity_updated_ms_ = 0;
    std::uint64_t total_delivered_ = 0;
    std::uint64_t latest_ = 0;
    std::uint64_t capacity_ = 0;
    std::uint64_t sample_count_ = 0;
};

inline std::unique_ptr<IPathEstimator> MakePathEstimator(PathEstimatorMode mode) {
    return std::make_unique<VmuxPathEstimator>(mode);
}

} // namespace ppp::app::multipath
