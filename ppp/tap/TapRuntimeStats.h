#pragma once

namespace ppp::tap {

struct TapRuntimeStats final {
    bool vnet_header = false;
    bool gso_merge_active = false;
};

} // namespace ppp::tap
