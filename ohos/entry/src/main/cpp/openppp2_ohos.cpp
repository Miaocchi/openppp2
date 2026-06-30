#include <napi/native_api.h>

#include <ppp/configurations/AppConfiguration.h>
#include <ppp/IDisposable.h>
#include <ppp/io/File.h>
#include <ppp/tap/ITap.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/Socket.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/asio/vdns.h>
#include <ppp/diagnostics/Error.h>
#include <ppp/diagnostics/Stopwatch.h>
#include <ppp/auxiliary/JsonAuxiliary.h>
#include <ppp/auxiliary/StringAuxiliary.h>
#include <ppp/threading/Timer.h>
#include <ppp/threading/Thread.h>
#include <ppp/threading/Executors.h>
#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/app/client/GeoRuleGenerator.h>

#include <linux/ppp/tap/TapLinux.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using ppp::IDisposable;
using ppp::app::client::VEthernetExchanger;
using ppp::app::client::VEthernetNetworkSwitcher;
using ppp::auxiliary::JsonAuxiliary;
using ppp::configurations::AppConfiguration;
using ppp::diagnostics::Stopwatch;
using ppp::net::IPEndPoint;
using ppp::net::Ipep;
using ppp::tap::ITap;
using ppp::threading::Executors;
using ppp::threading::Timer;

namespace {
constexpr const char* LOG_TAG = "openppp2-ohos";

#define OH_LOGI(fmt, ...) std::fprintf(stderr, "[%s] " fmt "\n", LOG_TAG, ##__VA_ARGS__)
#define OH_LOGW(fmt, ...) std::fprintf(stderr, "[%s] " fmt "\n", LOG_TAG, ##__VA_ARGS__)
#define OH_LOGE(fmt, ...) std::fprintf(stderr, "[%s] " fmt "\n", LOG_TAG, ##__VA_ARGS__)

enum {
    LINK_STATE_ESTABLISHED = 0,
    LINK_STATE_UNKNOWN = 1,
    LINK_STATE_CLIENT_UNINITIALIZED = 2,
    LINK_STATE_EXCHANGE_UNINITIALIZED = 3,
    LINK_STATE_RECONNECTING = 4,
    LINK_STATE_CONNECTING = 5,
    LINK_STATE_APPLICATION_UNINITIALIZED = 6,
};

enum {
    ERROR_SUCCESS = 0,
    ERROR_UNKNOWN = 1,
    ERROR_ALLOCATED_MEMORY = 2,
    ERROR_APPLICATION_UNINITIALIZED = 3,
    ERROR_NEW_CONFIGURATION_FAIL = 101,
    ERROR_ARG_CONFIGURATION_STRING_IS_NULL_OR_EMPTY = 102,
    ERROR_ARG_CONFIGURATION_STRING_NOT_IS_JSON_OBJECT_STRING = 103,
    ERROR_ARG_CONFIGURATION_STRING_CONFIGURE_ERROR = 104,
    ERROR_NEW_NETWORKINTERFACE_FAIL = 201,
    ERROR_ARG_TUN_IS_INVALID = 202,
    ERROR_ARG_IP_IS_NULL_OR_EMPTY = 203,
    ERROR_ARG_MASK_IS_NULL_OR_EMPTY = 204,
    ERROR_ARG_IP_IS_NOT_AF_INET_FORMAT = 205,
    ERROR_ARG_MASK_IS_NOT_AF_INET_FORMAT = 206,
    ERROR_ARG_MASK_SUBNET_IP_RANGE_GREATER_65535 = 207,
    ERROR_ARG_IP_IS_INVALID = 208,
    ERROR_IT_IS_RUNING = 301,
    ERROR_NETWORK_INTERFACE_NOT_CONFIGURED = 302,
    ERROR_APP_CONFIGURATION_NOT_CONFIGURED = 303,
    ERROR_OPEN_VETHERNET_FAIL = 304,
    ERROR_OPEN_TUNTAP_FAIL = 305,
    ERROR_VETHERNET_PPPD_THREAD_NOT_RUNING = 306,
    ERROR_IT_IS_NOT_RUNING = 401,
};

struct NetworkInterface final {
    int VTun = -1;
    uint16_t VMux = 0;
    bool VNet = false;
    bool StaticMode = false;
    bool BlockQUIC = false;
    boost::asio::ip::address IPAddress;
    boost::asio::ip::address GatewayServer;
    boost::asio::ip::address SubmaskAddress;
};

static ppp::diagnostics::ErrorCode TranslateError(int err) noexcept {
    using ErrorCode = ppp::diagnostics::ErrorCode;
    switch (err) {
        case ERROR_SUCCESS:
            return ErrorCode::Success;
        case ERROR_ALLOCATED_MEMORY:
        case ERROR_NEW_CONFIGURATION_FAIL:
        case ERROR_NEW_NETWORKINTERFACE_FAIL:
            return ErrorCode::MemoryAllocationFailed;
        case ERROR_APPLICATION_UNINITIALIZED:
            return ErrorCode::AppContextUnavailable;
        case ERROR_ARG_CONFIGURATION_STRING_IS_NULL_OR_EMPTY:
            return ErrorCode::ConfigFieldMissing;
        case ERROR_ARG_CONFIGURATION_STRING_NOT_IS_JSON_OBJECT_STRING:
            return ErrorCode::ConfigFileMalformed;
        case ERROR_ARG_CONFIGURATION_STRING_CONFIGURE_ERROR:
            return ErrorCode::ConfigLoadFailed;
        case ERROR_ARG_TUN_IS_INVALID:
            return ErrorCode::TunnelDeviceMissing;
        case ERROR_ARG_IP_IS_NULL_OR_EMPTY:
        case ERROR_ARG_MASK_IS_NULL_OR_EMPTY:
            return ErrorCode::ConfigFieldMissing;
        case ERROR_ARG_IP_IS_NOT_AF_INET_FORMAT:
        case ERROR_ARG_IP_IS_INVALID:
            return ErrorCode::NetworkAddressInvalid;
        case ERROR_ARG_MASK_IS_NOT_AF_INET_FORMAT:
        case ERROR_ARG_MASK_SUBNET_IP_RANGE_GREATER_65535:
            return ErrorCode::NetworkMaskInvalid;
        case ERROR_IT_IS_RUNING:
            return ErrorCode::AppAlreadyRunning;
        case ERROR_NETWORK_INTERFACE_NOT_CONFIGURED:
            return ErrorCode::NetworkInterfaceUnavailable;
        case ERROR_APP_CONFIGURATION_NOT_CONFIGURED:
            return ErrorCode::AppConfigurationMissing;
        case ERROR_OPEN_VETHERNET_FAIL:
            return ErrorCode::NetworkInterfaceOpenFailed;
        case ERROR_OPEN_TUNTAP_FAIL:
            return ErrorCode::TunnelOpenFailed;
        case ERROR_VETHERNET_PPPD_THREAD_NOT_RUNING:
            return ErrorCode::RuntimeThreadStartFailed;
        case ERROR_IT_IS_NOT_RUNING:
            return ErrorCode::AndroidLibInvalidState;
        default:
            return ErrorCode::AndroidLibUnknownFailure;
    }
}

static int SetLastErrorForResult(int err) noexcept {
    if (err == ERROR_SUCCESS) {
        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::Success);
        return err;
    }
    if (ppp::diagnostics::GetLastErrorCodeSnapshot() == ppp::diagnostics::ErrorCode::Success) {
        ppp::diagnostics::SetLastErrorCode(TranslateError(err));
    }
    return err;
}

static int SetLastErrorAndReturn(ppp::diagnostics::ErrorCode code, int err) noexcept {
    ppp::diagnostics::SetLastErrorCode(code);
    return err;
}

class OpenPpp2Application final {
public:
    static std::shared_ptr<OpenPpp2Application> GetDefault() noexcept {
        struct Domain final {
            Domain() noexcept {
                auto awaitable = ppp::make_shared_object<Executors::Awaitable>();
                if (awaitable == nullptr) {
                    return;
                }
                std::weak_ptr<Executors::Awaitable> awaitable_weak = awaitable;
                std::thread([this, awaitable_weak]() noexcept {
                    ppp::global::cctor();
                    app_ = ppp::make_shared_object<OpenPpp2Application>();
                    if (app_ != nullptr) {
                        auto start = [this, awaitable_weak](int, const char**) noexcept -> int {
                            if (auto locked = awaitable_weak.lock()) {
                                locked->Processed();
                            }
                            app_->DllMain();
                            return 0;
                        };
                        Executors::Run(nullptr, start);
                    }
                }).detach();
                awaitable->Await();
            }
            std::shared_ptr<OpenPpp2Application> app_;
        };
        static Domain domain;
        return domain.app_;
    }

    void DllMain() noexcept {
        int max_concurrent = ppp::GetProcesserCount();
        if (max_concurrent > 1) {
            Executors::SetMaxSchedulers(max_concurrent);
        }
    }

    static int Invoke(const ppp::function<int()>& task) noexcept {
        auto context = Executors::GetDefault();
        if (context == nullptr) {
            return SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::RuntimeIoContextMissing,
                                         ERROR_VETHERNET_PPPD_THREAD_NOT_RUNING);
        }
        auto awaitable = ppp::make_shared_object<Executors::Awaitable>();
        if (awaitable == nullptr) {
            return SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::MemoryAllocationFailed,
                                         ERROR_ALLOCATED_MEMORY);
        }

        int err = ERROR_UNKNOWN;
        boost::asio::post(*context, [awaitable, &err, task]() noexcept {
            err = task();
            awaitable->Processed();
        });
        if (!awaitable->Await()) {
            return SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::RuntimeEventDispatchFailed, ERROR_UNKNOWN);
        }
        return err;
    }

    static bool Timeout() noexcept {
        auto context = Executors::GetDefault();
        auto app = GetDefault();
        if (context == nullptr || app == nullptr || std::atomic_load(&app->client_) == nullptr) {
            return false;
        }

        auto timeout = Timer::Timeout(context, 1000, [](Timer*) noexcept {
            auto app = GetDefault();
            if (app != nullptr) {
                app->timeout_.reset();
                Timeout();
            }
        });
        if (timeout == nullptr) {
            return false;
        }
        app->timeout_ = std::move(timeout);
        app->OnTick();
        return true;
    }

    bool Release() noexcept {
        bool any = false;
        auto timeout = std::move(timeout_);
        if (timeout != nullptr) {
            timeout->Dispose();
        }

        auto client = std::atomic_exchange(&client_, std::shared_ptr<VEthernetNetworkSwitcher>());
        if (client != nullptr) {
            any = true;
            client->Dispose();
        }

        configuration_.reset();
        network_interface_.reset();
        bypass_ip_list_.reset();
        dns_rules_list_.reset();
        transmission_statistics_.Clear();
        statistics_json_ = "{}";
        stopwatch_.Reset();
        return any;
    }

    void OnTick() noexcept {
        ReportTransmissionStatistics();
    }

    void ReportTransmissionStatistics() noexcept {
        uint64_t incoming_traffic = 0;
        uint64_t outgoing_traffic = 0;
        std::shared_ptr<ppp::transmissions::ITransmissionStatistics> snapshot;
        if (!GetTransmissionStatistics(incoming_traffic, outgoing_traffic, snapshot)) {
            incoming_traffic = 0;
            outgoing_traffic = 0;
            snapshot = nullptr;
        }

        Json::Value json;
        json["tx"] = stl::to_string<ppp::string>(outgoing_traffic);
        json["rx"] = stl::to_string<ppp::string>(incoming_traffic);
        if (snapshot) {
            json["in"] = stl::to_string<ppp::string>(snapshot->IncomingTraffic.load());
            json["out"] = stl::to_string<ppp::string>(snapshot->OutgoingTraffic.load());
        }
        statistics_json_ = JsonAuxiliary::ToStyledString(json);
    }

    bool GetTransmissionStatistics(uint64_t& incoming_traffic,
                                   uint64_t& outgoing_traffic,
                                   std::shared_ptr<ppp::transmissions::ITransmissionStatistics>& snapshot) noexcept {
        snapshot = nullptr;
        incoming_traffic = 0;
        outgoing_traffic = 0;
        auto client = std::atomic_load(&client_);
        if (client != nullptr && !client->IsDisposed()) {
            auto statistics = client->GetStatistics();
            if (statistics != nullptr) {
                return ppp::transmissions::ITransmissionStatistics::GetTransmissionStatistics(
                    statistics, transmission_statistics_, incoming_traffic, outgoing_traffic, snapshot);
            }
        }
        return false;
    }

public:
    std::shared_ptr<Timer> timeout_;
    Stopwatch stopwatch_;
    std::shared_ptr<VEthernetNetworkSwitcher> client_;
    std::shared_ptr<AppConfiguration> configuration_;
    std::shared_ptr<NetworkInterface> network_interface_;
    std::shared_ptr<ppp::string> bypass_ip_list_;
    std::shared_ptr<ppp::string> dns_rules_list_;
    ppp::transmissions::ITransmissionStatistics transmission_statistics_;
    ppp::string statistics_json_ = "{}";
};

static bool ProtectSocketFd(int fd) noexcept {
    if (fd < 0) {
        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtectorNetworkProtectInvalidSocket);
        return false;
    }
    // HarmonyOS VPN socket protection must be validated against the concrete
    // device SDK. This callback keeps the OpenPPP2 data plane from crashing
    // while the ArkTS VpnConnection.protect(fd) path is finalized.
    return true;
}

static int GetLinkStateCore() noexcept {
    using NetworkState = VEthernetExchanger::NetworkState;
    auto app = OpenPpp2Application::GetDefault();
    if (app == nullptr) {
        return LINK_STATE_APPLICATION_UNINITIALIZED;
    }
    auto client = std::atomic_load(&app->client_);
    if (client == nullptr) {
        return LINK_STATE_CLIENT_UNINITIALIZED;
    }
    auto exchanger = client->GetExchanger();
    if (exchanger == nullptr) {
        return LINK_STATE_EXCHANGE_UNINITIALIZED;
    }
    NetworkState network_state = exchanger->GetNetworkState();
    if (network_state == NetworkState::NetworkState_Connecting) {
        return LINK_STATE_CONNECTING;
    }
    if (network_state == NetworkState::NetworkState_Reconnecting) {
        return LINK_STATE_RECONNECTING;
    }
    if (network_state == NetworkState::NetworkState_Established) {
        return LINK_STATE_ESTABLISHED;
    }
    return LINK_STATE_UNKNOWN;
}

static std::shared_ptr<ITap> CreateTap(std::shared_ptr<boost::asio::io_context> context,
                                       std::shared_ptr<NetworkInterface> network_interface) noexcept {
    if (context == nullptr || network_interface == nullptr || network_interface->VTun == -1) {
        return nullptr;
    }
    uint32_t ip = IPEndPoint::ToEndPoint(
        boost::asio::ip::tcp::endpoint(network_interface->IPAddress, IPEndPoint::MinPort)).GetAddress();
    uint32_t mask = IPEndPoint::ToEndPoint(
        boost::asio::ip::tcp::endpoint(network_interface->SubmaskAddress, IPEndPoint::MinPort)).GetAddress();
    uint32_t gw = IPEndPoint::ToEndPoint(
        boost::asio::ip::tcp::endpoint(network_interface->GatewayServer, IPEndPoint::MinPort)).GetAddress();

    ppp::string dev = ITap::FindAnyDevice();
    if (dev.empty()) {
        dev = "ohos-tun";
    }
    void* tun = reinterpret_cast<void*>(static_cast<std::intptr_t>(network_interface->VTun));
    return ppp::tap::TapLinux::From(context, dev, tun, ip, gw, mask, true, true);
}

static int OpenSwitcher(std::shared_ptr<boost::asio::io_context> context,
                        std::shared_ptr<OpenPpp2Application> app,
                        std::shared_ptr<ITap> tap,
                        std::shared_ptr<VEthernetNetworkSwitcher>& client,
                        std::shared_ptr<NetworkInterface> network_interface,
                        std::shared_ptr<AppConfiguration> configuration) noexcept {
    bool lwip = false;
    int max_concurrent = ppp::GetProcesserCount();
    client = ppp::make_shared_object<VEthernetNetworkSwitcher>(
        context, lwip, network_interface->VNet, max_concurrent > 1, configuration);
    if (client == nullptr) {
        return ERROR_ALLOCATED_MEMORY;
    }
    client->Mux(&network_interface->VMux);
    client->StaticMode(&network_interface->StaticMode);

    const bool proxy_only_runtime = configuration->client.proxy_only;
    if (proxy_only_runtime) {
        configuration->ApplyProxyModeDefaults();
        bool proxy_only_flag = true;
        client->ProxyOnly(&proxy_only_flag);
    }

    ppp::string user_bypass_text;
    if (app->bypass_ip_list_ != nullptr) {
        user_bypass_text = *app->bypass_ip_list_;
    }
    if (!proxy_only_runtime && app->dns_rules_list_ != nullptr) {
        client->LoadAllDnsRules(*app->dns_rules_list_, false);
    }

    if (!proxy_only_runtime && configuration->geo_rules.enabled) {
        auto geo_result = ppp::app::client::GeoRuleGenerator::Generate(*configuration, nullptr);
        if (!geo_result.output_bypass_path.empty()) {
            ppp::string geo_bypass_text = ppp::io::File::ReadAllText(geo_result.output_bypass_path.data());
            if (!geo_bypass_text.empty()) {
                if (!user_bypass_text.empty() && user_bypass_text.back() != '\n') {
                    user_bypass_text.push_back('\n');
                }
                user_bypass_text += geo_bypass_text;
            }
        }
        if (!geo_result.output_dns_rules_path.empty()) {
            client->LoadAllDnsRules(geo_result.output_dns_rules_path, true);
        }
    }

    if (!proxy_only_runtime && !user_bypass_text.empty()) {
        client->SetBypassIpList(std::move(user_bypass_text));
    }

    bool ok = client->Open(tap);
    if (!ok) {
        if (ppp::diagnostics::GetLastErrorCodeSnapshot() == ppp::diagnostics::ErrorCode::Success) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::NetworkInterfaceOpenFailed);
        }
        return ERROR_OPEN_VETHERNET_FAIL;
    }

    auto protector = client->GetProtectorNetwork();
    if (protector != nullptr) {
        protector->ProtectEvent = [](int fd) noexcept {
            return ProtectSocketFd(fd);
        };
    }

    std::atomic_store(&app->client_, client);
    app->stopwatch_.Restart();
    OpenPpp2Application::Timeout();
    return ERROR_SUCCESS;
}

static int TryOpenSwitcher(std::shared_ptr<boost::asio::io_context> context,
                           std::shared_ptr<VEthernetNetworkSwitcher>& ethernet) noexcept {
    auto app = OpenPpp2Application::GetDefault();
    if (app == nullptr) {
        return SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::AppContextUnavailable,
                                     ERROR_APPLICATION_UNINITIALIZED);
    }
    if (std::atomic_load(&app->client_) != nullptr) {
        return SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::AppAlreadyRunning, ERROR_IT_IS_RUNING);
    }
    auto network_interface = app->network_interface_;
    if (network_interface == nullptr) {
        return SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::NetworkInterfaceUnavailable,
                                     ERROR_NETWORK_INTERFACE_NOT_CONFIGURED);
    }
    auto configuration = app->configuration_;
    if (configuration == nullptr) {
        return SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::AppConfigurationMissing,
                                     ERROR_APP_CONFIGURATION_NOT_CONFIGURED);
    }

    auto tap = CreateTap(context, network_interface);
    if (tap == nullptr) {
        return SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::TunnelOpenFailed, ERROR_OPEN_TUNTAP_FAIL);
    }

    std::shared_ptr<VEthernetNetworkSwitcher> client;
    int err = OpenSwitcher(context, app, tap, client, network_interface, configuration);
    if (err == ERROR_SUCCESS) {
        ethernet = client;
    } else {
        IDisposable::DisposeReferences(tap, client);
    }
    return err;
}

static int RunCore(int key) noexcept {
    (void)key;
    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::Success);
    auto context = ppp::make_shared_object<boost::asio::io_context>();
    if (context == nullptr) {
        return SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::MemoryAllocationFailed, ERROR_ALLOCATED_MEMORY);
    }

    int err = ERROR_SUCCESS;
    boost::asio::post(*context, [&err, context]() noexcept {
        auto app = OpenPpp2Application::GetDefault();
        if (app == nullptr) {
            err = ERROR_APPLICATION_UNINITIALIZED;
            context->stop();
            return;
        }
        std::shared_ptr<VEthernetNetworkSwitcher> ethernet;
        err = TryOpenSwitcher(context, ethernet);
        if (err != ERROR_SUCCESS && err != ERROR_IT_IS_RUNING) {
            app->Release();
            context->stop();
        }
    });

    auto work = boost::asio::make_work_guard(*context);
    boost::system::error_code ec;
    context->restart();
    for (;;) {
        try {
            context->run();
            break;
        } catch (const std::exception& e) {
            OH_LOGE("run handler exception: %s", e.what());
        } catch (...) {
            OH_LOGE("run handler exception: <unknown>");
        }
    }
    return SetLastErrorForResult(err);
}

static int StopCore() noexcept {
    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::Success);
    int err = OpenPpp2Application::Invoke([]() noexcept -> int {
        auto app = OpenPpp2Application::GetDefault();
        if (app == nullptr) {
            return SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::AppContextUnavailable,
                                         ERROR_APPLICATION_UNINITIALIZED);
        }
        if (!app->Release()) {
            return SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::AndroidLibInvalidState,
                                         ERROR_IT_IS_NOT_RUNING);
        }
        return ERROR_SUCCESS;
    });
    return SetLastErrorForResult(err);
}

static std::string GetString(napi_env env, napi_value value) {
    size_t length = 0;
    napi_get_value_string_utf8(env, value, nullptr, 0, &length);
    std::vector<char> buffer(length + 1, '\0');
    size_t copied = 0;
    napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(), &copied);
    return std::string(buffer.data(), copied);
}

static napi_value MakeUndefined(napi_env env) {
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

static napi_value MakeBool(napi_env env, bool value) {
    napi_value result;
    napi_get_boolean(env, value, &result);
    return result;
}

static napi_value MakeInt(napi_env env, int32_t value) {
    napi_value result;
    napi_create_int32(env, value, &result);
    return result;
}

static napi_value MakeString(napi_env env, const std::string& value) {
    napi_value result;
    napi_create_string_utf8(env, value.c_str(), value.size(), &result);
    return result;
}

static napi_value GetProperty(napi_env env, napi_value object, const char* name) {
    napi_value key;
    napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &key);
    napi_value value;
    napi_get_property(env, object, key, &value);
    return value;
}

static int32_t GetIntProperty(napi_env env, napi_value object, const char* name, int32_t fallback = 0) {
    napi_value value = GetProperty(env, object, name);
    napi_valuetype type;
    napi_typeof(env, value, &type);
    if (type != napi_number) {
        return fallback;
    }
    int32_t result = fallback;
    napi_get_value_int32(env, value, &result);
    return result;
}

static bool GetBoolProperty(napi_env env, napi_value object, const char* name, bool fallback = false) {
    napi_value value = GetProperty(env, object, name);
    napi_valuetype type;
    napi_typeof(env, value, &type);
    if (type != napi_boolean) {
        return fallback;
    }
    bool result = fallback;
    napi_get_value_bool(env, value, &result);
    return result;
}

static std::string GetStringProperty(napi_env env, napi_value object, const char* name,
                                     const char* fallback = "") {
    napi_value value = GetProperty(env, object, name);
    napi_valuetype type;
    napi_typeof(env, value, &type);
    if (type != napi_string) {
        return fallback;
    }
    return GetString(env, value);
}

static napi_value NapiGetDefaultCipherSuites(napi_env env, napi_callback_info info) {
    (void)info;
    OpenPpp2Application::GetDefault();
    return MakeString(env, ppp::GetDefaultCipherSuites());
}

static napi_value NapiSetRootPath(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        return MakeBool(env, false);
    }
    std::string path = GetString(env, args[0]);
    if (path.empty()) {
        return MakeBool(env, false);
    }
    int rc = chdir(path.c_str());
    OH_LOGI("setRootPath chdir(%s) rc=%d errno=%d", path.c_str(), rc, errno);
    return MakeBool(env, rc == 0);
}

static napi_value NapiSetAppConfiguration(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::ConfigFieldMissing,
                                                  ERROR_ARG_CONFIGURATION_STRING_IS_NULL_OR_EMPTY));
    }
    std::string json_string = GetString(env, args[0]);
    if (json_string.empty()) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::ConfigFieldMissing,
                                                  ERROR_ARG_CONFIGURATION_STRING_IS_NULL_OR_EMPTY));
    }

    auto config = ppp::make_shared_object<AppConfiguration>();
    if (config == nullptr) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::MemoryAllocationFailed,
                                                  ERROR_NEW_CONFIGURATION_FAIL));
    }

    Json::Value json = JsonAuxiliary::FromString(json_string.c_str(), static_cast<int>(json_string.size()));
    if (!json.isObject()) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::ConfigFileMalformed,
                                                  ERROR_ARG_CONFIGURATION_STRING_NOT_IS_JSON_OBJECT_STRING));
    }
    if (!config->Load(json)) {
        return MakeInt(env, SetLastErrorForResult(ERROR_ARG_CONFIGURATION_STRING_CONFIGURE_ERROR));
    }

    auto app = OpenPpp2Application::GetDefault();
    if (app == nullptr) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::AppContextUnavailable,
                                                  ERROR_APPLICATION_UNINITIALIZED));
    }
    int err = OpenPpp2Application::Invoke([app, config]() noexcept {
        ppp::net::asio::vdns::ttl = config->udp.dns.ttl;
        ppp::net::asio::vdns::enabled = config->udp.dns.turbo;
        app->configuration_ = config;
        return ERROR_SUCCESS;
    });
    return MakeInt(env, SetLastErrorForResult(err));
}

static napi_value NapiGetAppConfiguration(napi_env env, napi_callback_info info) {
    (void)info;
    std::shared_ptr<ppp::string> json;
    int err = OpenPpp2Application::Invoke([&json]() noexcept {
        auto app = OpenPpp2Application::GetDefault();
        if (app != nullptr && app->configuration_ != nullptr) {
            json = ppp::make_shared_object<ppp::string>(app->configuration_->ToString());
        }
        return ERROR_SUCCESS;
    });
    if (err != ERROR_SUCCESS || json == nullptr) {
        SetLastErrorForResult(err);
        return MakeString(env, "");
    }
    return MakeString(env, std::string(json->data(), json->size()));
}

static napi_value NapiSetNetworkInterface(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::TunnelDeviceMissing,
                                                  ERROR_ARG_TUN_IS_INVALID));
    }

    int tun = GetIntProperty(env, args[0], "tunFd", -1);
    if (tun == -1) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::TunnelDeviceMissing,
                                                  ERROR_ARG_TUN_IS_INVALID));
    }

    std::string ip_string = GetStringProperty(env, args[0], "ip");
    std::string mask_string = GetStringProperty(env, args[0], "mask");
    std::string gw_string = GetStringProperty(env, args[0], "gateway");
    if (ip_string.empty()) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::ConfigFieldMissing,
                                                  ERROR_ARG_IP_IS_NULL_OR_EMPTY));
    }
    if (mask_string.empty()) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::ConfigFieldMissing,
                                                  ERROR_ARG_MASK_IS_NULL_OR_EMPTY));
    }

    boost::system::error_code ec;
    auto ip_address = ppp::StringToAddress(ip_string.data(), ec);
    if (ec || !ip_address.is_v4()) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::NetworkAddressInvalid,
                                                  ERROR_ARG_IP_IS_NOT_AF_INET_FORMAT));
    }
    ec.clear();
    auto mask_address = ppp::StringToAddress(mask_string.data(), ec);
    if (ec || !mask_address.is_v4()) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::NetworkMaskInvalid,
                                                  ERROR_ARG_MASK_IS_NOT_AF_INET_FORMAT));
    }

    uint32_t addresses[2] = {
        IPEndPoint::ToEndPoint(boost::asio::ip::tcp::endpoint(ip_address, IPEndPoint::MinPort)).GetAddress(),
        IPEndPoint::ToEndPoint(boost::asio::ip::tcp::endpoint(mask_address, IPEndPoint::MinPort)).GetAddress(),
    };
    int prefix = IPEndPoint::NetmaskToPrefix(addresses[1]);
    if (prefix < 16) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::NetworkMaskInvalid,
                                                  ERROR_ARG_MASK_SUBNET_IP_RANGE_GREATER_65535));
    }
    if (prefix > 30) {
        addresses[1] = IPEndPoint::NetmaskToPrefix(prefix);
        mask_address = Ipep::ToAddress(addresses[1]);
    }
    if (IPEndPoint::IsInvalid(ip_address)) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::NetworkAddressInvalid,
                                                  ERROR_ARG_IP_IS_INVALID));
    }

    boost::asio::ip::address gw_address;
    if (!gw_string.empty()) {
        ec.clear();
        gw_address = ppp::StringToAddress(gw_string.data(), ec);
        if (ec || !gw_address.is_v4()) {
            gw_address = ppp::net::Ipep::FixedIPAddress(ip_address, mask_address);
        }
    } else {
        gw_address = ppp::net::Ipep::FixedIPAddress(ip_address, mask_address);
    }
    ip_address = Ipep::FixedIPAddress(ip_address, gw_address, mask_address);

    auto network_interface = ppp::make_shared_object<NetworkInterface>();
    if (network_interface == nullptr) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::MemoryAllocationFailed,
                                                  ERROR_NEW_NETWORKINTERFACE_FAIL));
    }
    network_interface->VTun = tun;
    network_interface->VMux = static_cast<uint16_t>(
        std::min<int>(std::max<int>(0, GetIntProperty(env, args[0], "mux", 0)), UINT16_MAX));
    network_interface->VNet = GetBoolProperty(env, args[0], "vnet", false);
    network_interface->BlockQUIC = GetBoolProperty(env, args[0], "blockQuic", false);
    network_interface->StaticMode = GetBoolProperty(env, args[0], "staticMode", false);
    network_interface->IPAddress = ip_address;
    network_interface->GatewayServer = gw_address;
    network_interface->SubmaskAddress = mask_address;

    auto app = OpenPpp2Application::GetDefault();
    if (app == nullptr) {
        return MakeInt(env, SetLastErrorAndReturn(ppp::diagnostics::ErrorCode::AppContextUnavailable,
                                                  ERROR_APPLICATION_UNINITIALIZED));
    }
    int err = OpenPpp2Application::Invoke([app, network_interface]() noexcept {
        app->network_interface_ = network_interface;
        return ERROR_SUCCESS;
    });
    return MakeInt(env, SetLastErrorForResult(err));
}

static napi_value NapiGetNetworkInterface(napi_env env, napi_callback_info info) {
    (void)info;
    std::string json_string;
    int err = OpenPpp2Application::Invoke([&json_string]() noexcept {
        auto app = OpenPpp2Application::GetDefault();
        if (app != nullptr && app->network_interface_ != nullptr) {
            auto ni = app->network_interface_;
            Json::Value json;
            json["block-quic"] = ni->BlockQUIC;
            json["tun"] = ni->VTun;
            json["mux"] = ni->VMux;
            json["vnet"] = ni->VNet;
            json["static"] = ni->StaticMode;
            json["gw"] = stl::transform<ppp::string>(ni->GatewayServer.to_string());
            json["ip"] = stl::transform<ppp::string>(ni->IPAddress.to_string());
            json["mask"] = stl::transform<ppp::string>(ni->SubmaskAddress.to_string());
            json_string = JsonAuxiliary::ToStyledString(json);
        }
        return ERROR_SUCCESS;
    });
    SetLastErrorForResult(err);
    return MakeString(env, json_string);
}

static napi_value NapiSetBypassIpList(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    auto app = OpenPpp2Application::GetDefault();
    if (app == nullptr || argc < 1) {
        return MakeBool(env, false);
    }
    app->bypass_ip_list_ = ppp::make_shared_object<ppp::string>(GetString(env, args[0]));
    return MakeBool(env, true);
}

static napi_value NapiSetDnsRulesList(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    auto app = OpenPpp2Application::GetDefault();
    if (app == nullptr || argc < 1) {
        return MakeBool(env, false);
    }
    app->dns_rules_list_ = ppp::make_shared_object<ppp::string>(GetString(env, args[0]));
    return MakeBool(env, true);
}

struct RunWork final {
    napi_env env = nullptr;
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    int key = 0;
    int result = ERROR_UNKNOWN;
};

static napi_value NapiRun(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    auto* data = new RunWork();
    data->env = env;
    if (argc > 0) {
        napi_get_value_int32(env, args[0], &data->key);
    }

    napi_value promise;
    napi_create_promise(env, &data->deferred, &promise);

    napi_value resource_name;
    napi_create_string_utf8(env, "openppp2.run", NAPI_AUTO_LENGTH, &resource_name);
    napi_create_async_work(
        env,
        nullptr,
        resource_name,
        [](napi_env, void* raw) {
            auto* data = static_cast<RunWork*>(raw);
            data->result = RunCore(data->key);
        },
        [](napi_env env, napi_status status, void* raw) {
            auto* data = static_cast<RunWork*>(raw);
            napi_value value;
            napi_create_int32(env, status == napi_ok ? data->result : ERROR_UNKNOWN, &value);
            napi_resolve_deferred(env, data->deferred, value);
            napi_delete_async_work(env, data->work);
            delete data;
        },
        data,
        &data->work);
    napi_queue_async_work(env, data->work);
    return promise;
}

static napi_value NapiStop(napi_env env, napi_callback_info info) {
    (void)info;
    return MakeInt(env, StopCore());
}

static napi_value NapiClearConfigure(napi_env env, napi_callback_info info) {
    (void)info;
    OpenPpp2Application::Invoke([]() noexcept {
        auto app = OpenPpp2Application::GetDefault();
        if (app != nullptr) {
            app->bypass_ip_list_.reset();
            app->dns_rules_list_.reset();
            app->configuration_.reset();
            app->network_interface_.reset();
        }
        return ERROR_SUCCESS;
    });
    return MakeUndefined(env);
}

static napi_value NapiGetLinkState(napi_env env, napi_callback_info info) {
    (void)info;
    int status = LINK_STATE_UNKNOWN;
    int err = OpenPpp2Application::Invoke([&status]() noexcept {
        status = GetLinkStateCore();
        return ERROR_SUCCESS;
    });
    if (err != ERROR_SUCCESS) {
        SetLastErrorForResult(err);
        return MakeInt(env, LINK_STATE_APPLICATION_UNINITIALIZED);
    }
    return MakeInt(env, status);
}

static napi_value NapiGetStatistics(napi_env env, napi_callback_info info) {
    (void)info;
    std::string statistics = "{}";
    OpenPpp2Application::Invoke([&statistics]() noexcept {
        auto app = OpenPpp2Application::GetDefault();
        if (app != nullptr) {
            app->ReportTransmissionStatistics();
            statistics = app->statistics_json_;
        }
        return ERROR_SUCCESS;
    });
    return MakeString(env, statistics);
}

static napi_value NapiGetLastErrorCode(napi_env env, napi_callback_info info) {
    (void)info;
    auto code = ppp::diagnostics::GetLastErrorCodeSnapshot();
    return MakeInt(env, static_cast<int32_t>(static_cast<uint32_t>(code)));
}

static napi_value NapiGetLastErrorText(napi_env env, napi_callback_info info) {
    (void)info;
    auto code = ppp::diagnostics::GetLastErrorCodeSnapshot();
    const char* text = ppp::diagnostics::FormatErrorString(code);
    return MakeString(env, text != nullptr ? text : "");
}

static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor properties[] = {
        {"getDefaultCipherSuites", nullptr, NapiGetDefaultCipherSuites, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setRootPath", nullptr, NapiSetRootPath, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setAppConfiguration", nullptr, NapiSetAppConfiguration, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getAppConfiguration", nullptr, NapiGetAppConfiguration, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setNetworkInterface", nullptr, NapiSetNetworkInterface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getNetworkInterface", nullptr, NapiGetNetworkInterface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setBypassIpList", nullptr, NapiSetBypassIpList, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDnsRulesList", nullptr, NapiSetDnsRulesList, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"run", nullptr, NapiRun, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, NapiStop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"clearConfigure", nullptr, NapiClearConfigure, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getLinkState", nullptr, NapiGetLinkState, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getStatistics", nullptr, NapiGetStatistics, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getLastErrorCode", nullptr, NapiGetLastErrorCode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getLastErrorText", nullptr, NapiGetLastErrorText, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
    return exports;
}

} // namespace

static napi_module openppp2Module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "openppp2",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterOpenPpp2Module()
{
    napi_module_register(&openppp2Module);
}
