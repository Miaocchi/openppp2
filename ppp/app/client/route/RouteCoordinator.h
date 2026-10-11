#pragma once

#include <atomic>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <ppp/app/client/route/IRoutePlatform.h>
#include <ppp/app/client/route/NetworkOwnership.h>
#include <ppp/app/client/route/RoutePlanInput.h>
#include <ppp/app/client/route/RouteState.h>

namespace ppp {
    namespace app {
        namespace client {
            namespace route {

                class RouteCoordinator final {
                public:
                    explicit RouteCoordinator(
                        std::unique_ptr<IRoutePlatform> platform) noexcept;
                    ~RouteCoordinator() noexcept;

                    bool Apply(const std::vector<RouteSpec>& routes) noexcept;
                    // Rolls back routes applied by the current attempt while
                    // keeping the coordinator usable for a reconnect.
                    bool RollbackApplied() noexcept;
                    bool Stop() noexcept;
                    RouteStateSnapshot Snapshot() const noexcept;
#if defined(OPENPPP2_ROUTE_TEST_MOBILE)
                    void SetStopWaiterObserverForTesting(
                        std::function<void()> observer) noexcept;
#endif

                    void ReplaceRib(RouteInformationTablePtr value) noexcept;
                    void ReplaceFib(ForwardInformationTablePtr value) noexcept;
                    void ReplacePeerPrefix(
                        RouteInformationTablePtr rib,
                        ForwardInformationTablePtr fib) noexcept;
                    void AddNic(uint32_t gateway, std::string interface_name) noexcept;
                    void MarkApplyReady(bool value) noexcept;
                    void Clear() noexcept;

                    // Identifies the process that owns the network control
                    // plane lease. Empty means another VPN process owns it.
                    const std::string& NetworkOwnershipToken() const noexcept {
                        return ownership_.has_value()
                            ? ownership_->Token() : empty_ownership_token_;
                    }

#if defined(_ANDROID) || defined(_IPHONE) || defined(OPENPPP2_ROUTE_TEST_MOBILE)
                    bool AddAllRoute(const RoutePlanInput& input) noexcept;
#endif

#if !defined(_ANDROID) && !defined(_IPHONE)
                    bool AddRoute(const RoutePlanInput& input) noexcept;
                    bool DeleteRoute() noexcept;
                    bool EnsureUnderlyingDefault(const RoutePlanInput& input) noexcept;
                    bool AddRoute(
                        const RoutePlanInput& input,
                        uint32_t ip,
                        uint32_t gw,
                        int prefix) noexcept;
                    bool DeleteRoute(
                        const RoutePlanInput& input,
                        uint32_t ip,
                        uint32_t gw,
                        int prefix) noexcept;
                    static bool ShouldDeferHostedRouteApply(
                        bool route_apply_ready,
                        bool exchanger_established) noexcept {
                        return !route_apply_ready || !exchanger_established;
                    }
                    bool ProtectDefaultRoute(const RoutePlanInput& input) noexcept;
#endif

                private:
                    static std::unique_ptr<IRoutePlatform> NewPlatform(
                        const RoutePlanInput& input) noexcept;
                    bool SetPlatformLocked(std::unique_ptr<IRoutePlatform> platform) noexcept;
                    bool ApplyLocked(const std::vector<RouteSpec>& routes) noexcept;
                    struct ProtectionState final {
                        std::atomic_bool active{false};
                        std::mutex mutex;
                        std::shared_ptr<IRoutePlatform> platform;
                        std::thread worker;
                    };
                    void RememberDefault(
                        const std::shared_ptr<IRoutePlatform>& platform,
                        const RouteSnapshotPtr& route) noexcept;
                    bool Rollback(std::vector<RouteSpec>& applied) noexcept;
                    static void StopProtection(
                        const std::shared_ptr<ProtectionState>& state) noexcept;

                    RouteState state_;
                    std::shared_ptr<IRoutePlatform> platform_;
                    std::vector<RouteSpec> applied_;
                    std::shared_ptr<ProtectionState> protection_;
                    mutable std::mutex operation_mutex_;
                    std::mutex stop_mutex_;
                    std::shared_future<bool> stop_attempt_;
                    bool stop_in_progress_ = false;
#if defined(OPENPPP2_ROUTE_TEST_MOBILE)
                    std::function<void()> stop_waiter_observer_;
#endif
                    std::atomic_bool stopped_{false};
                    std::optional<NetworkOwnershipLease> ownership_;
                    static const std::string empty_ownership_token_;
                };

            }
        }
    }
}
