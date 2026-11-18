#pragma once

#include <ppp/configurations/AppConfiguration.h>
#include <ppp/configurations/MultipathConfiguration.h>
#include <ppp/coroutines/YieldContext.h>
#include <ppp/threading/Executors.h>
#include <ppp/transmissions/ITransmission.h>
#include <ppp/transmissions/ITcpipTransmission.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace ppp::app::multipath {
    class PathAttempt final : public std::enable_shared_from_this<PathAttempt> {
    public:
        using ContextPtr = ppp::threading::Executors::ContextPtr;
        using StrandPtr = ppp::threading::Executors::StrandPtr;

        static std::shared_ptr<PathAttempt> Start(const ContextPtr& context, const StrandPtr& strand,
            std::uint32_t timeout_ms, const ppp::function<void()>& on_timeout) noexcept;
        ~PathAttempt() noexcept;
        bool IsActive() const noexcept { return active_.load(std::memory_order_acquire); }
        bool AddCancel(const ppp::function<void()>& cancel) noexcept;
        bool AddLifetime(const std::shared_ptr<void>& lifetime) noexcept;
        std::shared_ptr<void> TakeLifetime() noexcept;
        void Complete() noexcept;

    private:
        PathAttempt(const ContextPtr& context, const StrandPtr& strand) noexcept;
        void CancelResources() noexcept;

        std::shared_ptr<boost::asio::steady_timer> timer_;
        std::atomic_bool active_{true};
        std::mutex mutex_;
        std::vector<ppp::function<void()>> cancellations_;
        std::vector<std::shared_ptr<void>> lifetimes_;
        ppp::function<void()> on_timeout_;
    };

    class PathDialer final {
    public:
        using Socket = boost::asio::ip::tcp::socket;
        using SocketPtr = std::shared_ptr<Socket>;
        using ContextPtr = ppp::threading::Executors::ContextPtr;
        using StrandPtr = ppp::threading::Executors::StrandPtr;
        using TransmissionPtr = std::shared_ptr<ppp::transmissions::ITransmission>;
        using AddressLeaseAcquire = ppp::function<bool(
            const boost::asio::ip::address&, std::shared_ptr<void>&)>;
        using Protector = ppp::function<bool(const SocketPtr&, const boost::asio::ip::tcp::endpoint&, ppp::coroutines::YieldContext&)>;
        using TransmissionFactory = ppp::function<TransmissionPtr(
            const ppp::configurations::MultipathConfiguration::Path&, const SocketPtr&)>;

        static TransmissionPtr Connect(
            const ContextPtr& context,
            const StrandPtr& strand,
            const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration,
            const ppp::configurations::MultipathConfiguration::Path& path,
            const std::shared_ptr<PathAttempt>& attempt,
            const AddressLeaseAcquire& acquire_address,
            const Protector& protect,
            const TransmissionFactory& create_transmission,
            ppp::coroutines::YieldContext& y) noexcept;
    };
}
