#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ppp::app::multipath {

struct RuntimePathState final {
    std::string id;
    std::string state;
    std::string control_state;
    std::uint32_t carriers = 0;
    std::uint32_t reconnect_attempts = 0;
    std::string error_code;
    std::optional<std::uint64_t> rtt_ms;
    std::optional<std::uint64_t> rx_bytes;
    std::optional<std::uint64_t> tx_bytes;
    std::optional<std::uint64_t> inflight_bytes;
    std::optional<std::uint64_t> retransmit_bytes;
    std::optional<std::uint64_t> tx_unique_payload_bytes;
    std::optional<std::uint64_t> rx_unique_payload_bytes;
    std::optional<std::uint64_t> capacity_rate_bytes_per_second;
    std::optional<std::uint64_t> queue_delay_ms;
    std::optional<std::uint64_t> bdp_bytes;
    std::optional<std::uint64_t> confidence;
    bool at_capacity = false;

    bool operator==(const RuntimePathState& other) const noexcept {
        return id == other.id && state == other.state && control_state == other.control_state && carriers == other.carriers &&
            reconnect_attempts == other.reconnect_attempts && error_code == other.error_code &&
            rtt_ms == other.rtt_ms && rx_bytes == other.rx_bytes && tx_bytes == other.tx_bytes &&
            inflight_bytes == other.inflight_bytes && retransmit_bytes == other.retransmit_bytes &&
            tx_unique_payload_bytes == other.tx_unique_payload_bytes &&
            rx_unique_payload_bytes == other.rx_unique_payload_bytes &&
            capacity_rate_bytes_per_second == other.capacity_rate_bytes_per_second &&
            queue_delay_ms == other.queue_delay_ms && bdp_bytes == other.bdp_bytes &&
            confidence == other.confidence && at_capacity == other.at_capacity;
    }
};

struct RuntimeCarrierState final {
    std::string path_id;
    std::uint16_t link_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t tx_bytes = 0;
    std::uint64_t rx_bytes = 0;
    std::uint64_t tx_unique_payload_bytes = 0;
    std::uint64_t rx_unique_payload_bytes = 0;
    std::uint64_t retransmit_bytes = 0;
    std::uint64_t retransmit_events = 0;
    std::uint64_t queued_bytes = 0;
    std::uint64_t wire_inflight_bytes = 0;
    std::uint64_t send_credit_bytes = 0;
    std::uint64_t rtt_ms = 0;
    std::uint64_t jitter_ms = 0;
    std::uint64_t queue_delay_ms = 0;
    std::uint64_t delivery_rate_bytes_per_second = 0;
    std::uint64_t capacity_rate_bytes_per_second = 0;
    std::uint64_t scheduling_weight = 0;
    std::uint64_t confidence = 0;
    bool metrics_known = false;
    bool app_limited = false;

    bool operator==(const RuntimeCarrierState& other) const noexcept {
        return path_id == other.path_id && link_id == other.link_id && generation == other.generation &&
            tx_bytes == other.tx_bytes && rx_bytes == other.rx_bytes &&
            tx_unique_payload_bytes == other.tx_unique_payload_bytes &&
            rx_unique_payload_bytes == other.rx_unique_payload_bytes &&
            retransmit_bytes == other.retransmit_bytes && retransmit_events == other.retransmit_events &&
            queued_bytes == other.queued_bytes && wire_inflight_bytes == other.wire_inflight_bytes &&
            send_credit_bytes == other.send_credit_bytes && rtt_ms == other.rtt_ms &&
            jitter_ms == other.jitter_ms && queue_delay_ms == other.queue_delay_ms &&
            delivery_rate_bytes_per_second == other.delivery_rate_bytes_per_second &&
            capacity_rate_bytes_per_second == other.capacity_rate_bytes_per_second &&
            scheduling_weight == other.scheduling_weight && confidence == other.confidence &&
            metrics_known == other.metrics_known && app_limited == other.app_limited;
    }
};

struct RuntimeState final {
    bool enabled = false;
    std::string policy;
    std::string state;
    std::uint32_t active_paths = 0;
    std::optional<std::uint64_t> remaining_grace_ms;
    std::vector<std::string> capabilities;
    std::optional<std::uint64_t> vmux_session_id;
    std::optional<std::uint64_t> reorder_events;
    std::optional<std::uint64_t> reorder_buffered_bytes;
    std::vector<RuntimePathState> paths;
    std::vector<RuntimeCarrierState> links;

    bool operator==(const RuntimeState& other) const noexcept {
        return enabled == other.enabled && policy == other.policy && state == other.state &&
            active_paths == other.active_paths && remaining_grace_ms == other.remaining_grace_ms &&
            vmux_session_id == other.vmux_session_id && reorder_events == other.reorder_events &&
            reorder_buffered_bytes == other.reorder_buffered_bytes &&
            capabilities == other.capabilities && paths == other.paths && links == other.links;
    }
};

}
