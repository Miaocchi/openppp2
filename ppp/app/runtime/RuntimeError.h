#pragma once

#include <ppp/stdafx.h>

namespace ppp {
    namespace app {
        namespace runtime {

            struct RuntimeError final {
                uint32_t code = 0;
                ppp::string severity;
                bool retryable = false;
                ppp::string user_message_key;
                ppp::string diagnostic_detail;

                bool HasError() const noexcept {
                    return code != 0 || !diagnostic_detail.empty();
                }
            };

        }
    }
}
