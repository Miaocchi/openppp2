#pragma once

#include <ppp/app/runtime/RuntimeSnapshot.h>

namespace ppp {
    namespace app {
        namespace runtime {

            ppp::string SerializeRuntimeSnapshot(const RuntimeSnapshot& snapshot) noexcept;
            bool ParseRuntimeSnapshot(const ppp::string& json, RuntimeSnapshot& snapshot) noexcept;

        }
    }
}
