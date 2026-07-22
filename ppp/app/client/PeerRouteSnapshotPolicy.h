#pragma once

#include <ppp/app/protocol/VirtualEthernetInformation.h>

namespace ppp {
    namespace app {
        namespace client {

            inline protocol::VirtualEthernetInformationExtensions WithoutDynamicPeerRouteSnapshot(
                const protocol::VirtualEthernetInformationExtensions& extensions) {
                protocol::VirtualEthernetInformationExtensions trusted = extensions;
                trusted.PeerRouteTable.Clear();
                return trusted;
            }
        }
    }
}
