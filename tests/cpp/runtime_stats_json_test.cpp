#define BOOST_TEST_MODULE runtime_stats_json_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/runtime/RuntimeStatsJson.h>

#include <json/json.h>

namespace runtime = ppp::app::runtime;

BOOST_AUTO_TEST_CASE(stats_json_preserves_v1_contract) {
    runtime::RuntimeStatsSample sample;
    sample.monotonic_ms = 8080123;
    sample.rx_bytes = 1932734464;
    sample.tx_bytes = 224690176;
    sample.link.quality_percent = 99.2;
    sample.link.grade = "Good";
    sample.link.error_count = 12;
    sample.link.success_count = 14803;
    sample.runtime.generation = 7;
    sample.runtime.phase = runtime::RuntimePhase::Connected;
    sample.runtime.role = "client";
    sample.runtime.mux_active_links = 4;
    sample.runtime.p2p_state = ppp::p2p::P2PState::Direct;

    Json::Value root;
    Json::Reader reader;
    const std::string encoded = runtime::SerializeRuntimeStats(sample);
    BOOST_REQUIRE(reader.parse(encoded.data(), encoded.data() + encoded.size(), root));
    BOOST_TEST(root["type"].asString() == "ppp-stats");
    BOOST_TEST(root["version"].asUInt() == 1u);
    BOOST_TEST(root["monotonic_ms"].asUInt64() == 8080123u);
    BOOST_TEST(root["rx_bytes"].asUInt64() == 1932734464u);
    BOOST_TEST(root["tx_bytes"].asUInt64() == 224690176u);
    BOOST_TEST(root["link"]["quality_percent"].asDouble() == 99.2);
    BOOST_TEST(root["link"]["grade"].asString() == "Good");
    BOOST_TEST(root["link"]["error_count"].asUInt64() == 12u);
    BOOST_TEST(root["link"]["success_count"].asUInt64() == 14803u);
    BOOST_TEST(root["runtime"]["phase"].asString() == "connected");
    BOOST_TEST(root["runtime"]["role"].asString() == "client");
    BOOST_TEST(root["runtime"]["mux_active_links"].asUInt() == 4u);
    BOOST_TEST(root["runtime"]["effective_path"].asString() == "direct");
}

BOOST_AUTO_TEST_CASE(stats_json_is_a_single_ndjson_record_without_newline) {
    const std::string json = runtime::SerializeRuntimeStats(runtime::RuntimeStatsSample());
    BOOST_TEST(!json.empty());
    BOOST_TEST(json.find('\n') == std::string::npos);
    BOOST_TEST(json.find('\r') == std::string::npos);
}

BOOST_AUTO_TEST_CASE(stats_json_omits_optional_runtime_blocks_by_default) {
    const std::string encoded = runtime::SerializeRuntimeStats(runtime::RuntimeStatsSample());
    Json::Value root;
    Json::Reader reader;
    BOOST_REQUIRE(reader.parse(encoded.data(), encoded.data() + encoded.size(), root));
    BOOST_TEST(!root.isMember("tcp_stack"));
    BOOST_TEST(!root.isMember("tap_linux"));
    BOOST_TEST(!root.isMember("xtcp"));
}

BOOST_AUTO_TEST_CASE(stats_json_emits_xtcp_block_when_present) {
    runtime::RuntimeStatsSample sample;
    sample.has_xtcp = true;
    sample.xtcp.ingress_submitted = 4096;
    sample.xtcp.ingress_dropped = 7;
    sample.xtcp.ingress_injected = 4089;
    sample.xtcp.flows_opened = 33;
    sample.xtcp.flows_closed = 30;
    sample.xtcp.flows_active = 3;
    sample.xtcp.timer_polls = 981;
    sample.xtcp.timer_events = 210;
    sample.xtcp.output_packets = 5100;
    sample.xtcp.output_bytes = 7340032;
    sample.xtcp.connector_read_bytes = 1048576;
    sample.xtcp.connector_written_bytes = 6291456;

    const std::string encoded = runtime::SerializeRuntimeStats(sample);
    Json::Value root;
    Json::Reader reader;
    BOOST_REQUIRE(reader.parse(encoded.data(), encoded.data() + encoded.size(), root));
    BOOST_REQUIRE(root.isMember("xtcp"));
    const Json::Value& xtcp = root["xtcp"];
    BOOST_TEST(xtcp["ingress_submitted"].asUInt64() == 4096u);
    BOOST_TEST(xtcp["ingress_dropped"].asUInt64() == 7u);
    BOOST_TEST(xtcp["ingress_injected"].asUInt64() == 4089u);
    BOOST_TEST(xtcp["flows_opened"].asUInt64() == 33u);
    BOOST_TEST(xtcp["flows_closed"].asUInt64() == 30u);
    BOOST_TEST(xtcp["flows_active"].asUInt64() == 3u);
    BOOST_TEST(xtcp["timer_polls"].asUInt64() == 981u);
    BOOST_TEST(xtcp["timer_events"].asUInt64() == 210u);
    BOOST_TEST(xtcp["output_packets"].asUInt64() == 5100u);
    BOOST_TEST(xtcp["output_bytes"].asUInt64() == 7340032u);
    BOOST_TEST(xtcp["connector_read_bytes"].asUInt64() == 1048576u);
    BOOST_TEST(xtcp["connector_written_bytes"].asUInt64() == 6291456u);
}

BOOST_AUTO_TEST_CASE(stats_json_emits_tcp_stack_and_active_linux_tap_blocks) {
    runtime::RuntimeStatsSample sample;
    sample.requested_tcp_stack = "xtcp";
    sample.active_tcp_stack = "xtcp";
    sample.has_tap_linux = true;
    sample.tap_linux.vnet_header = true;
    sample.tap_linux.gso_merge_active = true;

    const std::string encoded = runtime::SerializeRuntimeStats(sample);
    Json::Value root;
    Json::Reader reader;
    BOOST_REQUIRE(reader.parse(encoded.data(), encoded.data() + encoded.size(), root));
    BOOST_REQUIRE(root.isMember("tcp_stack"));
    BOOST_TEST(root["tcp_stack"]["requested"].asString() == "xtcp");
    BOOST_TEST(root["tcp_stack"]["active"].asString() == "xtcp");
    BOOST_REQUIRE(root.isMember("tap_linux"));
    BOOST_TEST(root["tap_linux"]["vnet_header"].asBool());
    BOOST_TEST(root["tap_linux"]["gso_merge_active"].asBool());
}
