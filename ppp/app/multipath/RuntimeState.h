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
    std::uint64_t scheduler_selection_count = 0;
    std::uint64_t scheduler_probe_selection_count = 0;
    std::uint64_t scheduler_probe_bytes = 0;
    std::uint64_t scheduler_probe_remaining_bytes = 0;
    std::uint64_t scheduler_last_selection_sequence = 0;
    std::uint64_t scheduler_last_payload_bytes = 0;
    std::uint64_t scheduler_last_send_credit_bytes = 0;
    std::uint64_t scheduler_last_queue_bytes = 0;
    std::uint64_t scheduler_last_wire_inflight_bytes = 0;
    std::uint64_t scheduler_last_probe_budget_bytes = 0;
    bool scheduler_last_probe = false;
    std::uint64_t scheduler_candidate_observation_count = 0;
    std::uint64_t scheduler_last_observation_sequence = 0;
    std::uint64_t scheduler_last_observed_send_credit_bytes = 0;
    std::uint64_t scheduler_last_observed_queue_bytes = 0;
    std::uint64_t scheduler_last_observed_wire_inflight_bytes = 0;
    std::uint64_t scheduler_last_observed_probe_remaining_bytes = 0;

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
            metrics_known == other.metrics_known && app_limited == other.app_limited &&
            scheduler_selection_count == other.scheduler_selection_count &&
            scheduler_probe_selection_count == other.scheduler_probe_selection_count &&
            scheduler_probe_bytes == other.scheduler_probe_bytes &&
            scheduler_probe_remaining_bytes == other.scheduler_probe_remaining_bytes &&
            scheduler_last_selection_sequence == other.scheduler_last_selection_sequence &&
            scheduler_last_payload_bytes == other.scheduler_last_payload_bytes &&
            scheduler_last_send_credit_bytes == other.scheduler_last_send_credit_bytes &&
            scheduler_last_queue_bytes == other.scheduler_last_queue_bytes &&
            scheduler_last_wire_inflight_bytes == other.scheduler_last_wire_inflight_bytes &&
            scheduler_last_probe_budget_bytes == other.scheduler_last_probe_budget_bytes &&
            scheduler_last_probe == other.scheduler_last_probe &&
            scheduler_candidate_observation_count == other.scheduler_candidate_observation_count &&
            scheduler_last_observation_sequence == other.scheduler_last_observation_sequence &&
            scheduler_last_observed_send_credit_bytes == other.scheduler_last_observed_send_credit_bytes &&
            scheduler_last_observed_queue_bytes == other.scheduler_last_observed_queue_bytes &&
            scheduler_last_observed_wire_inflight_bytes == other.scheduler_last_observed_wire_inflight_bytes &&
            scheduler_last_observed_probe_remaining_bytes == other.scheduler_last_observed_probe_remaining_bytes;
    }
};

struct IntentRuntimeStats final {
    std::uint64_t latency = 0;
    std::uint64_t bandwidth = 0;
    std::uint64_t unknown = 0;
    std::uint64_t intent_flip_count = 0;
    std::uint64_t credit_fallback_count = 0;

    bool RecordCreditFallback(bool preferred_out_of_credit, bool alternate_selected) noexcept {
        if (!preferred_out_of_credit || !alternate_selected) return false;
        ++credit_fallback_count;
        return true;
    }

    bool operator==(const IntentRuntimeStats& other) const noexcept {
        return latency == other.latency && bandwidth == other.bandwidth &&
            unknown == other.unknown && intent_flip_count == other.intent_flip_count &&
            credit_fallback_count == other.credit_fallback_count;
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
    IntentRuntimeStats intent;

    bool operator==(const RuntimeState& other) const noexcept {
        return enabled == other.enabled && policy == other.policy && state == other.state &&
            active_paths == other.active_paths && remaining_grace_ms == other.remaining_grace_ms &&
            vmux_session_id == other.vmux_session_id && reorder_events == other.reorder_events &&
            reorder_buffered_bytes == other.reorder_buffered_bytes &&
            capabilities == other.capabilities && paths == other.paths && links == other.links &&
            intent == other.intent;
    }
};

}
