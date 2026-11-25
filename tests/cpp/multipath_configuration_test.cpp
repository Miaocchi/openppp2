#define BOOST_TEST_MODULE multipath_configuration_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/configurations/AppConfiguration.h>
#include <ppp/configurations/MultipathConfiguration.h>

#include <json/json.h>

#include <fstream>
#include <cstdlib>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {
    using ppp::configurations::AppConfiguration;
    using ppp::configurations::MultipathConfiguration;

    static Json::Value MakeMultipath(bool enabled = true) {
        Json::Value root(Json::objectValue);
        root["enabled"] = enabled;
        root["paths"] = Json::arrayValue;
        Json::Value path(Json::objectValue);
        path["id"] = "origin";
        path["transport"] = "tcp";
        path["connect_host"] = "origin.example.test";
        path["port"] = 443;
        root["paths"].append(path);
        return root;
    }

    static bool Parse(Json::Value value, ppp::string& error) {
        MultipathConfiguration config;
        return config.Load(value, error);
    }
}

BOOST_AUTO_TEST_CASE(defaults_and_roundtrip_preserve_typed_paths) {
    MultipathConfiguration defaults;
    BOOST_TEST(!defaults.enabled);
    BOOST_TEST(defaults.policy == "adaptive");
    BOOST_TEST(defaults.max_connections == 8);
    BOOST_TEST(defaults.max_concurrent_connects == 4);
    BOOST_TEST(defaults.resume_grace_ms == 30000);
    AppConfiguration app_defaults;
    Json::Value defaults_json = app_defaults.ToJson();
    BOOST_TEST(!defaults_json["client"].isMember("multipath"));
    BOOST_TEST(!defaults_json["server"].isMember("multipath"));

    Json::Value source = MakeMultipath();
    source["policy"] = "failover";
    source["max_connections"] = 2;
    source["max_concurrent_connects"] = 1;
    source["paths"][0]["connections"] = 2;
    ppp::string error;
    MultipathConfiguration parsed;
    BOOST_REQUIRE(parsed.Load(source, error));
    Json::Value serialized = parsed.ToJson();
    MultipathConfiguration roundtrip;
    BOOST_REQUIRE(roundtrip.Load(serialized, error));
    BOOST_TEST(roundtrip.enabled);
    BOOST_TEST(roundtrip.policy == "failover");
    BOOST_TEST(roundtrip.max_connections == 2);
    BOOST_TEST(roundtrip.max_concurrent_connects == 1);
    BOOST_REQUIRE(roundtrip.paths.size() == 1u);
    BOOST_TEST(roundtrip.paths[0].id == "origin");
    BOOST_TEST(roundtrip.paths[0].transport == "tcp");
    BOOST_TEST(roundtrip.paths[0].connections == 2);
}

BOOST_AUTO_TEST_CASE(websocket_fields_are_transport_scoped_and_crlf_safe) {
    Json::Value ws = MakeMultipath();
    ws["paths"][0]["transport"] = "wss";
    ws["paths"][0]["tls_server_name"] = "edge.example.test";
    ws["paths"][0]["http_host"] = "cdn.example.test";
    ws["paths"][0]["ws_path"] = "/tunnel";
    ppp::string error;
    BOOST_TEST(Parse(ws, error));

    ws["paths"][0]["ws_path"] = "/tunnel%0d%0aHost:evil";
    BOOST_TEST(!Parse(ws, error));
    BOOST_TEST(error.find("ws_path") != std::string::npos);

    ws["paths"][0]["ws_path"] = "/tunnel";
    ws["paths"][0]["http_host"] = "cdn.example.test\r\nX-Test: yes";
    BOOST_TEST(!Parse(ws, error));
    BOOST_TEST(error.find("http_host") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(url_paths_split_dial_address_from_wss_identity) {
    Json::Value root = MakeMultipath();
    Json::Value& path = root["paths"][0];
    path.removeMember("transport");
    path.removeMember("connect_host");
    path.removeMember("port");
    path["url"] = "wss://vpn.example.com[203.0.113.10]:443/tun";
    ppp::string error;

    MultipathConfiguration parsed;
    BOOST_REQUIRE(parsed.Load(root, error));
    BOOST_REQUIRE(parsed.paths.size() == 1u);
    BOOST_TEST(parsed.paths[0].transport == "wss");
    BOOST_TEST(parsed.paths[0].connect_host == "203.0.113.10");
    BOOST_TEST(parsed.paths[0].port == 443);
    BOOST_TEST(parsed.paths[0].tls_server_name == "vpn.example.com");
    BOOST_TEST(parsed.paths[0].http_host == "vpn.example.com");
    BOOST_TEST(parsed.paths[0].ws_path == "/tun");
}

BOOST_AUTO_TEST_CASE(unknown_keys_and_wrong_types_are_rejected_with_safe_errors) {
    ppp::string error;
    Json::Value root = MakeMultipath();
    root["credential"] = "must-not-be-accepted";
    BOOST_TEST(!Parse(root, error));
    BOOST_TEST(error.find("unknown client.multipath key") != std::string::npos);

    root = MakeMultipath();
    root["paths"][0]["port"] = "443";
    BOOST_TEST(!Parse(root, error));
    BOOST_TEST(error.find("port must be an integer") != std::string::npos);

    root = MakeMultipath();
    root["paths"][0]["unexpected"] = 1;
    BOOST_TEST(!Parse(root, error));
    BOOST_TEST(error.find("unknown client.multipath.paths[0] key") != std::string::npos);

    root = MakeMultipath();
    root["paths"] = Json::Value(Json::nullValue);
    BOOST_TEST(!Parse(root, error));
    BOOST_TEST(error.find("paths must be an array") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(path_identity_transport_and_resource_limits_are_enforced) {
    ppp::string error;
    Json::Value root = MakeMultipath();
    root["paths"][0]["id"] = "same";
    Json::Value second = root["paths"][0];
    second["transport"] = "wss";
    second["tls_server_name"] = "edge.example.test";
    root["paths"].append(second);
    BOOST_TEST(!Parse(root, error));
    BOOST_TEST(error.find("unique") != std::string::npos);

    root = MakeMultipath();
    root["max_connections"] = 1;
    root["paths"][0]["connections"] = 2;
    BOOST_TEST(!Parse(root, error));
    BOOST_TEST(error.find("exceeds client.multipath.max_connections") != std::string::npos);

    root = MakeMultipath();
    root["max_connections"] = 33;
    BOOST_TEST(!Parse(root, error));
    BOOST_TEST(error.find("between 1 and 32") != std::string::npos);

    root = MakeMultipath();
    for (int i = 1; i < 9; ++i) {
        Json::Value path = root["paths"][0];
        ppp::string id = "path" + stl::to_string<ppp::string>(i);
        path["id"] = id.c_str();
        root["paths"].append(path);
    }
    root["max_connections"] = 32;
    BOOST_TEST(!Parse(root, error));
    BOOST_TEST(error.find("8 path limit") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(app_configuration_requires_configured_auth_key_and_roundtrips) {
#if !defined(_WIN32)
    char path[] = "/tmp/openppp2-multipath-secret-XXXXXX";
    int fd = mkstemp(path);
    BOOST_REQUIRE(fd >= 0);
    const std::string secret(64, 'a');
    BOOST_REQUIRE(write(fd, secret.data(), secret.size()) == static_cast<ssize_t>(secret.size()));
    close(fd);

    AppConfiguration defaults;
    Json::Value json = defaults.ToJson();
    json["client"]["multipath"] = MakeMultipath();
    json["server"]["multipath"]["enabled"] = true;
    json["server"]["multipath"]["max_sessions"] = 2048;
    AppConfiguration missing_key;
    BOOST_TEST(!missing_key.Load(json));

    json["transport-auth"]["keys"][0]["id"] = "multipath-test";
    json["transport-auth"]["keys"][0]["state"] = "active";
    json["transport-auth"]["keys"][0]["secret-file"] = path;
    AppConfiguration loaded;
    BOOST_REQUIRE(loaded.Load(json));
    BOOST_TEST(loaded.client.multipath.enabled);
    BOOST_TEST(!loaded.client.transport_auth.enabled);
    BOOST_TEST(loaded.server.multipath.enabled);
    BOOST_TEST(!loaded.server.transport_auth.enabled);
    BOOST_REQUIRE(loaded.client.multipath.paths.size() == 1u);
    Json::Value saved = loaded.ToJson();
    AppConfiguration reloaded;
    BOOST_REQUIRE(reloaded.Load(saved));
    BOOST_TEST(reloaded.client.multipath.enabled);
    BOOST_TEST(reloaded.client.multipath.paths[0].connect_host == "origin.example.test");
    BOOST_TEST(reloaded.server.multipath.enabled);
    BOOST_TEST(reloaded.server.multipath.max_sessions == 2048);
    unlink(path);
#endif
}

BOOST_AUTO_TEST_CASE(server_defaults_and_limits_are_strictly_validated) {
    ppp::configurations::ServerMultipathConfiguration config;
    BOOST_TEST(!config.enabled);
    BOOST_TEST(config.resume_grace_ms == 30000);
    BOOST_TEST(config.max_connections == 32);
    BOOST_TEST(config.max_sessions == 1024);

    Json::Value json(Json::objectValue);
    json["enabled"] = true;
    json["resume_grace_ms"] = 30000;
    json["max_connections"] = 32;
    json["max_sessions"] = 2048;
    ppp::string error;
    BOOST_REQUIRE(config.Load(json, error));
    BOOST_TEST(config.enabled);
    BOOST_TEST(config.max_sessions == 2048);

    json["max_connections"] = 33;
    BOOST_TEST(!config.Load(json, error));
    BOOST_TEST(error.find("between 1 and 32") != std::string::npos);
    json.removeMember("max_connections");
    json["unknown"] = 1;
    BOOST_TEST(!config.Load(json, error));
    BOOST_TEST(error.find("unknown server.multipath key") != std::string::npos);
}
