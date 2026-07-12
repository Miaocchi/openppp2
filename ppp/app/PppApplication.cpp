#include <ppp/configurations/AppConfiguration.h>
#include <ppp/app/PppApplicationInternal.h>
#include <ppp/app/runtime/RuntimeReadiness.h>
#include <ppp/app/server/VirtualEthernetSwitcher.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/diagnostics/Error.h>
#include <ppp/diagnostics/Telemetry.h>

#include <algorithm>

namespace ppp::app {

using server::VirtualEthernetSwitcher;
using client::VEthernetNetworkSwitcher;

std::shared_ptr<PppApplication> DEFAULT_;
std::atomic<bool> GLOBAL_RESTART{false};
std::atomic<bool> GLOBAL_VBGP{false};
std::atomic<uint64_t> GLOBAL_VBGP_LAST{0};
std::atomic<bool> GLOBAL_VIRR{false};
std::atomic<uint64_t> GLOBAL_VIRR_NEXT{0};
ApplicationGlobals GLOBAL_;

PppApplication& PppApplication::GetInstance() noexcept {
    static std::shared_ptr<PppApplication> instance = ppp::make_shared_object<PppApplication>();
    if (DEFAULT_ != instance) {
        DEFAULT_ = instance;
    }
    return *instance;
}

int PppApplication::Run(int argc, char** argv) noexcept {
    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::Success);

    ppp::RT = ppp::ToBoolean(ppp::GetCommandArgument("--rt", argc, const_cast<const char**>(argv), "y").data());
    ppp::global::cctor();

#if BOOST_ASIO_HAS_IO_URING != 0
    if (!ppp::diagnostics::IfIOUringKernelVersion()) {
        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeEnvironmentInvalid);
        return -1;
    }
#endif

    std::shared_ptr<PppApplication> app = DEFAULT_;
    if (NULLPTR == app) {
        app = ppp::make_shared_object<PppApplication>();
        if (NULLPTR == app) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::MemoryAllocationFailed);
            return -1;
        }
        DEFAULT_ = app;
    }

    int prepared_status = app->PreparedArgumentEnvironment(argc, const_cast<const char**>(argv));
    int result_code = Executors::Run(
        app->GetBufferAllocator(),
        [app, prepared_status](int inner_argc, const char* inner_argv[]) noexcept -> int {
            int rc = RunPreparedApplication(app, prepared_status, inner_argc, inner_argv);
#if defined(_WIN32)
            if (rc != 0) {
                ppp::win32::Win32Native::PauseWindowsConsole();
            }
#endif
            return rc;
        },
        argc,
        const_cast<const char**>(argv));

    app->Release();
    ppp::telemetry::Shutdown();

    if (GLOBAL_RESTART.load(std::memory_order_relaxed)) {
#if defined(_WIN32)
        ppp::string command_line = "\"" + ppp::string(*argv) + "\"";
        for (int i = 1; i < argc; ++i) {
            command_line += " \"" + ppp::string(argv[i]) + "\"";
        }

        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        if (CreateProcessA(NULLPTR, command_line.data(), NULLPTR, NULLPTR, FALSE, 0, NULLPTR, NULLPTR, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
#else
        execvp(*argv, argv);
#endif
    }

    return result_code;
}

std::shared_ptr<PppApplication> PppApplication::GetDefault() noexcept {
    return DEFAULT_;
}

std::shared_ptr<ppp::app::runtime::RuntimeSnapshotPublisher> PppApplication::GetRuntimeSnapshotPublisher() noexcept {
    std::lock_guard<std::mutex> scope(runtime_state_mutex_);
    if (NULLPTR == runtime_snapshot_publisher_) {
        runtime_snapshot_publisher_ = ppp::make_shared_object<ppp::app::runtime::RuntimeSnapshotPublisher>();
    }
    return runtime_snapshot_publisher_;
}

std::uint64_t PppApplication::BeginRuntimeGeneration() noexcept {
    std::lock_guard<std::mutex> scope(runtime_state_mutex_);
    ++runtime_generation_;
    if (0 == runtime_generation_) {
        runtime_generation_ = 1;
    }
    runtime_stop_publish_started_ = false;
    runtime_information_observed_ = false;
    runtime_error_snapshot_ = ppp::app::runtime::RuntimeError();
    return runtime_generation_;
}

void PppApplication::PublishRuntimePhase(ppp::app::runtime::RuntimePhase phase) noexcept {
    UpdateAndPublishRuntimeSnapshot(phase);
}

ppp::string PppApplication::BuildRuntimeRole() const noexcept {
    if (client_mode_ || application_mode_ == ApplicationMode::Client) {
        return "client";
    }

    if (application_mode_ == ApplicationMode::Proxy) {
        return "proxy";
    }

    if (application_mode_ == ApplicationMode::Server) {
        return "server";
    }

    return "unknown";
}

void PppApplication::OnRuntimeError(int error_code) noexcept {
    {
        std::lock_guard<std::mutex> scope(runtime_state_mutex_);
        if (0 == runtime_generation_ || runtime_stop_publish_started_) {
            return;
        }
    }

    ppp::app::runtime::RuntimeError snapshot;
    if (!ppp::diagnostics::IsValidErrorCodeValue(error_code)) {
        snapshot.code = 0;
        snapshot.severity = "error";
        snapshot.retryable = true;
        snapshot.user_message_key = "UnknownError";
        snapshot.diagnostic_detail = std::to_string(error_code);
    }
    else {
        ppp::diagnostics::ErrorCode code = static_cast<ppp::diagnostics::ErrorCode>(error_code);
        snapshot.code = static_cast<std::uint32_t>(error_code);
        snapshot.severity = ppp::diagnostics::GetErrorSeverityName(ppp::diagnostics::GetErrorSeverity(code));
        snapshot.retryable = code != ppp::diagnostics::ErrorCode::Success && !ppp::diagnostics::IsErrorFatal(code);
        snapshot.user_message_key = ppp::diagnostics::FormatErrorString(code);
        snapshot.diagnostic_detail = ppp::diagnostics::FormatErrorTriplet(code);
    }

    {
        std::lock_guard<std::mutex> scope(runtime_state_mutex_);
        runtime_error_snapshot_ = std::move(snapshot);
    }

    UpdateAndPublishRuntimeSnapshot(ppp::app::runtime::RuntimePhase::Failed);
}

void PppApplication::RegisterRuntimeErrorCallback() noexcept {
    if (runtime_error_callback_registered_) {
        return;
    }

    std::weak_ptr<PppApplication> self = shared_from_this();
    ppp::diagnostics::RegisterErrorHandler(
        "ppp-runtime-snapshot-error",
        [self](int error_code) noexcept {
            std::shared_ptr<PppApplication> owner = self.lock();
            if (NULLPTR == owner) {
                return;
            }

            owner->OnRuntimeError(error_code);
        });

    runtime_error_callback_registered_ = true;
}

void PppApplication::UnregisterRuntimeErrorCallback() noexcept {
    if (!runtime_error_callback_registered_) {
        return;
    }

    ppp::diagnostics::RegisterErrorHandler("ppp-runtime-snapshot-error", NULLPTR);
    runtime_error_callback_registered_ = false;
}

ppp::app::runtime::RuntimePhase PppApplication::ResolveRuntimePhase(
    const std::shared_ptr<ppp::app::client::VEthernetNetworkSwitcher>& client,
    const std::shared_ptr<ppp::app::client::VEthernetExchanger>& exchanger) noexcept {
    if (NULLPTR == client) {
        return client_mode_
            ? ppp::app::runtime::RuntimePhase::Starting
            : ppp::app::runtime::RuntimePhase::Idle;
    }
    if (NULLPTR == exchanger) {
        return ppp::app::runtime::RuntimePhase::Connecting;
    }

    auto network_state = exchanger->GetNetworkState();
    if (network_state == client::VEthernetExchanger::NetworkState_Reconnecting) {
        return ppp::app::runtime::RuntimePhase::Reconnecting;
    }

    if (network_state == client::VEthernetExchanger::NetworkState_Established) {
        // ponytail: route/dns/policy collapse into peer-info observation until
        // dedicated readiness hooks exist; upgrade path is per-subsystem flags.
        ppp::app::runtime::RuntimeReadiness readiness;
        readiness.session = true;
        readiness.adapter = NULLPTR != client->GetTap();
        readiness.route = runtime_information_observed_;
        readiness.dns = runtime_information_observed_;
        readiness.policy = runtime_information_observed_;
        return ppp::app::runtime::GateConnectedPhase(
            ppp::app::runtime::RuntimePhase::Connected,
            readiness);
    }

    return runtime_information_observed_
        ? ppp::app::runtime::RuntimePhase::Handshaking
        : ppp::app::runtime::RuntimePhase::Connecting;
}

void PppApplication::UpdateAndPublishRuntimeSnapshot(ppp::app::runtime::RuntimePhase phase) noexcept {
    std::shared_ptr<ppp::app::runtime::RuntimeSnapshotPublisher> publisher = GetRuntimeSnapshotPublisher();
    if (NULLPTR == publisher) {
        return;
    }

    ppp::app::runtime::RuntimeSnapshot latest = publisher->GetLatest();
    ppp::app::runtime::RuntimeSnapshot snapshot;

    {
        std::lock_guard<std::mutex> scope(runtime_state_mutex_);
        if (0 == runtime_generation_) {
            return;
        }

        snapshot.generation = runtime_generation_;
        snapshot.schema_version = ppp::app::runtime::RuntimeSnapshot::SchemaVersion;
        snapshot.monotonic_ms = std::max(std::uint64_t(ppp::threading::Executors::GetTickCount()), latest.monotonic_ms);
        snapshot.phase = phase;
        snapshot.last_error = runtime_error_snapshot_;
        snapshot.role = BuildRuntimeRole();
        snapshot.transport = "";
        snapshot.requested_mux_mode = "";
        snapshot.effective_mux_mode = "";
        snapshot.mux_fallback_reason = "";
        snapshot.p2p_state = "";
        snapshot.effective_path = "";

        if (phase == ppp::app::runtime::RuntimePhase::Idle) {
            snapshot.server.clear();
            snapshot.role = snapshot.role.empty() ? "server" : snapshot.role;
            snapshot.transport.clear();
            snapshot.requested_mux_mode.clear();
            snapshot.effective_mux_mode.clear();
            snapshot.mux_fallback_reason.clear();
            snapshot.p2p_state.clear();
            snapshot.effective_path.clear();
            snapshot.last_error = ppp::app::runtime::RuntimeError();
        }

        if (configuration_ != NULLPTR) {
            snapshot.requested_mux_mode = configuration_->mux.mode;
            snapshot.effective_mux_mode = configuration_->GetEffectiveMuxMode();
        }

        if (client_mode_) {
            std::shared_ptr<ppp::app::client::VEthernetNetworkSwitcher> client = client_;
            if (NULLPTR != client) {
                const ppp::string remote_uri = client->GetRemoteUri();
                if (!remote_uri.empty()) {
                    const auto delimiter_pos = remote_uri.rfind('/') + 1;
                    if (delimiter_pos < remote_uri.size()) {
                        snapshot.transport = remote_uri.substr(delimiter_pos);
                        snapshot.server = remote_uri.substr(0, remote_uri.size() - snapshot.transport.size() - 1);
                    }
                    else {
                        snapshot.server = remote_uri;
                    }

                    if (snapshot.transport == "ppp+ws") {
                        snapshot.transport = "websocket";
                    }
                    else if (snapshot.transport == "ppp+wss") {
                        snapshot.transport = "websocket-secure";
                    }
                    else if (snapshot.transport == "ppp+tcp") {
                        snapshot.transport = "tcp";
                    }
                }

                const auto information_extensions = client->GetInformationExtensions();
                if (information_extensions.P2P.enabled) {
                    snapshot.p2p_state = information_extensions.P2P.mode;
                    snapshot.effective_path = information_extensions.P2P.mode;
                }
                else {
                    snapshot.p2p_state = "direct";
                    snapshot.effective_path = "tunnel";
                }

                runtime_information_observed_ = true;
                if (!client->IsMuxEnabled()) {
                    snapshot.mux_fallback_reason = "mux disabled";
                }
            }
            else {
                snapshot.server = "";
                snapshot.transport = "";
                snapshot.p2p_state.clear();
                snapshot.effective_path.clear();
            }
        }
        else {
            snapshot.server = "";
            snapshot.transport = "server";
            snapshot.p2p_state = "";
            snapshot.effective_path = "";
        }

        if (latest.generation > snapshot.generation) {
            return;
        }
        if (latest.generation == snapshot.generation && latest.monotonic_ms >= snapshot.monotonic_ms) {
            snapshot.monotonic_ms = latest.monotonic_ms;
        }
        if (phase == ppp::app::runtime::RuntimePhase::Failed && ppp::diagnostics::GetLastErrorCode() == ppp::diagnostics::ErrorCode::Success) {
            // Keep an empty error detail for protocol-level failed transitions when no dedicated error is available.
            snapshot.last_error = ppp::app::runtime::RuntimeError();
        }
    }

    publisher->Publish(std::move(snapshot));
}

bool PppApplication::TryEnterRuntimeStopSequence() noexcept {
    std::lock_guard<std::mutex> scope(runtime_state_mutex_);
    if (0 == runtime_generation_ || runtime_stop_publish_started_) {
        return false;
    }

    runtime_stop_publish_started_ = true;
    return true;
}

std::shared_ptr<VirtualEthernetSwitcher> PppApplication::GetServer() noexcept {
    return server_;
}

std::shared_ptr<VEthernetNetworkSwitcher> PppApplication::GetClient() noexcept {
    return client_;
}

std::shared_ptr<AppConfiguration> PppApplication::GetConfiguration() noexcept {
    return configuration_;
}

bool PppApplication::OnShutdownApplication() noexcept {
    return ShutdownApplication(false);
}

bool PppApplication::ShutdownApplication(bool restart) noexcept {
    std::shared_ptr<boost::asio::io_context> context = Executors::GetDefault();
    if (NULLPTR == context) {
        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeIoContextMissing);
        return false;
    }

    GLOBAL_RESTART.store(GLOBAL_RESTART.load(std::memory_order_relaxed) || restart, std::memory_order_relaxed);
    boost::asio::post(*context, [restart, context]() noexcept {
        std::shared_ptr<PppApplication> app = std::move(DEFAULT_);
        if (NULLPTR == app) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::AppContextUnavailable);
            return false;
        }

        const bool should_publish_runtime_stop = app->TryEnterRuntimeStopSequence();
        if (should_publish_runtime_stop) {
            app->PublishRuntimePhase(ppp::app::runtime::RuntimePhase::Stopping);
        }
        app->Dispose();
        if (should_publish_runtime_stop) {
            app->PublishRuntimePhase(ppp::app::runtime::RuntimePhase::Idle);
        }
        std::shared_ptr<Timer> timeout = Timer::Timeout(context, 1000, [](Timer*) noexcept { Executors::Exit(); });
        if (NULLPTR == timeout) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeTimerStartFailed);
        }
        return NULLPTR != timeout;
    });
    return true;
}

bool PppApplication::AddShutdownApplicationEventHandler() noexcept {
#if defined(_WIN32)
    bool registered = ppp::win32::Win32Native::AddShutdownApplicationEventHandler(PppApplication::OnShutdownApplication);
#else
    bool registered = ppp::unix__::UnixAfx::AddShutdownApplicationEventHandler(PppApplication::OnShutdownApplication);
#endif
    if (!registered) {
        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeEventDispatchFailed);
    }
    return registered;
}

bool PppApplication::NextTickAlwaysTimeout(bool next) noexcept {
    std::shared_ptr<boost::asio::io_context> context = Executors::GetDefault();
    if (NULLPTR == context) {
        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeIoContextMissing);
        return false;
    }

    std::shared_ptr<PppApplication> app = DEFAULT_;
    if (NULLPTR == app) {
        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::AppContextUnavailable);
        return false;
    }

    std::shared_ptr<VirtualEthernetSwitcher> server = app->server_;
    std::shared_ptr<VEthernetNetworkSwitcher> client = app->client_;
    if (NULLPTR == server && NULLPTR == client) {
        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeInitializationFailed);
        return false;
    }

    std::shared_ptr<Timer> timeout = Timer::Timeout(context, 1000, [](Timer*) noexcept {
        std::shared_ptr<PppApplication> inner = DEFAULT_;
        if (NULLPTR != inner) {
            inner->NextTickAlwaysTimeout(true);
        }
    });
    if (NULLPTR == timeout) {
        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeTimerStartFailed);
        return false;
    }

    app->timeout_ = std::move(timeout);
    app->OnTick(Executors::GetTickCount());
    return true;
}

void PppApplication::ClearTickAlwaysTimeout() noexcept {
    std::shared_ptr<Timer> timeout = std::move(timeout_);
    if (NULLPTR != timeout) {
        timeout->Dispose();
    }
}

int RunPreparedApplication(const std::shared_ptr<PppApplication>& app, int prepared_status, int argc, const char* argv[]) noexcept {
    if (ppp::HasCommandArgument("--pull-iplist", argc, argv)) {
        app->PullIPList(ppp::GetCommandArgument("--pull-iplist", argc, argv), false);
        int rc = ppp::diagnostics::GetLastErrorCode() == ppp::diagnostics::ErrorCode::Success ? 0 : -1;
        Executors::Exit();
        return rc;
    }

#if defined(_WIN32)
    if (Windows_PreferredNetwork(argc, argv)) {
        return ppp::diagnostics::GetLastErrorCode() == ppp::diagnostics::ErrorCode::Success ? 0 : -1;
    }

    if (Windows_NoLsp(argc, argv)) {
        return ppp::diagnostics::GetLastErrorCode() == ppp::diagnostics::ErrorCode::Success ? 0 : -1;
    }

    if (ppp::HasCommandArgument("--system-network-optimization", argc, argv)) {
        if (!ppp::win32::Win32Native::OptimizationSystemNetworkSettings()) {
            if (ppp::diagnostics::ErrorCode::Success == ppp::diagnostics::GetLastErrorCode()) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::NetworkInterfaceConfigureFailed);
            }
        }
        return ppp::diagnostics::GetLastErrorCode() == ppp::diagnostics::ErrorCode::Success ? 0 : -1;
    }
#endif

    if (prepared_status != 0) {
        app->PrintHelpInformation();
        if (ppp::diagnostics::ErrorCode::Success == ppp::diagnostics::GetLastErrorCode()) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::AppInvalidCommandLine);
        }
        return -1;
    }

    PppApplication::AddShutdownApplicationEventHandler();

#if SIGRESTART
    signal(SIGRESTART, [](int) noexcept { PppApplication::ShutdownApplication(true); });
#endif

    return app->Main(argc, argv);
}

} // namespace ppp::app
