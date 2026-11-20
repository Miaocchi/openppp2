#pragma once

#include <ppp/stdafx.h>

#include <json/json.h>

namespace ppp {
    namespace configurations {
        struct ServerMultipathConfiguration final {
            static constexpr int DefaultResumeGraceMs = 30000;
            static constexpr int MaxConnections = 32;
            static constexpr int DefaultMaxSessions = 1024;
            static constexpr int MaxSessions = 65535;

            bool enabled = false;
            int resume_grace_ms = DefaultResumeGraceMs;
            int max_connections = MaxConnections;
            int max_sessions = DefaultMaxSessions;

            bool Load(const Json::Value& json, ppp::string& error) noexcept;
            bool Validate(ppp::string& error) const noexcept;
            Json::Value ToJson() const noexcept;
        };

        /** Client-side configuration for the opt-in multi-entry connection mode. */
        struct MultipathConfiguration final {
            struct Path final {
                ppp::string id;
                ppp::string transport;
                ppp::string connect_host;
                int port = 0;
                int connections = 1;
                ppp::string tls_server_name;
                ppp::string http_host;
                ppp::string ws_path;
            };

            static constexpr int MaxPaths = 8;
            static constexpr int MaxConnections = 32;
            static constexpr int DefaultMaxConnections = 8;
            static constexpr int DefaultMaxConcurrentConnects = 4;
            static constexpr int DefaultResumeGraceMs = 30000;

            bool enabled = false;
            ppp::string policy = "adaptive";
            int max_connections = DefaultMaxConnections;
            int max_concurrent_connects = DefaultMaxConcurrentConnects;
            int resume_grace_ms = DefaultResumeGraceMs;
            ppp::vector<Path> paths;

            bool Load(const Json::Value& json, ppp::string& error) noexcept;
            bool Validate(ppp::string& error) const noexcept;
            Json::Value ToJson() const noexcept;
        };
    }
}
