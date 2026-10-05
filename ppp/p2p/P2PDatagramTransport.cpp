#include <ppp/p2p/P2PDatagramTransport.h>
#include <ppp/p2p/P2PSocketProtector.h>

#include <array>
#include <atomic>
#include <mutex>

namespace ppp {
    namespace p2p {
        namespace {

            class NativeSocketP2PDatagramTransport final
                : public IP2PDatagramTransport,
                  public std::enable_shared_from_this<NativeSocketP2PDatagramTransport> {
            public:
                NativeSocketP2PDatagramTransport(
                        boost::asio::io_context& io_context,
                        const std::shared_ptr<ISocketProtector>& protector) noexcept
                    : io_context_(io_context), protector_(protector) {}

                bool IsReady() const noexcept override {
                    return !closed_.load(std::memory_order_acquire) &&
                        protector_ && protector_->IsReady();
                }

                bool Start(const P2PDatagramReceiveCallback& callback) noexcept override {
                    if (started_.exchange(true, std::memory_order_acq_rel) ||
                        !callback || !IsReady()) {
                        return false;
                    }

                    {
                        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                        if (closed_.load(std::memory_order_acquire)) {
                            return false;
                        }
                        callback_ = callback;
                        socket_ = std::make_unique<boost::asio::ip::udp::socket>(io_context_);
                        boost::system::error_code ec;
                        socket_->open(boost::asio::ip::udp::v4(), ec);
                        if (ec || !socket_->is_open()) {
                            CloseLocked();
                            return false;
                        }
                        socket_->bind(
                            boost::asio::ip::udp::endpoint(
                                boost::asio::ip::address_v4::any(), 0), ec);
                        if (ec || !ProtectP2PSocket(
                                protector_, static_cast<int>(socket_->native_handle()))) {
                            CloseLocked();
                            return false;
                        }
                        local_endpoint_ = socket_->local_endpoint(ec);
                        if (ec) {
                            CloseLocked();
                            return false;
                        }

                        // async_receive_from supplies no UDP_GRO segment metadata.
                        // Keep native datagram boundaries for STUN/control/data.
                    }

                    StartReceive();
                    return true;
                }

                boost::asio::ip::udp::endpoint LocalEndpoint() const noexcept override {
                    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                    return local_endpoint_;
                }

                bool SendTo(
                        const uint8_t* packet,
                        int packet_size,
                        const boost::asio::ip::udp::endpoint& endpoint) noexcept override {
                    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                    if (closed_.load(std::memory_order_acquire) || !socket_ ||
                        !socket_->is_open() || !packet || packet_size < 1 ||
                        packet_size > P2P_MAX_PACKET_SIZE ||
                        !endpoint.address().is_v4() ||
                        endpoint.address().is_unspecified() ||
                        endpoint.address().is_multicast() || endpoint.port() == 0) {
                        return false;
                    }
                    boost::system::error_code ec;
                    socket_->send_to(
                        boost::asio::buffer(packet, static_cast<std::size_t>(packet_size)),
                        endpoint, 0, ec);
                    return !ec;
                }

                void Close() noexcept override {
                    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                    if (closed_.load(std::memory_order_acquire)) {
                        return;
                    }
                    CloseLocked();
                }

            private:
                // Caller must hold lifecycle_mutex_; serializes teardown with the receive
                // chain and SendTo so the socket object is never touched concurrently.
                void CloseLocked() noexcept {
                    closed_.store(true, std::memory_order_release);
                    if (socket_) {
                        boost::system::error_code ec;
                        socket_->close(ec);
                    }
                    callback_ = nullptr;
                    local_endpoint_ = {};
                }

                void StartReceive() noexcept {
                    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
                    if (closed_.load(std::memory_order_acquire) || !socket_) {
                        return;
                    }
                    // Initiation is serialized with CloseLocked() under lifecycle_mutex_;
                    // the completion handler never runs inline, so holding the lock across
                    // async_receive_from cannot deadlock.
                    socket_->async_receive_from(
                        boost::asio::buffer(receive_buffer_),
                        receive_sender_,
                        [self = shared_from_this()](
                                const boost::system::error_code& ec,
                                std::size_t bytes) noexcept {
                            if (self->closed_.load(std::memory_order_acquire)) {
                                return;
                            }
                            // Copy the callback under the lock, then release it before
                            // invoking: user code may call Close()/SendTo() re-entrantly.
                            P2PDatagramReceiveCallback callback;
                            {
                                std::lock_guard<std::mutex> lock(self->lifecycle_mutex_);
                                callback = self->callback_;
                            }
                            if (ec) {
                                // A remote oversized datagram must not destroy an
                                // otherwise healthy shared receive chain.
                                if (ec == boost::asio::error::message_size) {
                                    self->StartReceive();
                                    return;
                                }
                                if (callback &&
                                    ec != boost::asio::error::operation_aborted) {
                                    callback(P2PDatagramReceiveStatus::Error,
                                        {}, nullptr, 0);
                                }
                                return;
                            }
                            // Linux can report truncated UDP receives as success.
                            // The extra byte distinguishes a complete maximum-size
                            // packet from a remotely oversized datagram.
                            if (bytes > P2P_MAX_PACKET_SIZE) {
                                self->StartReceive();
                                return;
                            }
                            if (callback) {
                                callback(P2PDatagramReceiveStatus::Packet,
                                    self->receive_sender_,
                                    self->receive_buffer_.data(),
                                    static_cast<int>(bytes));
                            }
                            self->StartReceive();
                        });
                }

            private:
                boost::asio::io_context& io_context_;
                std::shared_ptr<ISocketProtector> protector_;
                // lifecycle_mutex_ serializes socket_/callback_/local_endpoint_ access across
                // Start/SendTo/Close and the receive completion chain.
                mutable std::mutex lifecycle_mutex_;
                std::unique_ptr<boost::asio::ip::udp::socket> socket_;
                P2PDatagramReceiveCallback callback_;
                boost::asio::ip::udp::endpoint local_endpoint_;
                boost::asio::ip::udp::endpoint receive_sender_;
                std::array<uint8_t, P2P_MAX_PACKET_SIZE + 1> receive_buffer_{};
                std::atomic<bool> started_{false};
                std::atomic<bool> closed_{false};
            };

            class NativeSocketP2PDatagramTransportFactory final
                : public IP2PDatagramTransportFactory {
            public:
                explicit NativeSocketP2PDatagramTransportFactory(
                        const std::shared_ptr<ISocketProtector>& protector) noexcept
                    : protector_(protector) {}

                std::shared_ptr<IP2PDatagramTransport> Create(
                        boost::asio::io_context& io_context) noexcept override {
                    return std::make_shared<NativeSocketP2PDatagramTransport>(
                        io_context, protector_);
                }

            private:
                std::shared_ptr<ISocketProtector> protector_;
            };

        }

        std::shared_ptr<IP2PDatagramTransportFactory>
        CreateNativeSocketP2PDatagramTransportFactory(
                const std::shared_ptr<ISocketProtector>& protector) noexcept {
#if defined(_IPHONE)
            (void)protector;
            return nullptr;
#else
            if (!protector) {
                return nullptr;
            }
            return std::make_shared<NativeSocketP2PDatagramTransportFactory>(protector);
#endif
        }

    }
}
