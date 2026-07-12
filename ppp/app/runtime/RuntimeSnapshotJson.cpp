#include <ppp/app/runtime/RuntimeSnapshotJson.h>

#include <json/json.h>

namespace ppp {
    namespace app {
        namespace runtime {

            namespace {

                ppp::string JsonString(const Json::Value& root, const char* name) noexcept {
                    if (!root.isMember(name) || !root[name].isString()) {
                        return ppp::string();
                    }
                    return root[name].asString();
                }

                void WriteRuntimeError(Json::Value& root, const RuntimeError& error) noexcept {
                    Json::Value value(Json::objectValue);
                    value["code"] = error.code;
                    value["severity"] = error.severity;
                    value["retryable"] = error.retryable;
                    value["user_message_key"] = error.user_message_key;
                    value["diagnostic_detail"] = error.diagnostic_detail;
                    root["last_error"] = std::move(value);
                }

                void ReadRuntimeError(const Json::Value& root, RuntimeError& error) noexcept {
                    error = RuntimeError();
                    if (!root.isMember("last_error") || !root["last_error"].isObject()) {
                        return;
                    }

                    const Json::Value& value = root["last_error"];
                    if (value.isMember("code") && value["code"].isUInt()) {
                        error.code = value["code"].asUInt();
                    }
                    if (value.isMember("severity") && value["severity"].isString()) {
                        error.severity = value["severity"].asString();
                    }
                    if (value.isMember("retryable") && value["retryable"].isBool()) {
                        error.retryable = value["retryable"].asBool();
                    }
                    if (value.isMember("user_message_key") && value["user_message_key"].isString()) {
                        error.user_message_key = value["user_message_key"].asString();
                    }
                    if (value.isMember("diagnostic_detail") && value["diagnostic_detail"].isString()) {
                        error.diagnostic_detail = value["diagnostic_detail"].asString();
                    }
                }

            }

            ppp::string SerializeRuntimeSnapshot(const RuntimeSnapshot& snapshot) noexcept {
                Json::Value root(Json::objectValue);
                root["schema_version"] = snapshot.schema_version;
                root["generation"] = Json::UInt64(snapshot.generation);
                root["monotonic_ms"] = Json::UInt64(snapshot.monotonic_ms);
                root["phase"] = ToString(snapshot.phase);
                root["role"] = snapshot.role;
                root["server"] = snapshot.server;
                root["transport"] = snapshot.transport;
                root["requested_mux_mode"] = snapshot.requested_mux_mode;
                root["effective_mux_mode"] = snapshot.effective_mux_mode;
                root["mux_fallback_reason"] = snapshot.mux_fallback_reason;
                root["p2p_state"] = snapshot.p2p_state;
                root["effective_path"] = snapshot.effective_path;
                WriteRuntimeError(root, snapshot.last_error);

                Json::FastWriter writer;
                ppp::string json = writer.write(root);
                while (!json.empty() && (json.back() == '\n' || json.back() == '\r')) {
                    json.pop_back();
                }
                return json;
            }

            bool ParseRuntimeSnapshot(const ppp::string& json, RuntimeSnapshot& snapshot) noexcept {
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
                if (!root.isMember("phase") || !root["phase"].isString()) {
                    return false;
                }

                const ppp::string phase_name = root["phase"].asString();
                const RuntimePhase phase = ParseRuntimePhase(phase_name);
                if (phase == RuntimePhase::Unknown && phase_name != "unknown") {
                    return false;
                }

                RuntimeSnapshot parsed;
                parsed.phase = phase;
                if (root.isMember("generation") && root["generation"].isUInt64()) {
                    parsed.generation = root["generation"].asUInt64();
                }
                if (root.isMember("monotonic_ms") && root["monotonic_ms"].isUInt64()) {
                    parsed.monotonic_ms = root["monotonic_ms"].asUInt64();
                }
                parsed.role = JsonString(root, "role");
                parsed.server = JsonString(root, "server");
                parsed.transport = JsonString(root, "transport");
                parsed.requested_mux_mode = JsonString(root, "requested_mux_mode");
                parsed.effective_mux_mode = JsonString(root, "effective_mux_mode");
                parsed.mux_fallback_reason = JsonString(root, "mux_fallback_reason");
                parsed.p2p_state = JsonString(root, "p2p_state");
                parsed.effective_path = JsonString(root, "effective_path");
                ReadRuntimeError(root, parsed.last_error);

                snapshot = std::move(parsed);
                return true;
            }

        }
    }
}
