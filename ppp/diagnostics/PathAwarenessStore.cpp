#include <ppp/diagnostics/PathAwarenessStore.h>
#include <ppp/auxiliary/JsonAuxiliary.h>

#include <chrono>
#include <deque>
#include <mutex>

namespace ppp {
    namespace diagnostics {
        using ppp::auxiliary::JsonAuxiliary;

        namespace {
            struct PathAwarenessState final {
                std::mutex              syncobj;
                ppp::string             local_snapshot = "{}";
                ppp::string             peer_snapshot = "null";
                std::deque<Json::Value> observations;
                Json::Value             txrx_by_path = Json::Value(Json::arrayValue);
                uint64_t                native_updated_at = 0;
            };

            PathAwarenessState& GetState() noexcept {
                static PathAwarenessState state;
                return state;
            }

            uint64_t NowMs() noexcept {
                using namespace std::chrono;
                return static_cast<uint64_t>(
                    duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
            }

            Json::Value ParseObjectOrEmpty(const ppp::string& json) noexcept {
                Json::Value root = JsonAuxiliary::FromString(json);
                if (!root.isObject()) {
                    root = Json::Value(Json::objectValue);
                }

                return root;
            }

            Json::Value BuildNativeLocked(const PathAwarenessState& state) noexcept {
                Json::Value native(Json::objectValue);
                native["schemaVersion"] = 1;
                native["updatedAtMs"] = Json::UInt64(state.native_updated_at);

                Json::Value observations(Json::arrayValue);
                for (const Json::Value& item : state.observations) {
                    observations.append(item);
                }
                native["observations"] = observations;

                native["txRxByPath"] = state.txrx_by_path.isArray()
                    ? state.txrx_by_path
                    : Json::Value(Json::arrayValue);
                return native;
            }
        }

        bool PathAwarenessStore::SetLocalSnapshot(const ppp::string& json) noexcept {
            PathAwarenessState& state = GetState();
            std::lock_guard<std::mutex> scope(state.syncobj);
            state.local_snapshot = json.empty() ? "{}" : json;
            return true;
        }

        ppp::string PathAwarenessStore::GetLocalSnapshot() noexcept {
            PathAwarenessState& state = GetState();
            std::lock_guard<std::mutex> scope(state.syncobj);
            return state.local_snapshot.empty() ? "{}" : state.local_snapshot;
        }

        ppp::string PathAwarenessStore::GetLocalSnapshotWithNative() noexcept {
            PathAwarenessState& state = GetState();
            std::lock_guard<std::mutex> scope(state.syncobj);

            Json::Value root = ParseObjectOrEmpty(state.local_snapshot);
            root["native"] = BuildNativeLocked(state);
            return JsonAuxiliary::ToString(root);
        }

        bool PathAwarenessStore::SetPeerSnapshot(const ppp::string& json) noexcept {
            PathAwarenessState& state = GetState();
            std::lock_guard<std::mutex> scope(state.syncobj);
            state.peer_snapshot = json.empty() ? "null" : json;
            return true;
        }

        ppp::string PathAwarenessStore::GetPeerSnapshot() noexcept {
            PathAwarenessState& state = GetState();
            std::lock_guard<std::mutex> scope(state.syncobj);
            return state.peer_snapshot.empty() ? "null" : state.peer_snapshot;
        }

        void PathAwarenessStore::RecordConnectObservation(
            const char* role,
            int path_id,
            const char* transport,
            int64_t connect_ms,
            int64_t handshake_ms,
            int error_code) noexcept {

            Json::Value item(Json::objectValue);
            item["updatedAtMs"] = Json::UInt64(NowMs());
            item["role"] = NULLPTR != role ? role : "unknown";
            item["pathId"] = path_id;
            item["transport"] = NULLPTR != transport ? transport : "default";
            if (connect_ms >= 0) {
                item["connectMs"] = Json::Int64(connect_ms);
            }
            if (handshake_ms >= 0) {
                item["handshakeMs"] = Json::Int64(handshake_ms);
            }
            item["errorCode"] = error_code;

            PathAwarenessState& state = GetState();
            std::lock_guard<std::mutex> scope(state.syncobj);
            state.native_updated_at = NowMs();
            state.observations.push_back(item);
            while (state.observations.size() > 24) {
                state.observations.pop_front();
            }
        }

        void PathAwarenessStore::RecordTrafficSnapshot(
            int path_id,
            const char* transport,
            uint64_t tx_bytes,
            uint64_t rx_bytes,
            uint64_t in_bytes,
            uint64_t out_bytes) noexcept {

            Json::Value item(Json::objectValue);
            item["updatedAtMs"] = Json::UInt64(NowMs());
            item["pathId"] = path_id;
            item["transport"] = NULLPTR != transport ? transport : "default";
            item["tx"] = stl::to_string<ppp::string>(tx_bytes);
            item["rx"] = stl::to_string<ppp::string>(rx_bytes);
            item["in"] = stl::to_string<ppp::string>(in_bytes);
            item["out"] = stl::to_string<ppp::string>(out_bytes);

            Json::Value txrx(Json::arrayValue);
            txrx.append(item);

            PathAwarenessState& state = GetState();
            std::lock_guard<std::mutex> scope(state.syncobj);
            state.native_updated_at = NowMs();
            state.txrx_by_path = txrx;
        }

        ppp::string PathAwarenessStore::GetNativeSnapshot() noexcept {
            PathAwarenessState& state = GetState();
            std::lock_guard<std::mutex> scope(state.syncobj);
            return JsonAuxiliary::ToString(BuildNativeLocked(state));
        }

        void PathAwarenessStore::Clear() noexcept {
            PathAwarenessState& state = GetState();
            std::lock_guard<std::mutex> scope(state.syncobj);
            state.local_snapshot = "{}";
            state.peer_snapshot = "null";
            state.observations.clear();
            state.txrx_by_path = Json::Value(Json::arrayValue);
            state.native_updated_at = 0;
        }
    }
}
