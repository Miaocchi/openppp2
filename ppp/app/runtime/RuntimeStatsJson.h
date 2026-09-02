#pragma once

#include <ppp/app/runtime/RuntimeSnapshotJson.h>
#include <ppp/app/runtime/RuntimeXtcpStats.h>
#include <ppp/tap/TapRuntimeStats.h>

#include <json/json.h>

#include <cstdint>
#include <string>

namespace ppp {
    namespace app {
        namespace runtime {

            struct RuntimeLinkStats final {
                double quality_percent = 100.0;
                std::string grade = "Unknown";
                std::uint64_t error_count = 0;
                std::uint64_t success_count = 0;
            };

            struct RuntimeStatsSample final {
                static constexpr std::uint32_t SchemaVersion = 1;

                std::uint64_t monotonic_ms = 0;
                std::uint64_t rx_bytes = 0;
                std::uint64_t tx_bytes = 0;
                RuntimeLinkStats link;
                RuntimeSnapshot runtime;
                std::string requested_tcp_stack;
                std::string active_tcp_stack;
                bool has_tap_linux = false;
                ppp::tap::TapRuntimeStats tap_linux;
                bool has_xtcp = false;
                RuntimeXtcpStats xtcp;
            };

            inline std::string SerializeRuntimeStats(
                const RuntimeStatsSample& sample) noexcept {
                Json::Value root(Json::objectValue);
                root["type"] = "ppp-stats";
                root["version"] = RuntimeStatsSample::SchemaVersion;
                root["monotonic_ms"] = Json::UInt64(sample.monotonic_ms);
                root["rx_bytes"] = Json::UInt64(sample.rx_bytes);
                root["tx_bytes"] = Json::UInt64(sample.tx_bytes);

                Json::Value link(Json::objectValue);
                link["quality_percent"] = sample.link.quality_percent;
                link["grade"] = detail::ToRuntimeJsonString(sample.link.grade);
                link["error_count"] = Json::UInt64(sample.link.error_count);
                link["success_count"] = Json::UInt64(sample.link.success_count);
                root["link"] = std::move(link);

                if (!sample.requested_tcp_stack.empty() && !sample.active_tcp_stack.empty()) {
                    Json::Value tcp_stack(Json::objectValue);
                    tcp_stack["requested"] = detail::ToRuntimeJsonString(sample.requested_tcp_stack);
                    tcp_stack["active"] = detail::ToRuntimeJsonString(sample.active_tcp_stack);
                    root["tcp_stack"] = std::move(tcp_stack);
                }

                if (sample.has_tap_linux) {
                    Json::Value tap_linux(Json::objectValue);
                    tap_linux["vnet_header"] = sample.tap_linux.vnet_header;
                    tap_linux["gso_merge_active"] = sample.tap_linux.gso_merge_active;
                    root["tap_linux"] = std::move(tap_linux);
                }

                if (sample.has_xtcp) {
                    Json::Value xtcp(Json::objectValue);
                    xtcp["ingress_submitted"] = Json::UInt64(sample.xtcp.ingress_submitted);
                    xtcp["ingress_dropped"] = Json::UInt64(sample.xtcp.ingress_dropped);
                    xtcp["ingress_injected"] = Json::UInt64(sample.xtcp.ingress_injected);
                    xtcp["flows_opened"] = Json::UInt64(sample.xtcp.flows_opened);
                    xtcp["flows_closed"] = Json::UInt64(sample.xtcp.flows_closed);
                    xtcp["flows_active"] = Json::UInt64(sample.xtcp.flows_active);
                    xtcp["timer_polls"] = Json::UInt64(sample.xtcp.timer_polls);
                    xtcp["timer_events"] = Json::UInt64(sample.xtcp.timer_events);
                    xtcp["output_packets"] = Json::UInt64(sample.xtcp.output_packets);
                    xtcp["output_bytes"] = Json::UInt64(sample.xtcp.output_bytes);
                    xtcp["connector_read_bytes"] = Json::UInt64(sample.xtcp.connector_read_bytes);
                    xtcp["connector_written_bytes"] = Json::UInt64(sample.xtcp.connector_written_bytes);
                    xtcp["queued_bytes"] = Json::UInt64(sample.xtcp.queued_bytes);
                    xtcp["queued_bytes_highwater"] = Json::UInt64(sample.xtcp.queued_bytes_highwater);
                    root["xtcp"] = std::move(xtcp);
                }

                Json::Reader reader;
                Json::Value runtime(Json::objectValue);
                const std::string runtime_json = SerializeRuntimeSnapshot(sample.runtime);
                if (reader.parse(
                    runtime_json.data(),
                    runtime_json.data() + runtime_json.size(),
                    runtime) && runtime.isObject()) {
                    root["runtime"] = std::move(runtime);
                }

                Json::FastWriter writer;
                const Json::String encoded = writer.write(root);
                std::string json = detail::FromRuntimeJsonString(encoded);
                while (!json.empty() && (json.back() == '\n' || json.back() == '\r')) {
                    json.pop_back();
                }
                return json;
            }

        }
    }
}
