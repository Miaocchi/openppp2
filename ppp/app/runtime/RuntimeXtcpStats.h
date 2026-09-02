#pragma once

#include <cstdint>

namespace ppp {
    namespace app {
        namespace runtime {

            /**
             * @brief Monotonic counters (and one gauge) exported by the XTCP
             *        userspace TCP stack runtime. All counters are cumulative
             *        since the current runtime generation started.
             */
            struct RuntimeXtcpStats final {
                std::uint64_t ingress_submitted = 0;      /**< IPv4/TCP datagrams accepted into the ingress queue */
                std::uint64_t ingress_dropped = 0;        /**< Datagrams rejected before queueing (not ready / budget) */
                std::uint64_t ingress_injected = 0;       /**< Datagrams handed to the XTCP stack */
                std::uint64_t flows_opened = 0;           /**< Flows created from inbound SYNs */
                std::uint64_t flows_closed = 0;           /**< Flows torn down (any reason) */
                std::uint64_t flows_active = 0;           /**< Gauge: flows currently tracked */
                std::uint64_t timer_polls = 0;            /**< PollAckTimers sweeps executed */
                std::uint64_t timer_events = 0;           /**< Timer events fired across all sweeps */
                std::uint64_t output_packets = 0;         /**< L3 packets emitted by the stack */
                std::uint64_t output_bytes = 0;           /**< L3 bytes emitted by the stack */
                std::uint64_t connector_read_bytes = 0;   /**< Bytes read from second-leg connectors (app -> stack) */
                std::uint64_t connector_written_bytes = 0;/**< Bytes written to second-leg connectors (stack -> app) */
                std::uint64_t queued_bytes = 0;           /**< Gauge: bytes in the runtime->connector bridge queue */
                std::uint64_t queued_bytes_highwater = 0; /**< Gauge peak of queued_bytes since start */
            };

        }
    }
}
