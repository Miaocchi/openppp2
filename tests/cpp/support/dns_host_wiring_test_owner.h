#pragma once

/**
 * @file dns_host_wiring_test_owner.h
 * @brief Test-only owner mirroring VEthernetNetworkSwitcher DNS host port cache semantics.
 */

#include <memory>

#include <ppp/app/client/dns/DnsHost.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/app/client/VEthernetExchanger.h>

namespace ppp {
    namespace app {
        namespace client {
            namespace dns {
                namespace test {

                    /** @brief Mirrors DnsHostPortsFor / InvalidateDnsHostPorts cache behavior for unit tests. */
                    class DnsHostWiringTestOwner final {
                    public:
                        explicit DnsHostWiringTestOwner(
                            const std::shared_ptr<VEthernetNetworkSwitcher>& switcher) noexcept
                            : switcher_(switcher) {}

                        std::shared_ptr<const DnsHostPorts> DnsHostPortsFor(
                            const std::shared_ptr<VEthernetExchanger>& exchanger) noexcept {

                            if (std::shared_ptr<VEthernetExchanger> cached = dns_host_ports_exchanger_.lock();
                                cached == exchanger && NULLPTR != dns_host_ports_cache_ &&
                                dns_host_ports_cache_->IsValid()) {
                                return dns_host_ports_cache_;
                            }

                            DnsHostPorts rebuilt_ports = MakeDnsHostPorts(switcher_, exchanger);
                            if (!rebuilt_ports.IsValid()) {
                                return NULLPTR;
                            }

                            dns_host_ports_cache_ = std::make_shared<const DnsHostPorts>(std::move(rebuilt_ports));
                            dns_host_ports_exchanger_ = exchanger;
                            return dns_host_ports_cache_;
                        }

                        void InvalidateDnsHostPorts() noexcept {
                            dns_host_ports_cache_.reset();
                            dns_host_ports_exchanger_.reset();
                        }

                    private:
                        std::shared_ptr<VEthernetNetworkSwitcher> switcher_;
                        std::shared_ptr<const DnsHostPorts> dns_host_ports_cache_;
                        std::weak_ptr<VEthernetExchanger> dns_host_ports_exchanger_;
                    };

                }  // namespace test
            }  // namespace dns
        }  // namespace client
    }  // namespace app
}  // namespace ppp
