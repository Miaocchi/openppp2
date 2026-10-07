/**
 * @file P2PSocketProtector.cpp
 * @brief Platform-specific socket protection implementations.
 *
 * @license GPL-3.0
 */

#include <ppp/p2p/P2PSocketProtector.h>

#if defined(_LINUX) && !defined(_ANDROID)
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#if defined(_ANDROID)
#include <android/OpenPPP2VpnProtectBridge.h>
#endif

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#if defined(_MACOS) && !defined(_IPHONE) && !defined(IPHONE)
#include <sys/socket.h>
#include <netinet/in.h>
#endif

namespace ppp {
    namespace p2p {

        // -------------------------------------------------------------------------
        // Linux physical-interface protector
        // -------------------------------------------------------------------------

#if defined(_LINUX) && !defined(_ANDROID)
        bool LinuxSocketProtector::Protect(int fd) noexcept {
            if (fd < 0 || interface_name_.empty()) {
                return false;
            }
            // Binding the socket to the physical NIC bypasses the TUN route
            // directly. SO_MARK is retained for deployments with a matching
            // policy rule, but is not used as the readiness signal.
            if (::setsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_BINDTODEVICE,
                    interface_name_.c_str(),
                    static_cast<socklen_t>(interface_name_.size() + 1)) != 0) {
                return false;
            }
            (void)mark_;
            return true;
        }
#endif

        // -------------------------------------------------------------------------
        // Android VpnService.protect() protector
        // -------------------------------------------------------------------------

#if defined(_ANDROID)
        bool AndroidSocketProtector::IsReady() const noexcept {
            return ppp::android::IsProtectBridgeReady();
        }

        bool AndroidSocketProtector::Protect(int fd) noexcept {
            return ppp::android::ProtectSocketFd(fd);
        }
#endif

#if defined(_WIN32)
        bool WindowsSocketProtector::Protect(int fd) noexcept {
            if (fd < 0 || interface_index_ <= 0) {
                return false;
            }
            const DWORD network_index = htonl(static_cast<DWORD>(interface_index_));
            return ::setsockopt(
                static_cast<SOCKET>(fd),
                IPPROTO_IP,
                IP_UNICAST_IF,
                reinterpret_cast<const char*>(&network_index),
                sizeof(network_index)) == 0;
        }
#endif

#if defined(_MACOS) && !defined(_IPHONE) && !defined(IPHONE)
        bool MacSocketProtector::Protect(int fd) noexcept {
            if (fd < 0 || interface_index_ <= 0) {
                return false;
            }
            const unsigned int native_index = static_cast<unsigned int>(interface_index_);
            return ::setsockopt(
                fd,
                IPPROTO_IP,
                IP_BOUND_IF,
                &native_index,
                sizeof(native_index)) == 0;
        }
#endif

        // -------------------------------------------------------------------------
        // Hot socket pool
        // -------------------------------------------------------------------------

        P2PSocketPool::P2PSocketPool(const std::shared_ptr<ISocketProtector>& protector,
                                     boost::asio::io_context& io_ctx,
                                     int pool_size) noexcept
            : protector_(protector)
            , io_ctx_(&io_ctx)
            , pool_size_(pool_size) {
        }

        P2PSocketPool::~P2PSocketPool() noexcept {
            available_.clear();
        }

        std::unique_ptr<boost::asio::ip::udp::socket> P2PSocketPool::Acquire() noexcept {
            // Try pool first.
            if (!available_.empty()) {
                auto socket = std::move(available_.back());
                available_.pop_back();
                if (socket && socket->is_open()) {
                    return socket;
                }
                // Stale socket in pool — discard and create new.
            }

            // Create new socket and protect it.
            auto socket = std::make_unique<boost::asio::ip::udp::socket>(*io_ctx_,
                boost::asio::ip::udp::v4());
            if (!socket || !socket->is_open()) {
                return nullptr;
            }

            int fd = static_cast<int>(socket->native_handle());
            if (!ProtectP2PSocket(protector_, fd)) {
                // H1: Protection failed — do not return an unprotected socket.
                boost::system::error_code ec;
                socket->close(ec);
                return nullptr;
            }

            return socket;
        }

        void P2PSocketPool::Return(std::unique_ptr<boost::asio::ip::udp::socket> socket) noexcept {
            if (!socket || !socket->is_open()) {
                return;
            }
            if (static_cast<int>(available_.size()) < pool_size_) {
                available_.emplace_back(std::move(socket));
            }
            // Otherwise, let the unique_ptr destructor close it.
        }

    }
}
