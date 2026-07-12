#define BOOST_TEST_MODULE dns_host_interface_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/client/dns/DnsHost.h>

namespace client_dns = ppp::app::client::dns;

struct FakeDnsHost final : client_dns::IDnsHost {
    client_dns::DnsHostPorts BuildDnsHostPorts(
        const std::shared_ptr<ppp::app::client::VEthernetExchanger>&) noexcept override {
        return client_dns::DnsHostPorts{};
    }
    std::shared_ptr<const client_dns::DnsHostPorts> DnsHostPortsFor(
        const std::shared_ptr<ppp::app::client::VEthernetExchanger>&) noexcept override {
        return std::make_shared<const client_dns::DnsHostPorts>();
    }
    void InvalidateDnsHostPorts() noexcept override {}
    bool RedirectDnsServer(
        const std::shared_ptr<ppp::app::client::VEthernetExchanger>&,
        const std::shared_ptr<ppp::net::packet::IPFrame>&,
        const std::shared_ptr<ppp::net::packet::UdpFrame>&,
        const std::shared_ptr<ppp::net::packet::BufferSegment>&) noexcept override {
        return false;
    }
};

BOOST_AUTO_TEST_CASE(idns_host_is_polymorphic) {
    FakeDnsHost host;
    client_dns::IDnsHost& iface = host;
    BOOST_TEST(!iface.RedirectDnsServer({}, {}, {}, {}));
}
