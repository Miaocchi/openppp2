#pragma once

#include <boost/asio/ip/address.hpp>

#include <cstddef>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ppp::app::multipath {
    /**
     * Tracks ownership of resolved endpoint addresses. Optional route callbacks
     * run only on the first acquire and final release; no route is implied when
     * callbacks are absent. Socket and VPN-platform protection remain separate.
     */
    class BypassLeaseManager final {
    private:
        struct State;

    public:
        using Address = boost::asio::ip::address;

        static constexpr std::size_t MaxPaths = 8;
        static constexpr std::size_t MaxCarriers = 32;
        static constexpr std::size_t MaxDnsAddressesPerPath = 64;
        static constexpr std::size_t MaxTrackedAddresses = MaxPaths * MaxCarriers * MaxDnsAddressesPerPath;
        static constexpr std::size_t MaxReferencesPerAddress = MaxPaths * MaxCarriers;

        struct RouteLeaseCallbacks final {
            std::function<bool(const Address&)> acquire;
            std::function<void(const Address&)> release;
        };

        struct Limits final {
            std::size_t max_tracked_addresses = MaxTrackedAddresses;
            std::size_t max_references_per_address = MaxReferencesPerAddress;
        };

        class Lease final {
        public:
            Lease() noexcept = default;
            Lease(const Lease&) = delete;
            Lease& operator=(const Lease&) = delete;
            Lease(Lease&& other) noexcept;
            Lease& operator=(Lease&& other) noexcept;
            ~Lease() noexcept;

            bool Valid() const noexcept { return state_ != nullptr; }
            const Address& GetAddress() const noexcept { return address_; }
            void Reset() noexcept;

        private:
            friend class BypassLeaseManager;
            Lease(const std::shared_ptr<State>& state, const Address& address, std::string key) noexcept;

            std::shared_ptr<State> state_;
            Address address_;
            std::string key_;
        };

        class LeaseSet final {
        public:
            LeaseSet() noexcept = default;
            LeaseSet(const LeaseSet&) = delete;
            LeaseSet& operator=(const LeaseSet&) = delete;
            LeaseSet(LeaseSet&&) noexcept = default;
            LeaseSet& operator=(LeaseSet&&) noexcept = default;

            bool Empty() const noexcept { return leases_.empty(); }
            std::size_t Size() const noexcept { return leases_.size(); }
            void Reset() noexcept { leases_.clear(); }

        private:
            friend class BypassLeaseManager;
            std::vector<Lease> leases_;
        };

        BypassLeaseManager() noexcept;
        explicit BypassLeaseManager(const RouteLeaseCallbacks& callbacks) noexcept;
        BypassLeaseManager(const RouteLeaseCallbacks& callbacks, const Limits& limits) noexcept;
        ~BypassLeaseManager() noexcept = default;

        BypassLeaseManager(const BypassLeaseManager&) = delete;
        BypassLeaseManager& operator=(const BypassLeaseManager&) = delete;

        /** Acquires one resolved IPv4/IPv6 candidate, replacing output on success. */
        bool AcquireAddress(const Address& address, Lease& output) noexcept;

        /**
         * Acquires a deduplicated address set transactionally. On failure the
         * output is unchanged; on success old leases are released only after
         * every address in the new set has been acquired.
         */
        bool AcquireCandidates(const std::vector<Address>& addresses, LeaseSet& output) noexcept;

        std::size_t TrackedAddressCount() const noexcept;
        std::size_t ReferenceCount(const Address& address) const noexcept;

    private:
        static void Release(const std::shared_ptr<State>& state, const std::string& key) noexcept;
        static bool MakeKey(const Address& address, std::string& key) noexcept;

        std::shared_ptr<State> state_;
    };
}
