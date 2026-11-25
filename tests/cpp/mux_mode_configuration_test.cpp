#include <ppp/configurations/AppConfiguration.h>
#include <json/json.h>

#include <iostream>
#include <stdexcept>

namespace {
using ppp::configurations::AppConfiguration;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

ppp::string LoadModeValue(const Json::Value* mode) {
    Json::Value json(Json::objectValue);
    if (NULLPTR != mode) {
        json["mux"]["mode"] = *mode;
    }

    AppConfiguration config;
    Require(config.Load(json), "configuration with mux.mode must load");
    Require(config.GetEffectiveMuxMode() == config.mux.mode, "effective mode must follow mux.mode without an override");
    Require(config.ToJson()["mux"]["mode"].asString() == config.mux.mode.data(), "normalized mux.mode must serialize");
    return config.mux.mode;
}

ppp::string LoadMode(const char* mode) {
    Json::Value value(mode);
    return LoadModeValue(&value);
}

void CanonicalModes() {
    Require(LoadMode("compat") == "compat", "compat must stay compat");
    Require(LoadMode("flow") == "flow", "flow must stay flow");
    Require(LoadMode("balance") == "balance", "balance must stay balance");
    Require(LoadMode("stripe") == "stripe", "stripe must stay stripe");
}

void Aliases() {
    Require(LoadMode("legacy") == "compat", "legacy must map to compat");
    Require(LoadMode("default") == "compat", "default must map to compat");
    Require(LoadMode("flow-v1") == "flow", "flow-v1 must map to flow");
    Require(LoadMode("primary") == "flow", "primary must map to flow");
    Require(LoadMode("primary-link") == "flow", "primary-link must map to flow");
    Require(LoadMode("balanced") == "balance", "balanced must map to balance");
    Require(LoadMode("lb") == "balance", "lb must map to balance");
    Require(LoadMode("load-balance") == "balance", "load-balance must map to balance");
    Require(LoadMode("striped") == "stripe", "striped must map to stripe");
    Require(LoadMode("striping") == "stripe", "striping must map to stripe");
}

void CaseAndWhitespace() {
    Require(LoadMode("  FLOW ") == "flow", "mode must be trimmed and case-insensitive");
    Require(LoadMode("Balance") == "balance", "mixed-case balance must normalize");
}

void FallbackToCompat() {
    Require(LoadModeValue(NULLPTR) == "compat", "missing mux.mode must default to compat");
    Require(LoadMode("") == "compat", "empty mux.mode must default to compat");
    Require(LoadMode("turbo") == "compat", "unknown mux.mode must fall back to compat");
    Require(LoadMode("flow_v2") == "compat", "receiver ordering names are not scheduler modes");
}

void IntentConfiguration() {
    Json::Value json(Json::objectValue);
    json["mux"]["intent"]["enabled"] = true;
    json["mux"]["intent"]["mode"] = "behavior";
    json["mux"]["intent"]["window_ms"] = 400;
    json["mux"]["intent"]["min_samples"] = 32768;
    json["mux"]["intent"]["hysteresis"] = 25;
    json["mux"]["intent"]["hold_ms"] = 7000;
    json["mux"]["intent"]["rules"] = Json::arrayValue;
    json["mux"]["intent"]["rules"][0]["intent"] = "bandwidth";
    json["mux"]["intent"]["rules"][0]["ports"].append(443);

    AppConfiguration config;
    Require(config.Load(json), "intent configuration must load");
    Require(config.mux.intent.enabled, "intent must be enabled");
    Require(config.mux.intent.mode == "behavior", "intent mode must round-trip");
    Require(config.mux.intent.window_ms == 400 && config.mux.intent.min_samples == 32768,
        "intent sampling values must round-trip");
    Require(config.mux.intent.rules.size() == 1 && config.mux.intent.rules[0].ports[0] == 443,
        "intent port rule must round-trip");
    const auto serialized = config.ToJson()["mux"]["intent"];
    Require(serialized["enabled"].asBool() && serialized["rules"][0]["ports"][0].asInt() == 443,
        "intent configuration must serialize");
}
}

int main() {
    try {
        CanonicalModes(); Aliases(); CaseAndWhitespace(); FallbackToCompat(); IntentConfiguration();
        std::cout << "mux mode configuration tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    return 0;
}
