#define BOOST_TEST_MODULE network_ownership_test

#include <boost/test/included/unit_test.hpp>

#include <ppp/app/client/route/NetworkOwnership.h>

namespace route = ppp::app::client::route;

BOOST_AUTO_TEST_CASE(lease_is_exclusive_and_releases_on_scope_exit) {
    auto first = route::NetworkOwnershipLease::Acquire("test-first");
    BOOST_REQUIRE(first.has_value());
    BOOST_TEST(first->IsHeld());
    BOOST_TEST(!first->Token().empty());
    BOOST_TEST(first->OwnerId() == "test-first");

    auto conflict = route::NetworkOwnershipLease::Acquire("test-second");
    BOOST_TEST(!conflict.has_value());

    first.reset();
    auto second = route::NetworkOwnershipLease::Acquire("test-second");
    BOOST_REQUIRE(second.has_value());
    BOOST_TEST(second->IsHeld());
}

BOOST_AUTO_TEST_CASE(empty_owner_id_is_rejected) {
    BOOST_TEST(!route::NetworkOwnershipLease::Acquire("").has_value());
}
