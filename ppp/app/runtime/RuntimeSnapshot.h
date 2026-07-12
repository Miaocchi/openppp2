#pragma once

#include <ppp/app/runtime/RuntimeError.h>
#include <ppp/app/runtime/RuntimePhase.h>

namespace ppp {
    namespace app {
        namespace runtime {

            struct RuntimeSnapshot final {
                static constexpr uint32_t SchemaVersion = 1;

                uint32_t schema_version = SchemaVersion;
                uint64_t generation = 0;
                uint64_t monotonic_ms = 0;
                RuntimePhase phase = RuntimePhase::Idle;
                ppp::string role;
                ppp::string server;
                ppp::string transport;
                ppp::string requested_mux_mode;
                ppp::string effective_mux_mode;
                ppp::string mux_fallback_reason;
                ppp::string p2p_state;
                ppp::string effective_path;
                RuntimeError last_error;
            };

        }
    }
}
