#define BOOST_TEST_MODULE peer_route_snapshot_policy_test
#include <ppp/stdafx.h>
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/client/PeerRouteSnapshotPolicy.h>

namespace client = ppp::app::client;
namespace protocol = ppp::app::protocol;

BOOST_AUTO_TEST_CASE(static_route_application_strips_server_peer_route_snapshot) {
    protocol::VirtualEthernetInformationExtensions extensions;
    extensions.Clear();
    extensions.ClientIPv4Assign.enabled = true;
    extensions.PeerRouteTable.enabled = true;
    extensions.PeerRouteTable.action = "snapshot";

    protocol::PeerPrefixRouteEntry route;
    route.network = "10.42.0.0";
    route.prefix = 16;
    route.via = "10.0.0.2";
    extensions.PeerRouteTable.routes.emplace_back(std::move(route));

    protocol::VirtualEthernetInformationExtensions trusted =
        client::WithoutDynamicPeerRouteSnapshot(extensions);

    BOOST_TEST(trusted.ClientIPv4Assign.enabled);
    BOOST_TEST(!trusted.PeerRouteTable.HasAny());
    BOOST_TEST(trusted.PeerRouteTable.routes.empty());
}
