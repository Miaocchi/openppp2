#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ppp::app::client::xtcp {

class XtcpIngressBudget final {
public:
    XtcpIngressBudget(std::size_t max_items, std::size_t max_bytes) noexcept
        : max_items_(max_items), max_bytes_(max_bytes) {}

    bool TryReserve(std::size_t bytes) noexcept {
        if (!running_ || !ready_ || bytes == 0 ||
            items_ >= max_items_ || bytes > max_bytes_ - used_bytes_) {
            return false;
        }
        ++items_;
        used_bytes_ += bytes;
        return true;
    }

    void Release(std::size_t bytes) noexcept {
        if (items_ > 0) {
            --items_;
        }
        used_bytes_ = bytes <= used_bytes_ ? used_bytes_ - bytes : 0;
    }

    std::uint64_t Start() noexcept {
        ++generation_;
        if (generation_ == 0) {
            ++generation_;
        }
        running_ = true;
        ready_ = false;
        items_ = 0;
        used_bytes_ = 0;
        return generation_;
    }

    void MarkReady(std::uint64_t generation) noexcept {
        if (running_ && generation == generation_) {
            ready_ = true;
        }
    }

    void Stop() noexcept {
        running_ = false;
        ready_ = false;
        items_ = 0;
        used_bytes_ = 0;
    }

    bool Accepts(std::uint64_t generation) const noexcept {
        return running_ && generation != 0 && generation == generation_;
    }

    bool IsReady() const noexcept { return running_ && ready_; }
    bool IsRunning() const noexcept { return running_; }
    std::uint64_t Generation() const noexcept { return generation_; }
    std::size_t Items() const noexcept { return items_; }
    std::size_t Bytes() const noexcept { return used_bytes_; }

private:
    const std::size_t max_items_;
    const std::size_t max_bytes_;
    std::size_t items_ = 0;
    std::size_t used_bytes_ = 0;
    std::uint64_t generation_ = 0;
    bool running_ = false;
    bool ready_ = false;
};

inline bool ShouldConsumeXtcpPacket(
    bool dispatch_consumed,
    bool xtcp_mode,
    bool ipv4_tcp) noexcept {
    return dispatch_consumed || (xtcp_mode && ipv4_tcp);
}

inline bool IsRegisteredExternalLoopback(
    bool is_loopback,
    std::uint16_t source_port,
    std::uint16_t registered_port,
    std::uint64_t generation,
    std::uint64_t registered_generation) noexcept {
    return is_loopback && source_port != 0 &&
        source_port == registered_port && generation != 0 &&
        generation == registered_generation;
}

constexpr std::array<unsigned char, 4> IPv4AddressBytes(
    std::uint32_t network_address) noexcept {
    return {{
        static_cast<unsigned char>(network_address >> 24),
        static_cast<unsigned char>(network_address >> 16),
        static_cast<unsigned char>(network_address >> 8),
        static_cast<unsigned char>(network_address),
    }};
}

constexpr bool IsFragmentedIPv4(
    std::uint16_t fragment_offset,
    std::uint8_t flags) noexcept {
    constexpr std::uint8_t kMoreFragments = 0x01;
    return fragment_offset != 0 || (flags & kMoreFragments) != 0;
}

} // namespace ppp::app::client::xtcp
