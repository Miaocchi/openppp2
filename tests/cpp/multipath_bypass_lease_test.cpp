#define BOOST_TEST_MODULE multipath_bypass_lease_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/multipath/BypassLease.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {
    using Manager = ppp::app::multipath::BypassLeaseManager;
    using Address = Manager::Address;

    static Address ParseAddress(const char* value) {
        boost::system::error_code ec;
        Address address = boost::asio::ip::make_address(value, ec);
        BOOST_REQUIRE(!ec);
        return address;
    }

    static std::vector<Address> Addresses(std::initializer_list<const char*> values) {
        std::vector<Address> result;
        result.reserve(values.size());
        for (const char* value : values) result.emplace_back(ParseAddress(value));
        return result;
    }
}

BOOST_AUTO_TEST_CASE(shared_addresses_are_acquired_once_and_released_after_last_owner) {
    std::vector<std::string> events;
    Manager::RouteLeaseCallbacks callbacks;
    callbacks.acquire = [&events](const Address& address) {
        events.emplace_back("+" + address.to_string());
        return true;
    };
    callbacks.release = [&events](const Address& address) {
        events.emplace_back("-" + address.to_string());
    };
    Manager manager(callbacks);
    const Address v4 = ParseAddress("192.0.2.10");
    const Address v6 = ParseAddress("2001:db8::10");
    Manager::Lease first;
    Manager::Lease second;

    BOOST_REQUIRE(manager.AcquireAddress(v4, first));
    BOOST_REQUIRE(manager.AcquireAddress(v4, second));
    BOOST_TEST(manager.TrackedAddressCount() == 1u);
    BOOST_TEST(manager.ReferenceCount(v4) == 2u);
    BOOST_TEST(events.size() == 1u);
    first.Reset();
    BOOST_TEST(manager.ReferenceCount(v4) == 1u);
    BOOST_TEST(events.size() == 1u);
    second.Reset();
    BOOST_TEST(manager.TrackedAddressCount() == 0u);
    BOOST_TEST(events.size() == 2u);
    BOOST_TEST(events[0] == "+192.0.2.10");
    BOOST_TEST(events[1] == "-192.0.2.10");

    Manager::Lease ipv6;
    BOOST_REQUIRE(manager.AcquireAddress(v6, ipv6));
    BOOST_TEST(ipv6.GetAddress() == v6);
}

BOOST_AUTO_TEST_CASE(dns_replacement_acquires_new_set_before_releasing_old_set) {
    std::vector<std::string> events;
    Manager::RouteLeaseCallbacks callbacks;
    callbacks.acquire = [&events](const Address& address) {
        events.emplace_back("+" + address.to_string());
        return true;
    };
    callbacks.release = [&events](const Address& address) {
        events.emplace_back("-" + address.to_string());
    };
    Manager manager(callbacks);
    Manager::LeaseSet current;
    BOOST_REQUIRE(manager.AcquireCandidates(Addresses({"192.0.2.1", "192.0.2.2"}), current));
    BOOST_REQUIRE(current.Size() == 2u);
    events.clear();

    BOOST_REQUIRE(manager.AcquireCandidates(Addresses({"192.0.2.2", "192.0.2.3"}), current));
    BOOST_REQUIRE(current.Size() == 2u);
    BOOST_TEST(manager.ReferenceCount(ParseAddress("192.0.2.1")) == 0u);
    BOOST_TEST(manager.ReferenceCount(ParseAddress("192.0.2.2")) == 1u);
    BOOST_TEST(manager.ReferenceCount(ParseAddress("192.0.2.3")) == 1u);
    BOOST_REQUIRE(events.size() == 2u);
    BOOST_TEST(events[0] == "+192.0.2.3");
    BOOST_TEST(events[1] == "-192.0.2.1");
    current.Reset();
    BOOST_TEST(manager.TrackedAddressCount() == 0u);
}

BOOST_AUTO_TEST_CASE(failed_dns_replacement_rolls_back_new_references_and_keeps_old_set) {
    std::vector<std::string> events;
    Manager::RouteLeaseCallbacks callbacks;
    callbacks.acquire = [&events](const Address& address) {
        const std::string value = address.to_string();
        events.emplace_back("+" + value);
        return value != "192.0.2.3";
    };
    callbacks.release = [&events](const Address& address) {
        events.emplace_back("-" + address.to_string());
    };
    Manager manager(callbacks);
    Manager::LeaseSet current;
    BOOST_REQUIRE(manager.AcquireCandidates(Addresses({"192.0.2.1", "192.0.2.2"}), current));
    events.clear();

    BOOST_TEST(!manager.AcquireCandidates(Addresses({"192.0.2.2", "192.0.2.3"}), current));
    BOOST_TEST(current.Size() == 2u);
    BOOST_TEST(manager.ReferenceCount(ParseAddress("192.0.2.1")) == 1u);
    BOOST_TEST(manager.ReferenceCount(ParseAddress("192.0.2.2")) == 1u);
    BOOST_TEST(manager.TrackedAddressCount() == 2u);
    BOOST_REQUIRE(events.size() == 1u);
    BOOST_TEST(events[0] == "+192.0.2.3");
    current.Reset();
    BOOST_TEST(manager.TrackedAddressCount() == 0u);
}

BOOST_AUTO_TEST_CASE(limits_invalid_candidates_and_callback_pairs_fail_closed) {
    Manager::Limits limits;
    limits.max_tracked_addresses = 1;
    limits.max_references_per_address = 1;
    Manager manager(Manager::RouteLeaseCallbacks(), limits);
    Manager::Lease first;
    Manager::Lease second;
    const Address address = ParseAddress("198.51.100.1");
    BOOST_REQUIRE(manager.AcquireAddress(address, first));
    BOOST_TEST(!manager.AcquireAddress(address, second));
    BOOST_TEST(!manager.AcquireAddress(ParseAddress("198.51.100.2"), second));
    BOOST_TEST(!manager.AcquireAddress(ParseAddress("0.0.0.0"), second));
    BOOST_TEST(!manager.AcquireAddress(ParseAddress("ff02::1"), second));

    std::vector<Address> too_many;
    for (std::uint32_t i = 1; i <= Manager::MaxDnsAddressesPerPath + 1; ++i) {
        too_many.emplace_back(boost::asio::ip::address_v4(0xc0000200u + i));
    }
    Manager::LeaseSet candidates;
    BOOST_TEST(!manager.AcquireCandidates(too_many, candidates));

    Manager::RouteLeaseCallbacks incomplete;
    incomplete.acquire = [](const Address&) { return true; };
    Manager invalid_callbacks(incomplete);
    Manager::Lease invalid;
    BOOST_TEST(!invalid_callbacks.AcquireAddress(address, invalid));
}

BOOST_AUTO_TEST_CASE(concurrent_references_share_one_first_and_final_callback) {
    std::atomic<int> acquired{0};
    std::atomic<int> released{0};
    std::atomic<int> failures{0};
    std::atomic<int> arrived{0};
    Manager::RouteLeaseCallbacks callbacks;
    callbacks.acquire = [&acquired](const Address&) { ++acquired; return true; };
    callbacks.release = [&released](const Address&) { ++released; };
    auto manager = std::make_shared<Manager>(callbacks);
    const Address address = ParseAddress("203.0.113.7");
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([manager, address, &failures, &arrived]() {
            Manager::Lease lease;
            if (!manager->AcquireAddress(address, lease)) ++failures;
            arrived.fetch_add(1, std::memory_order_release);
            while (arrived.load(std::memory_order_acquire) != 8) {
                std::this_thread::yield();
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    BOOST_TEST(failures.load() == 0);
    BOOST_TEST(acquired.load() == 1);
    BOOST_TEST(released.load() == 1);
    BOOST_TEST(manager->TrackedAddressCount() == 0u);
}
