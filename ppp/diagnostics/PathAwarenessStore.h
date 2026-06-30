#pragma once

#include <ppp/stdafx.h>

namespace ppp {
    namespace diagnostics {
        class PathAwarenessStore final {
        public:
            static bool                             SetLocalSnapshot(const ppp::string& json) noexcept;
            static ppp::string                      GetLocalSnapshot() noexcept;
            static ppp::string                      GetLocalSnapshotWithNative() noexcept;

            static bool                             SetPeerSnapshot(const ppp::string& json) noexcept;
            static ppp::string                      GetPeerSnapshot() noexcept;

            static void                             RecordConnectObservation(
                const char* role,
                int path_id,
                const char* transport,
                int64_t connect_ms,
                int64_t handshake_ms,
                int error_code) noexcept;
            static void                             RecordTrafficSnapshot(
                int path_id,
                const char* transport,
                uint64_t tx_bytes,
                uint64_t rx_bytes,
                uint64_t in_bytes,
                uint64_t out_bytes) noexcept;

            static ppp::string                      GetNativeSnapshot() noexcept;
            static void                             Clear() noexcept;
        };
    }
}
