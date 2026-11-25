#include <ppp/app/multipath/PathDialer.h>

#include <ppp/net/Socket.h>
#include <ppp/threading/Executors.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/transmissions/IWebsocketTransmission.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>

namespace ppp::app::multipath {
    PathAttempt::PathAttempt(const ContextPtr& context, const StrandPtr& strand) noexcept {
        timer_ = strand
            ? ppp::make_shared_object<boost::asio::steady_timer>(*strand)
            : (context ? ppp::make_shared_object<boost::asio::steady_timer>(*context) : nullptr);
    }

    std::shared_ptr<PathAttempt> PathAttempt::Start(
        const ContextPtr& context, const StrandPtr& strand, std::uint32_t timeout_ms,
        const ppp::function<void()>& on_timeout) noexcept {
        if (!context || timeout_ms == 0) {
            return nullptr;
        }
        std::shared_ptr<PathAttempt> attempt;
        try { attempt.reset(new PathAttempt(context, strand)); }
        catch (...) { return nullptr; }
        if (!attempt || !attempt->timer_) {
            return nullptr;
        }
        attempt->on_timeout_ = on_timeout;
        attempt->timer_->expires_after(std::chrono::milliseconds(timeout_ms));
        attempt->timer_->async_wait([attempt](const boost::system::error_code& ec) noexcept {
            if (ec || !attempt->active_.exchange(false, std::memory_order_acq_rel)) {
                return;
            }
            attempt->CancelResources();
            auto on_timeout = std::move(attempt->on_timeout_);
            if (on_timeout) {
                try { on_timeout(); }
                catch (...) {}
            }
        });
        return attempt;
    }

    PathAttempt::~PathAttempt() noexcept {
        Complete();
    }

    bool PathAttempt::AddCancel(const ppp::function<void()>& cancel) noexcept {
        if (!cancel) {
            return IsActive();
        }
        bool cancel_now = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.load(std::memory_order_acquire)) {
                try { cancellations_.emplace_back(cancel); }
                catch (...) { cancel_now = true; }
            }
            else {
                cancel_now = true;
            }
        }
        if (cancel_now) {
            try { cancel(); }
            catch (...) {}
            return false;
        }
        return true;
    }

    bool PathAttempt::AddLifetime(const std::shared_ptr<void>& lifetime) noexcept {
        if (!lifetime) {
            return IsActive();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_.load(std::memory_order_acquire)) {
            return false;
        }
        try { lifetimes_.emplace_back(lifetime); }
        catch (...) { return false; }
        return true;
    }

    std::shared_ptr<void> PathAttempt::TakeLifetime() noexcept {
        std::vector<std::shared_ptr<void>> lifetimes;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lifetimes.swap(lifetimes_);
        }
        if (lifetimes.empty()) {
            return nullptr;
        }
        try {
            return std::static_pointer_cast<void>(
                ppp::make_shared_object<std::vector<std::shared_ptr<void>>>(std::move(lifetimes)));
        }
        catch (...) {
            return nullptr;
        }
    }

    void PathAttempt::CancelResources() noexcept {
        std::vector<ppp::function<void()>> cancellations;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancellations.swap(cancellations_);
        }
        for (const auto& cancel : cancellations) {
            if (cancel) {
                try { cancel(); }
                catch (...) {}
            }
        }
    }

    void PathAttempt::Complete() noexcept {
        if (!active_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (timer_) {
            try { timer_->cancel(); }
            catch (...) {}
        }
        std::lock_guard<std::mutex> lock(mutex_);
        cancellations_.clear();
        lifetimes_.clear();
        on_timeout_ = {};
    }

    PathDialer::TransmissionPtr PathDialer::Connect(
        const ContextPtr& context,
        const StrandPtr& strand,
        const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration,
        const ppp::configurations::MultipathConfiguration::Path& path,
        const std::shared_ptr<PathAttempt>& attempt,
        const AddressLeaseAcquire& acquire_address,
        const Protector& protect,
        const TransmissionFactory& create_transmission,
        ppp::coroutines::YieldContext& y) noexcept {
        if (!context || !configuration || path.connect_host.empty() || path.port < 1 || path.port > 65535 ||
            !create_transmission || !attempt || !attempt->IsActive()) {
            return nullptr;
        }

        std::shared_ptr<boost::asio::ip::tcp::resolver> resolver = strand
            ? ppp::make_shared_object<boost::asio::ip::tcp::resolver>(*strand)
            : ppp::make_shared_object<boost::asio::ip::tcp::resolver>(*context);
        if (!resolver) {
            return nullptr;
        }
        if (!attempt->AddCancel([resolver]() noexcept { resolver->cancel(); })) {
            return nullptr;
        }
        const std::string service = std::to_string(path.port);
        using ResolveResults = boost::asio::ip::tcp::resolver::results_type;
        auto resolve_results = ppp::make_shared_object<ResolveResults>();
        auto resolve_state = ppp::make_shared_object<std::atomic_int>(0);
        if (!resolve_results || !resolve_state) {
            return nullptr;
        }
        const auto anchor = y.Arm();
        if (!anchor || !attempt->AddCancel([resolver, resolve_state, anchor]() noexcept {
                resolver->cancel();
                int expected = 0;
                if (resolve_state->compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
                    anchor.R();
                }
            })) {
            return nullptr;
        }
        try {
            resolver->async_resolve(path.connect_host, service,
                [resolve_results, resolve_state, anchor](const boost::system::error_code& ec,
                    const ResolveResults& results) noexcept {
                    int expected = 0;
                    if (!resolve_state->compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                        return;
                    }
                    if (!ec) {
                        *resolve_results = results;
                    }
                    anchor.R();
                });
        }
        catch (...) {
            int expected = 0;
            if (resolve_state->compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
                anchor.R();
            }
        }
        y.Suspend(anchor);
        if (resolve_state->load(std::memory_order_acquire) != 1 || !attempt->IsActive()) {
            return nullptr;
        }

        std::size_t endpoint_attempts = 0;
        for (const auto& resolved : *resolve_results) {
            if (endpoint_attempts++ >= 16) {
                break;
            }
            if (!attempt->IsActive()) {
                break;
            }

            const auto endpoint = resolved.endpoint();
            std::shared_ptr<void> endpoint_lifetime;
            if (acquire_address && !acquire_address(endpoint.address(), endpoint_lifetime)) {
                continue;
            }
            auto socket = strand
                ? ppp::make_shared_object<Socket>(*strand)
                : ppp::make_shared_object<Socket>(*context);
            if (!socket) {
                break;
            }
            if (!attempt->AddCancel([socket]() noexcept { ppp::net::Socket::Closesocket(socket); })) {
                break;
            }

            boost::system::error_code ec;
            socket->open(endpoint.protocol(), ec);
            if (ec) {
                ppp::net::Socket::Closesocket(socket);
                continue;
            }

            // Match the primary TCP connect path: window overrides and
            // transport options (keepalive, TOS, TCP_NODELAY, Fast Open) must
            // be applied while the socket is open and before connect, so
            // multipath carriers use the same configuration as the main link.
            ppp::net::Socket::SetWindowSizeIfNotZero(
                socket->native_handle(), configuration->tcp.cwnd, configuration->tcp.rwnd);
            ppp::net::Socket::AdjustSocketOptional(*socket,
                endpoint.address().is_v4(), configuration->tcp.fast_open,
                configuration->tcp.turbo || configuration->mux.intent.enabled);

            if ((protect && !protect(socket, endpoint, y)) || !attempt->IsActive()) {
                ppp::net::Socket::Closesocket(socket);
                continue;
            }

            const bool connected = ppp::coroutines::asio::async_connect(*socket, endpoint, y);
            if (!connected || !attempt->IsActive()) {
                ppp::net::Socket::Closesocket(socket);
                continue;
            }

            TransmissionPtr transmission = create_transmission(path, socket);
            if (!transmission) {
                ppp::net::Socket::Closesocket(socket);
                continue;
            }
            if (!attempt->AddCancel([transmission]() noexcept { transmission->Dispose(); })) {
                transmission->Dispose();
                return nullptr;
            }

            bool upgraded = true;
            if (auto websocket = std::dynamic_pointer_cast<ppp::transmissions::IWebsocketTransmission>(transmission)) {
                upgraded = websocket->UpgradeOnly(true, y);
            }
            elif(auto websocket = std::dynamic_pointer_cast<ppp::transmissions::ISslWebsocketTransmission>(transmission)) {
                upgraded = websocket->UpgradeOnly(true, y);
            }
            if (!upgraded) {
                transmission->Dispose();
                continue;
            }
            if (!attempt->AddLifetime(endpoint_lifetime)) {
                transmission->Dispose();
                return nullptr;
            }
            return transmission;
        }
        return nullptr;
    }
}
