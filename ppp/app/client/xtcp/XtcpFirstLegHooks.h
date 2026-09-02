#pragma once

#include <cstdint>

namespace ppp::app::client::xtcp {

class XtcpFirstLegHooks {
public:
    virtual ~XtcpFirstLegHooks() noexcept = default;

    virtual void OnFirstLegReady(
        std::uint64_t runtime_generation,
        std::uint64_t flow_generation) noexcept = 0;
    virtual void OnFirstLegClosed(
        std::uint64_t runtime_generation,
        std::uint64_t flow_generation) noexcept = 0;
};

} // namespace ppp::app::client::xtcp
