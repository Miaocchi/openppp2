#pragma once

#include <ppp/app/runtime/RuntimeSnapshot.h>

#include <json/json.h>

#include <algorithm>
#include <climits>
#include <string>
#include <utility>

namespace ppp {
    namespace app {
        namespace runtime {

            namespace detail {

                inline Json::String ToRuntimeJsonString(
                    const std::string& value) noexcept {
                    return Json::String(value.data(), value.size());
                }

                inline std::string FromRuntimeJsonString(
                    const Json::String& value) noexcept {
                    return std::string(value.data(), value.size());
                }

                inline std::string RuntimeJsonString(
                    const Json::Value& root,
                    const char* name) noexcept {
                    if (!root.isMember(name) || !root[name].isString()) {
                        return std::string();
                    }
                    return FromRuntimeJsonString(root[name].asString());
                }

                inline bool ValidPathControlState(const std::string& state) noexcept {
                    return state.empty() || state == "disabled" || state == "probing" ||
                        state == "warming" || state == "active" || state == "degraded" ||
                        state == "draining" || state == "retiring" || state == "backoff" ||
                        state == "rejected" || state == "stopped";
                }

                inline void WriteRuntimeError(
                    Json::Value& root,
                    const RuntimeError& error) noexcept {
                    Json::Value value(Json::objectValue);
                    value["code"] = error.code;
                    value["severity"] = ToRuntimeJsonString(error.severity);
                    value["retryable"] = error.retryable;
                    value["user_message_key"] = ToRuntimeJsonString(error.user_message_key);
                    value["diagnostic_detail"] = ToRuntimeJsonString(error.diagnostic_detail);
                    root["last_error"] = std::move(value);
                }

                inline void WriteRuntimeTraffic(
                    Json::Value& root,
                    const RuntimeTraffic& traffic) noexcept {
                    Json::Value value(Json::objectValue);
                    value["rx_bytes"] = Json::UInt64(traffic.rx_bytes);
                    value["tx_bytes"] = Json::UInt64(traffic.tx_bytes);
                    root["traffic"] = std::move(value);
                }

                inline void ReadRuntimeTraffic(
                    const Json::Value& root,
                    RuntimeTraffic& traffic) noexcept {
                    traffic = RuntimeTraffic();
                    if (!root.isMember("traffic") || !root["traffic"].isObject()) {
                        return;
                    }

                    const Json::Value& value = root["traffic"];
                    if (value.isMember("rx_bytes") && value["rx_bytes"].isUInt64()) {
                        traffic.rx_bytes = value["rx_bytes"].asUInt64();
                    }
                    if (value.isMember("tx_bytes") && value["tx_bytes"].isUInt64()) {
                        traffic.tx_bytes = value["tx_bytes"].asUInt64();
                    }
                }

                inline void ReadRuntimeError(
                    const Json::Value& root,
                    RuntimeError& error) noexcept {
                    error = RuntimeError();
                    if (!root.isMember("last_error") || !root["last_error"].isObject()) {
                        return;
                    }

                    const Json::Value& value = root["last_error"];
                    if (value.isMember("code") && value["code"].isUInt()) {
                        error.code = value["code"].asUInt();
                    }
                    if (value.isMember("severity") && value["severity"].isString()) {
                        error.severity = FromRuntimeJsonString(value["severity"].asString());
                    }
                    if (value.isMember("retryable") && value["retryable"].isBool()) {
                        error.retryable = value["retryable"].asBool();
                    }
                    if (value.isMember("user_message_key") && value["user_message_key"].isString()) {
                        error.user_message_key = FromRuntimeJsonString(value["user_message_key"].asString());
                    }
                    if (value.isMember("diagnostic_detail") && value["diagnostic_detail"].isString()) {
                        error.diagnostic_detail = FromRuntimeJsonString(value["diagnostic_detail"].asString());
                    }
                }

                inline void WriteRuntimePeer(
                    Json::Value& value,
                    const RuntimePeerSnapshot& peer) noexcept {
                    value["peer_uuid"] = ToRuntimeJsonString(peer.peer_uuid);
                    value["p2p_priority"] = ToRuntimeJsonString(peer.p2p_priority);
                    value["virtual_ip"] = peer.virtual_ip;
                    value["state"] = ppp::p2p::ToString(peer.state);
                    value["effective_path"] = ToRuntimeJsonString(peer.effective_path);
                    value["has_current"] = peer.has_current;
                    value["has_pending"] = peer.has_pending;
                    value["has_previous"] = peer.has_previous;
                    value["pending_ready"] = peer.pending_ready;
                    value["migration_pending"] = peer.migration_pending;
                    value["generation"] = Json::UInt64(peer.generation);
                    value["key_generation"] = Json::UInt64(peer.key_generation);
                    value["key_deadline_ms"] = Json::UInt64(peer.key_deadline_ms);
                    value["last_receive_ms"] = Json::UInt64(peer.last_receive_ms);
                    value["local_candidate"] = ToRuntimeJsonString(peer.local_candidate);
                    value["peer_candidate"] = ToRuntimeJsonString(peer.peer_candidate);
                }

                inline void ReadRuntimePeer(
                    const Json::Value& value,
                    RuntimePeerSnapshot& peer) noexcept {
                    if (!value.isObject()) return;
                    peer.peer_uuid = RuntimeJsonString(value, "peer_uuid");
                    peer.p2p_priority = RuntimeJsonString(value, "p2p_priority");
                    if (peer.p2p_priority.empty()) peer.p2p_priority = "ipv6-first";
                    if (value["virtual_ip"].isUInt()) peer.virtual_ip = value["virtual_ip"].asUInt();
                    if (value["state"].isString()) peer.state =
                        ppp::p2p::ParseP2PState(RuntimeJsonString(value, "state"));
                    peer.effective_path = RuntimeJsonString(value, "effective_path");
                    if (value["has_current"].isBool()) peer.has_current = value["has_current"].asBool();
                    if (value["has_pending"].isBool()) peer.has_pending = value["has_pending"].asBool();
                    if (value["has_previous"].isBool()) peer.has_previous = value["has_previous"].asBool();
                    if (value["pending_ready"].isBool()) peer.pending_ready = value["pending_ready"].asBool();
                    if (value["migration_pending"].isBool()) peer.migration_pending = value["migration_pending"].asBool();
                    if (value["generation"].isUInt64()) peer.generation = value["generation"].asUInt64();
                    if (value["key_generation"].isUInt64()) peer.key_generation = value["key_generation"].asUInt64();
                    if (value["key_deadline_ms"].isUInt64()) peer.key_deadline_ms = value["key_deadline_ms"].asUInt64();
                    if (value["last_receive_ms"].isUInt64()) peer.last_receive_ms = value["last_receive_ms"].asUInt64();
                    peer.local_candidate = RuntimeJsonString(value, "local_candidate");
                    peer.peer_candidate = RuntimeJsonString(value, "peer_candidate");
                }

                inline bool SafeRuntimeToken(const std::string& value, std::size_t maximum) noexcept {
                    if (value.empty() || value.size() > maximum) return false;
                    for (unsigned char c : value) {
                        const bool ascii_alnum = (c >= 'a' && c <= 'z') ||
                            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                        if (!(ascii_alnum || c == '_' || c == '-' || c == '.')) return false;
                    }
                    return true;
                }

                inline void WriteOptionalRuntimeNumber(Json::Value& object,
                    const char* name, const std::optional<std::uint64_t>& number) noexcept {
                    object[name] = number ? Json::Value(Json::UInt64(*number))
                                          : Json::Value(Json::nullValue);
                }

                using ppp::app::multipath::RuntimeCarrierState;
                inline constexpr std::pair<const char*, std::uint64_t RuntimeCarrierState::*> CarrierNumbers[] = {
                    {"generation", &RuntimeCarrierState::generation},
                    {"tx_bytes", &RuntimeCarrierState::tx_bytes},
                    {"rx_bytes", &RuntimeCarrierState::rx_bytes},
                    {"tx_unique_payload_bytes", &RuntimeCarrierState::tx_unique_payload_bytes},
                    {"rx_unique_payload_bytes", &RuntimeCarrierState::rx_unique_payload_bytes},
                    {"retransmit_bytes", &RuntimeCarrierState::retransmit_bytes},
                    {"retransmit_events", &RuntimeCarrierState::retransmit_events},
                    {"queued_bytes", &RuntimeCarrierState::queued_bytes},
                    {"wire_inflight_bytes", &RuntimeCarrierState::wire_inflight_bytes},
                    {"send_credit_bytes", &RuntimeCarrierState::send_credit_bytes},
                    {"rtt_ms", &RuntimeCarrierState::rtt_ms},
                    {"jitter_ms", &RuntimeCarrierState::jitter_ms},
                    {"queue_delay_ms", &RuntimeCarrierState::queue_delay_ms},
                    {"delivery_rate_bytes_per_second", &RuntimeCarrierState::delivery_rate_bytes_per_second},
                    {"capacity_rate_bytes_per_second", &RuntimeCarrierState::capacity_rate_bytes_per_second},
                    {"scheduling_weight", &RuntimeCarrierState::scheduling_weight},
                    {"confidence", &RuntimeCarrierState::confidence},
                };

                inline void WriteRuntimeMultipath(Json::Value& root,
                    const ppp::app::multipath::RuntimeState& state) noexcept {
                    Json::Value value(Json::objectValue);
                    value["enabled"] = state.enabled;
                    value["policy"] = ToRuntimeJsonString(
                        state.policy == "adaptive" || state.policy == "failover" ? state.policy : "");
                    const bool valid_session_state = state.state == "idle" || state.state == "creating" ||
                        state.state == "active" || state.state == "recovering" ||
                        state.state == "failed" || state.state == "stopped" || state.state == "unknown";
                    value["state"] = ToRuntimeJsonString(valid_session_state ? state.state : "unknown");
                    value["active_paths"] = Json::UInt(std::min<std::uint32_t>(state.active_paths, 8));
                    WriteOptionalRuntimeNumber(value, "remaining_grace_ms", state.remaining_grace_ms);
                    if (state.vmux_session_id) {
                        WriteOptionalRuntimeNumber(value, "vmux_session_id", state.vmux_session_id);
                        WriteOptionalRuntimeNumber(value, "reorder_events", state.reorder_events);
                        WriteOptionalRuntimeNumber(value, "reorder_buffered_bytes", state.reorder_buffered_bytes);
                    }
                    Json::Value capabilities(Json::arrayValue);
                    for (const std::string& capability : state.capabilities) {
                        if (capabilities.size() >= 32) break;
                        if (SafeRuntimeToken(capability, 64)) capabilities.append(ToRuntimeJsonString(capability));
                    }
                    value["capabilities"] = std::move(capabilities);
                    Json::Value paths(Json::arrayValue);
                    std::size_t written_paths = 0;
                    for (const auto& path : state.paths) {
                        if (written_paths >= 8) break;
                        ++written_paths;
                        Json::Value item(Json::objectValue);
                        const std::string safe_id = SafeRuntimeToken(path.id, 64)
                            ? path.id : "unknown-" + std::to_string(written_paths);
                        item["id"] = ToRuntimeJsonString(safe_id);
                        const bool valid_state = path.state == "waiting" || path.state == "connecting" ||
                            path.state == "authenticating" || path.state == "joined" ||
                            path.state == "backoff" || path.state == "stopped" || path.state == "unknown";
                        item["state"] = ToRuntimeJsonString(valid_state ? path.state : "unknown");
                        if (SafeRuntimeToken(path.control_state, 32)) {
                            item["control_state"] = ToRuntimeJsonString(path.control_state);
                        }
                        item["carriers"] = Json::UInt(std::min<std::uint32_t>(path.carriers, 32));
                        item["reconnect_attempts"] = Json::UInt(std::min<std::uint32_t>(path.reconnect_attempts, 1000000));
                        item["error_code"] = ToRuntimeJsonString(
                            path.error_code.empty() || SafeRuntimeToken(path.error_code, 64) ? path.error_code : "");
                        WriteOptionalRuntimeNumber(item, "rtt_ms", path.rtt_ms);
                        WriteOptionalRuntimeNumber(item, "rx_bytes", path.rx_bytes);
                        WriteOptionalRuntimeNumber(item, "tx_bytes", path.tx_bytes);
                        WriteOptionalRuntimeNumber(item, "inflight_bytes", path.inflight_bytes);
                        WriteOptionalRuntimeNumber(item, "retransmit_bytes", path.retransmit_bytes);
                        if (path.tx_unique_payload_bytes) {
                            WriteOptionalRuntimeNumber(item, "tx_unique_payload_bytes", path.tx_unique_payload_bytes);
                        }
                        if (path.rx_unique_payload_bytes) {
                            WriteOptionalRuntimeNumber(item, "rx_unique_payload_bytes", path.rx_unique_payload_bytes);
                        }
                        WriteOptionalRuntimeNumber(item, "capacity_rate_bytes_per_second",
                            path.capacity_rate_bytes_per_second);
                        WriteOptionalRuntimeNumber(item, "queue_delay_ms", path.queue_delay_ms);
                        WriteOptionalRuntimeNumber(item, "bdp_bytes", path.bdp_bytes);
                        WriteOptionalRuntimeNumber(item, "confidence", path.confidence);
                        item["at_capacity"] = path.at_capacity;
                        paths.append(std::move(item));
                    }
                    value["paths"] = std::move(paths);
                    if (!state.links.empty()) {
                        Json::Value links(Json::arrayValue);
                        for (const auto& link : state.links) {
                            if (links.size() >= 32) break;
                            Json::Value item(Json::objectValue);
                            if (SafeRuntimeToken(link.path_id, 64)) item["path_id"] = ToRuntimeJsonString(link.path_id);
                            item["link_id"] = Json::UInt(link.link_id);
                            for (const auto& [name, field] : CarrierNumbers) {
                                item[name] = Json::UInt64(link.*field);
                            }
                            item["metrics_known"] = link.metrics_known;
                            item["app_limited"] = link.app_limited;
                            links.append(std::move(item));
                        }
                        value["links"] = std::move(links);
                    }
                    root["multipath"] = std::move(value);
                }

                inline bool ReadOptionalRuntimeNumber(const Json::Value& object,
                    const char* name, std::optional<std::uint64_t>& output) noexcept {
                    output.reset();
                    if (!object.isMember(name) || object[name].isNull()) return true;
                    if (!object[name].isUInt64()) return false;
                    output = object[name].asUInt64();
                    return true;
                }

                inline bool ReadRuntimeMultipath(const Json::Value& root,
                    ppp::app::multipath::RuntimeState& state) noexcept {
                    using ppp::app::multipath::RuntimePathState;
                    state = {};
                    if (!root.isMember("multipath")) return true;
                    const Json::Value& value = root["multipath"];
                    if (!value.isObject() || !value.isMember("enabled") || !value["enabled"].isBool() ||
                        !value.isMember("paths") || !value["paths"].isArray() || value["paths"].size() > 8 ||
                        !value.isMember("active_paths") || !value["active_paths"].isUInt()) return false;
                    state.enabled = value["enabled"].asBool();
                    state.policy = RuntimeJsonString(value, "policy");
                    state.state = RuntimeJsonString(value, "state");
                    state.active_paths = value["active_paths"].asUInt();
                    if (state.active_paths > 8 ||
                        (!state.policy.empty() && state.policy != "adaptive" && state.policy != "failover") ||
                        (!state.state.empty() && state.state != "idle" && state.state != "creating" &&
                         state.state != "active" && state.state != "recovering" && state.state != "failed" &&
                         state.state != "stopped" && state.state != "unknown")) return false;
                    if (!ReadOptionalRuntimeNumber(value, "remaining_grace_ms", state.remaining_grace_ms) ||
                        !ReadOptionalRuntimeNumber(value, "vmux_session_id", state.vmux_session_id) ||
                        !ReadOptionalRuntimeNumber(value, "reorder_events", state.reorder_events) ||
                        !ReadOptionalRuntimeNumber(value, "reorder_buffered_bytes", state.reorder_buffered_bytes)) return false;
                    if (value.isMember("capabilities")) {
                        if (!value["capabilities"].isArray() || value["capabilities"].size() > 32) return false;
                        for (const Json::Value& capability : value["capabilities"]) {
                            if (!capability.isString() || !SafeRuntimeToken(FromRuntimeJsonString(capability.asString()), 64)) return false;
                            state.capabilities.emplace_back(FromRuntimeJsonString(capability.asString()));
                        }
                    }
                    for (const Json::Value& item : value["paths"]) {
                        if (!item.isObject()) return false;
                        RuntimePathState path;
                        path.id = RuntimeJsonString(item, "id");
                        path.state = RuntimeJsonString(item, "state");
                        path.control_state = RuntimeJsonString(item, "control_state");
                        path.error_code = RuntimeJsonString(item, "error_code");
                        if (!SafeRuntimeToken(path.id, 64) ||
                            (!path.error_code.empty() && !SafeRuntimeToken(path.error_code, 64)) ||
                            !ValidPathControlState(path.control_state) ||
                            (path.state != "waiting" && path.state != "connecting" &&
                             path.state != "authenticating" && path.state != "joined" &&
                             path.state != "backoff" && path.state != "stopped" && path.state != "unknown") ||
                            !item.isMember("carriers") || !item["carriers"].isUInt() ||
                            !item.isMember("reconnect_attempts") || !item["reconnect_attempts"].isUInt()) return false;
                        if (std::any_of(state.paths.begin(), state.paths.end(),
                            [&path](const RuntimePathState& prior) { return prior.id == path.id; })) return false;
                        path.carriers = item["carriers"].asUInt();
                        path.reconnect_attempts = item["reconnect_attempts"].asUInt();
                        if (path.carriers > 32 || path.reconnect_attempts > 1000000 ||
                            !ReadOptionalRuntimeNumber(item, "rtt_ms", path.rtt_ms) ||
                            !ReadOptionalRuntimeNumber(item, "rx_bytes", path.rx_bytes) ||
                            !ReadOptionalRuntimeNumber(item, "tx_bytes", path.tx_bytes) ||
                            !ReadOptionalRuntimeNumber(item, "inflight_bytes", path.inflight_bytes) ||
                            !ReadOptionalRuntimeNumber(item, "retransmit_bytes", path.retransmit_bytes) ||
                            !ReadOptionalRuntimeNumber(item, "tx_unique_payload_bytes", path.tx_unique_payload_bytes) ||
                            !ReadOptionalRuntimeNumber(item, "rx_unique_payload_bytes", path.rx_unique_payload_bytes) ||
                            !ReadOptionalRuntimeNumber(item, "capacity_rate_bytes_per_second",
                                path.capacity_rate_bytes_per_second) ||
                            !ReadOptionalRuntimeNumber(item, "queue_delay_ms", path.queue_delay_ms) ||
                            !ReadOptionalRuntimeNumber(item, "bdp_bytes", path.bdp_bytes) ||
                            !ReadOptionalRuntimeNumber(item, "confidence", path.confidence)) return false;
                        if (item.isMember("at_capacity")) {
                            if (!item["at_capacity"].isBool()) return false;
                            path.at_capacity = item["at_capacity"].asBool();
                        }
                        state.paths.emplace_back(std::move(path));
                    }
                    if (value.isMember("links")) {
                        if (!value["links"].isArray() || value["links"].size() > 32) return false;
                        for (const Json::Value& item : value["links"]) {
                            if (!item.isObject() || !item.isMember("link_id") || !item["link_id"].isUInt() ||
                                item["link_id"].asUInt() == 0 || item["link_id"].asUInt() > UINT16_MAX ||
                                !item.isMember("metrics_known") || !item["metrics_known"].isBool() ||
                                !item.isMember("app_limited") || !item["app_limited"].isBool()) return false;
                            RuntimeCarrierState link;
                            link.link_id = static_cast<std::uint16_t>(item["link_id"].asUInt());
                            link.metrics_known = item["metrics_known"].asBool();
                            link.app_limited = item["app_limited"].asBool();
                            if (item.isMember("path_id")) {
                                if (!item["path_id"].isString()) return false;
                                link.path_id = RuntimeJsonString(item, "path_id");
                                if (!SafeRuntimeToken(link.path_id, 64)) return false;
                            }
                            for (const auto& [name, field] : CarrierNumbers) {
                                if (!item.isMember(name) || !item[name].isUInt64()) return false;
                                link.*field = item[name].asUInt64();
                            }
                            if (link.generation == 0 || std::any_of(state.links.begin(), state.links.end(),
                                [&link](const auto& prior) { return prior.link_id == link.link_id; })) return false;
                            state.links.emplace_back(std::move(link));
                        }
                    }
                    if (state.enabled && state.paths.empty() && !state.vmux_session_id) return false;
                    return true;
                }

            }

            inline std::string SerializeRuntimeSnapshot(
                const RuntimeSnapshot& snapshot) noexcept {
                Json::Value root(Json::objectValue);
                root["schema_version"] = snapshot.schema_version;
                root["generation"] = Json::UInt64(snapshot.generation);
                root["monotonic_ms"] = Json::UInt64(snapshot.monotonic_ms);
                root["phase"] = ToString(snapshot.phase);
                root["role"] = detail::ToRuntimeJsonString(snapshot.role);
                root["server"] = detail::ToRuntimeJsonString(snapshot.server);
                root["transport"] = detail::ToRuntimeJsonString(snapshot.transport);
                Json::Value capabilities(Json::arrayValue);
                for (const std::string& capability : snapshot.capabilities) {
                    capabilities.append(detail::ToRuntimeJsonString(capability));
                }
                root["capabilities"] = std::move(capabilities);
                root["requested_mux_mode"] = detail::ToRuntimeJsonString(snapshot.requested_mux_mode);
                root["effective_mux_mode"] = detail::ToRuntimeJsonString(snapshot.effective_mux_mode);
                root["mux_receiver_ordering"] = detail::ToRuntimeJsonString(snapshot.mux_receiver_ordering);
                root["mux_scheduler"] = detail::ToRuntimeJsonString(snapshot.mux_scheduler);
                root["mux_pool_policy"] = detail::ToRuntimeJsonString(snapshot.mux_pool_policy);
                root["mux_turbo"] = snapshot.mux_turbo;
                root["mux_active_links"] = snapshot.mux_active_links;
                root["mux_fallback_reason"] = detail::ToRuntimeJsonString(snapshot.mux_fallback_reason);
                root["p2p_state"] = ppp::p2p::ToString(snapshot.p2p_state);
                root["p2p_gate_reason"] = snapshot.p2p_gate_reason.c_str();
                root["p2p_priority"] = detail::ToRuntimeJsonString(snapshot.p2p_priority);
                root["effective_path"] = ppp::p2p::EffectivePath(snapshot.p2p_state);
                Json::Value peers(Json::arrayValue);
                for (const RuntimePeerSnapshot& peer : snapshot.peers) {
                    Json::Value value(Json::objectValue);
                    detail::WriteRuntimePeer(value, peer);
                    peers.append(std::move(value));
                }
                root["peers"] = std::move(peers);
                detail::WriteRuntimeTraffic(root, snapshot.traffic);
                root["connected_monotonic_ms"] = Json::UInt64(snapshot.connected_monotonic_ms);
                detail::WriteRuntimeError(root, snapshot.last_error);
                detail::WriteRuntimeMultipath(root, snapshot.multipath);

                Json::FastWriter writer;
                const Json::String encoded = writer.write(root);
                std::string json = detail::FromRuntimeJsonString(encoded);
                while (!json.empty() && (json.back() == '\n' || json.back() == '\r')) {
                    json.pop_back();
                }
                return json;
            }

            inline bool ParseRuntimeSnapshot(
                const std::string& json,
                RuntimeSnapshot& snapshot) noexcept {
                if (json.empty()) {
                    return false;
                }

                Json::Reader reader;
                Json::Value root;
                if (!reader.parse(json.data(), json.data() + json.size(), root) || !root.isObject()) {
                    return false;
                }
                if (!root.isMember("schema_version") || !root["schema_version"].isUInt()) {
                    return false;
                }
                if (root["schema_version"].asUInt() != RuntimeSnapshot::SchemaVersion) {
                    return false;
                }
                if (!root.isMember("generation") || !root["generation"].isUInt64()) {
                    return false;
                }
                if (!root.isMember("monotonic_ms") || !root["monotonic_ms"].isUInt64()) {
                    return false;
                }
                if (!root.isMember("phase") || !root["phase"].isString()) {
                    return false;
                }

                const std::string phase_name = detail::RuntimeJsonString(root, "phase");
                const RuntimePhase phase = ParseRuntimePhase(phase_name);
                if (phase == RuntimePhase::Unknown && phase_name != "unknown") {
                    return false;
                }

                RuntimeSnapshot parsed;
                parsed.schema_version = root["schema_version"].asUInt();
                parsed.generation = root["generation"].asUInt64();
                parsed.monotonic_ms = root["monotonic_ms"].asUInt64();
                parsed.phase = phase;
                parsed.role = detail::RuntimeJsonString(root, "role");
                parsed.server = detail::RuntimeJsonString(root, "server");
                parsed.transport = detail::RuntimeJsonString(root, "transport");
                if (root.isMember("capabilities") && root["capabilities"].isArray()) {
                    for (const Json::Value& capability : root["capabilities"]) {
                        if (capability.isString()) {
                            parsed.capabilities.emplace_back(
                                detail::FromRuntimeJsonString(capability.asString()));
                        }
                    }
                }
                parsed.requested_mux_mode = detail::RuntimeJsonString(root, "requested_mux_mode");
                parsed.effective_mux_mode = detail::RuntimeJsonString(root, "effective_mux_mode");
                parsed.mux_receiver_ordering = detail::RuntimeJsonString(root, "mux_receiver_ordering");
                parsed.mux_scheduler = detail::RuntimeJsonString(root, "mux_scheduler");
                parsed.mux_pool_policy = detail::RuntimeJsonString(root, "mux_pool_policy");
                if (root.isMember("mux_turbo") && root["mux_turbo"].isBool()) {
                    parsed.mux_turbo = root["mux_turbo"].asBool();
                }
                if (root.isMember("mux_active_links") && root["mux_active_links"].isUInt()) {
                    parsed.mux_active_links = static_cast<std::uint16_t>(
                        std::min<unsigned int>(root["mux_active_links"].asUInt(), UINT16_MAX));
                }
                parsed.mux_fallback_reason = detail::RuntimeJsonString(root, "mux_fallback_reason");
                parsed.p2p_gate_reason = root.isMember("p2p_gate_reason")
                    ? detail::RuntimeJsonString(root, "p2p_gate_reason") : "";
                parsed.p2p_state = root.isMember("p2p_state")
                    ? ppp::p2p::ParseP2PState(
                        detail::RuntimeJsonString(root, "p2p_state"))
                    : ppp::p2p::P2PState::Disabled;
                parsed.p2p_priority = detail::RuntimeJsonString(root, "p2p_priority");
                if (parsed.p2p_priority.empty()) parsed.p2p_priority = "ipv6-first";
                if (root["peers"].isArray()) {
                    for (const Json::Value& value : root["peers"]) {
                        RuntimePeerSnapshot peer;
                        detail::ReadRuntimePeer(value, peer);
                        parsed.peers.emplace_back(std::move(peer));
                    }
                }
                detail::ReadRuntimeTraffic(root, parsed.traffic);
                if (root.isMember("connected_monotonic_ms") &&
                    root["connected_monotonic_ms"].isUInt64()) {
                    parsed.connected_monotonic_ms = root["connected_monotonic_ms"].asUInt64();
                }
                detail::ReadRuntimeError(root, parsed.last_error);
                if (!detail::ReadRuntimeMultipath(root, parsed.multipath)) return false;

                snapshot = std::move(parsed);
                return true;
            }

        }
    }
}
