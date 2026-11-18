#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ppp::app::multipath {

struct RuntimePathState final {
    std::string id;
    std::string state;
    std::uint32_t carriers = 0;
    std::uint32_t reconnect_attempts = 0;
    std::string error_code;
    std::optional<std::uint64_t> rtt_ms;
    std::optional<std::uint64_t> rx_bytes;
    std::optional<std::uint64_t> tx_bytes;
    std::optional<std::uint64_t> inflight_bytes;
    std::optional<std::uint64_t> retransmit_bytes;

    bool operator==(const RuntimePathState& other) const noexcept {
        return id == other.id && state == other.state && carriers == other.carriers &&
            reconnect_attempts == other.reconnect_attempts && error_code == other.error_code &&
            rtt_ms == other.rtt_ms && rx_bytes == other.rx_bytes && tx_bytes == other.tx_bytes &&
            inflight_bytes == other.inflight_bytes && retransmit_bytes == other.retransmit_bytes;
    }
};

struct RuntimeState final {
    bool enabled = false;
    std::string policy;
    std::string state;
    std::uint32_t active_paths = 0;
    std::optional<std::uint64_t> remaining_grace_ms;
    std::vector<std::string> capabilities;
    std::vector<RuntimePathState> paths;

    bool operator==(const RuntimeState& other) const noexcept {
        return enabled == other.enabled && policy == other.policy && state == other.state &&
            active_paths == other.active_paths && remaining_grace_ms == other.remaining_grace_ms &&
            capabilities == other.capabilities && paths == other.paths;
    }
};

}
