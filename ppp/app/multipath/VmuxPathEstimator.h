#pragma once

#include "PathControlTypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>

namespace ppp::app::multipath {

struct VmuxPathSample final {
    std::optional<std::uint64_t> delivery_rate_bytes_per_second;
    std::optional<std::uint64_t> srtt_ms;
    std::optional<std::uint64_t> min_rtt_ms;
    std::optional<std::uint64_t> wire_inflight_bytes;
    std::optional<std::uint64_t> local_queue_bytes;
    std::optional<std::uint64_t> retransmission_bytes;
    std::uint64_t timestamp_ms = 0;
    std::uint64_t epoch = 0;
};

struct VmuxPathEstimatorConfig final {
    std::size_t round_max_samples = 8;
    std::uint64_t metric_expiry_ms = 10000;
};

/** Pure VMUX metric estimator. It has no transport or clock dependencies. */
class VmuxPathEstimator final {
public:
    explicit VmuxPathEstimator(VmuxPathEstimatorConfig config = {}) noexcept
        : config_(config) {
        if (config_.round_max_samples == 0) config_.round_max_samples = 1;
    }

    PathTelemetry Observe(const VmuxPathSample& sample) noexcept {
        if (!initialized_ || sample.epoch != epoch_) {
            Reset(sample.epoch);
        }
        if (initialized_ && sample.timestamp_ms < last_timestamp_ms_) return Snapshot(last_timestamp_ms_);
        initialized_ = true;
        last_timestamp_ms_ = sample.timestamp_ms;
        if (sample.delivery_rate_bytes_per_second) {
            rate_samples_.push_back(*sample.delivery_rate_bytes_per_second);
            while (rate_samples_.size() > config_.round_max_samples) rate_samples_.pop_front();
        }
        if (sample.min_rtt_ms) min_rtt_ms_ = *sample.min_rtt_ms;
        if (sample.srtt_ms) srtt_ms_ = *sample.srtt_ms;
        if (sample.wire_inflight_bytes) wire_inflight_bytes_ = *sample.wire_inflight_bytes;
        if (sample.local_queue_bytes) local_queue_bytes_ = *sample.local_queue_bytes;
        if (sample.retransmission_bytes) retransmission_bytes_ = *sample.retransmission_bytes;
        if (sample.delivery_rate_bytes_per_second || sample.srtt_ms || sample.min_rtt_ms ||
            sample.wire_inflight_bytes || sample.local_queue_bytes || sample.retransmission_bytes)
            last_metric_timestamp_ms_ = sample.timestamp_ms;
        ++sample_count_;
        return Snapshot(sample.timestamp_ms);
    }

    PathTelemetry SnapshotAt(std::uint64_t now_ms) const noexcept {
        return Snapshot(now_ms);
    }

    void Reset(std::uint64_t epoch = 0) noexcept {
        epoch_ = epoch;
        initialized_ = false;
        last_timestamp_ms_ = 0;
        last_metric_timestamp_ms_ = 0;
        sample_count_ = 0;
        rate_samples_.clear();
        min_rtt_ms_.reset(); srtt_ms_.reset(); wire_inflight_bytes_.reset();
        local_queue_bytes_.reset(); retransmission_bytes_.reset();
    }

private:
    static std::uint64_t SaturatingProduct(std::uint64_t left,
                                           std::uint64_t right) noexcept {
        if (left == 0 || right == 0) return 0;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        return left > maximum / right ? maximum : left * right;
    }

    PathTelemetry Snapshot(std::uint64_t now_ms) const noexcept {
        PathTelemetry out;
        if (!initialized_ || now_ms < last_metric_timestamp_ms_ ||
            now_ms - last_metric_timestamp_ms_ > config_.metric_expiry_ms) return out;
        out.sample_count = sample_count_;
        out.updated_ms = last_metric_timestamp_ms_;
        if (!rate_samples_.empty()) {
            out.current_rate_bytes_per_second = *std::max_element(rate_samples_.begin(), rate_samples_.end());
            out.capacity_rate_bytes_per_second = out.current_rate_bytes_per_second;
        }
        if (min_rtt_ms_) out.min_rtt_ms = *min_rtt_ms_;
        if (srtt_ms_) out.srtt_ms = *srtt_ms_;
        if (srtt_ms_ && min_rtt_ms_) out.queue_delay_ms = *srtt_ms_ > *min_rtt_ms_ ? *srtt_ms_ - *min_rtt_ms_ : 0;
        if (wire_inflight_bytes_) out.wire_inflight_bytes = *wire_inflight_bytes_;
        if (local_queue_bytes_) out.local_queue_bytes = *local_queue_bytes_;
        if (retransmission_bytes_) out.retransmission_bytes = *retransmission_bytes_;
        if (out.current_rate_bytes_per_second && srtt_ms_)
            out.bdp_bytes = SaturatingProduct(out.current_rate_bytes_per_second, *srtt_ms_) / 1000;
        const bool any = !rate_samples_.empty() || min_rtt_ms_ || srtt_ms_ || wire_inflight_bytes_ || local_queue_bytes_ || retransmission_bytes_;
        out.has_sample = any;
        const unsigned known = static_cast<unsigned>(!rate_samples_.empty()) + static_cast<unsigned>(min_rtt_ms_.has_value()) + static_cast<unsigned>(srtt_ms_.has_value()) + static_cast<unsigned>(wire_inflight_bytes_.has_value()) + static_cast<unsigned>(local_queue_bytes_.has_value());
        out.confidence = static_cast<std::uint8_t>((known * 100u) / 5u);
        out.at_capacity = out.bdp_bytes != 0 && (out.wire_inflight_bytes >= out.bdp_bytes || out.local_queue_bytes >= out.bdp_bytes);
        return out;
    }

    VmuxPathEstimatorConfig config_;
    bool initialized_ = false;
    std::uint64_t epoch_ = 0, last_timestamp_ms_ = 0, last_metric_timestamp_ms_ = 0, sample_count_ = 0;
    std::deque<std::uint64_t> rate_samples_;
    std::optional<std::uint64_t> min_rtt_ms_, srtt_ms_, wire_inflight_bytes_, local_queue_bytes_, retransmission_bytes_;
};

} // namespace ppp::app::multipath
