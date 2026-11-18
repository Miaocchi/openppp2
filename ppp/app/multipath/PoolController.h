#pragma once

#include <cstdint>
#include <optional>

namespace ppp::app::multipath {

// Inputs are snapshots; PoolController has no ownership of carriers.
struct PoolDemand final {
    std::uint64_t offered_rate_bytes_per_second = 0;
    std::uint32_t active_carriers = 0;
    std::uint32_t warming_carriers = 0;
    bool all_at_capacity = false;
};

struct PoolCapacity final {
    std::uint64_t aggregate_rate_bytes_per_second = 0;
    std::uint64_t marginal_gain_bytes_per_second = 0;
    bool carrier_pressure = false;
    std::uint64_t warmup_until_ms = 0;
    std::uint64_t marginal_gain_window_bytes_per_second = 1;
    std::uint32_t floor = 1;
    std::uint32_t soft_limit = 0;
    std::uint32_t hard_limit = 0;
    std::optional<bool> aggregate_capacity_shortage;
};

enum class PoolDecision : std::uint8_t { Hold, Add, Retire };

struct PoolDecisionResult final {
    PoolDecision decision = PoolDecision::Hold;
    bool single_carrier_pressure = false;
    bool aggregate_capacity_shortage = false;
    bool in_warmup = false;
    bool marginal_gain_sufficient = false;
};

class PoolController final {
public:
    static PoolDecisionResult Decide(const PoolDemand& demand,
                                     const PoolCapacity& capacity,
                                     std::uint64_t now_ms) noexcept {
        const std::uint32_t floor = capacity.floor;
        const std::uint32_t soft = capacity.soft_limit < floor ? floor : capacity.soft_limit;
        const std::uint32_t hard = capacity.hard_limit < soft ? soft : capacity.hard_limit;
        const bool pressure = capacity.carrier_pressure;
        const bool shortage = capacity.aggregate_capacity_shortage.value_or(
            demand.offered_rate_bytes_per_second > capacity.aggregate_rate_bytes_per_second);
        const bool warmup = demand.warming_carriers != 0 && now_ms < capacity.warmup_until_ms;
        const bool gain = capacity.marginal_gain_bytes_per_second >=
                          capacity.marginal_gain_window_bytes_per_second;

        PoolDecisionResult result;
        result.single_carrier_pressure = pressure;
        result.aggregate_capacity_shortage = shortage;
        result.in_warmup = warmup;
        result.marginal_gain_sufficient = gain;

        // A new carrier is justified by aggregate shortage (or pressure when
        // it leaves aggregate capacity short), never by all_at_capacity alone.
        if (demand.active_carriers + demand.warming_carriers < hard &&
            (shortage || pressure) && !warmup && gain) {
            result.decision = PoolDecision::Add;
            return result;
        }
        if (demand.active_carriers > floor && !shortage && !pressure &&
            demand.warming_carriers == 0 && demand.active_carriers > soft) {
            result.decision = PoolDecision::Retire;
        }
        return result;
    }
};

} // namespace ppp::app::multipath
