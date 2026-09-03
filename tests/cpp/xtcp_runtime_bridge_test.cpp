// xtcp_runtime_bridge_test.cpp
//
// Unprivileged end-to-end tests for the XTCP runtime bridge: hand-built
// IPv4/TCP segments (valid checksums, via the pinned upstream harness) enter
// through XtcpRuntime::SubmitIPv4Tcp, the stack's L3 output is captured, and
// the second leg is a real loopback TCP echo server. Covers:
//   - endpoint byte order (10.0.0.2 must not become 2.0.0.10)
//   - full handshake + bidirectional byte-exact data
//   - bidirectional half-close (data+FIN tail delivered before EOF, the
//     reverse direction still sends)
//   - RST before the first leg is ready cancels the pending flow without
//     injecting the deferred SYN
//   - connect/close churn leaves no flows behind
//   - runtime stats counters reflect the traffic

#include <ppp/app/client/xtcp/XtcpRuntime.h>

#include <xtcp/buf/bufref.h>
#include <harness/raw_pkt.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,  \
                         #condition);                                        \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using ppp::app::client::xtcp::XtcpFirstLegHooks;
using ppp::app::client::xtcp::XtcpRuntime;
using ppp::app::runtime::RuntimeXtcpStats;

constexpr std::uint8_t kFin = 0x01;
constexpr std::uint8_t kSyn = 0x02;
constexpr std::uint8_t kRst = 0x04;
constexpr std::uint8_t kAck = 0x10;

// Client (first leg) and service (second leg) addresses inside the tunnel.
constexpr std::uint32_t kClientIp = 0x0A000002u;  // 10.0.0.2
constexpr std::uint32_t kServiceIp = 0x0A000001u; // 10.0.0.1
constexpr std::uint16_t kServicePort = 80;
constexpr std::uint16_t kClientPort = 40000;

struct TcpView final {
    std::uint32_t src_ip = 0;
    std::uint32_t dst_ip = 0;
    std::uint16_t sport = 0;
    std::uint16_t dport = 0;
    std::uint32_t seq = 0;
    std::uint32_t ack = 0;
    std::uint8_t flags = 0;
    std::vector<Byte> payload;
};

std::uint32_t ReadBe32(const Byte* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
        (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::uint16_t ReadBe16(const Byte* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

bool ParseTcp(const std::vector<Byte>& packet, TcpView& view) noexcept {
    if (packet.size() < 40 || (packet[0] >> 4) != 4 || packet[9] != 6) {
        return false;
    }
    const std::size_t ip_header = (packet[0] & 0x0F) * 4;
    if (packet.size() < ip_header + 20) {
        return false;
    }
    const Byte* tcp = packet.data() + ip_header;
    const std::size_t tcp_header = (tcp[12] >> 4) * 4;
    if (packet.size() < ip_header + tcp_header) {
        return false;
    }
    view.src_ip = ReadBe32(packet.data() + 12);
    view.dst_ip = ReadBe32(packet.data() + 16);
    view.sport = ReadBe16(tcp);
    view.dport = ReadBe16(tcp + 2);
    view.seq = ReadBe32(tcp + 4);
    view.ack = ReadBe32(tcp + 8);
    view.flags = tcp[13];
    view.payload.assign(tcp + tcp_header, packet.data() + packet.size());
    return true;
}

// Waits until `predicate` is true, pumping nothing (the io_context runs on
// its own thread). Returns false on timeout.
bool WaitFor(const std::function<bool()>& predicate, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// A half-close-aware loopback echo server: echoes bytes back, and when the
// peer half-closes (read EOF) it shuts down its write side after every
// received byte was echoed, then closes the socket - modelling the ppp-side
// connection object going away (Dispose closes the accepted socket).
// Accepts any number of sequential connections.
class EchoServer final : public std::enable_shared_from_this<EchoServer> {
public:
    EchoServer(boost::asio::io_context& context, std::uint16_t port)
        : acceptor_(context, boost::asio::ip::tcp::endpoint(
              boost::asio::ip::address_v4::loopback(), port)) {
        boost::system::error_code ec;
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    }

    void Start() {
        DoAccept();
    }

    boost::asio::ip::tcp::endpoint Endpoint() const {
        return acceptor_.local_endpoint();
    }

    std::uint64_t EchoedBytes() const {
        return echoed_bytes_.load(std::memory_order_relaxed);
    }

    std::uint64_t EofCount() const {
        return eof_count_.load(std::memory_order_relaxed);
    }

private:
    class Session final : public std::enable_shared_from_this<Session> {
    public:
        Session(boost::asio::ip::tcp::socket socket, EchoServer& owner)
            : socket_(std::move(socket)), owner_(owner) {}

        void Start() {
            DoRead();
        }

    private:
        void DoRead() {
            socket_.async_read_some(boost::asio::buffer(read_buffer_),
                [self = shared_from_this()](const boost::system::error_code& ec,
                    std::size_t length) noexcept {
                    if (ec == boost::asio::error::eof || (length == 0 && !ec)) {
                        self->owner_.eof_count_.fetch_add(1, std::memory_order_relaxed);
                        boost::system::error_code ignored;
                        self->socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ignored);
                        self->socket_.close(ignored);
                        return;
                    }
                    if (ec) {
                        return;
                    }
                    self->owner_.echoed_bytes_.fetch_add(length, std::memory_order_relaxed);
                    boost::system::error_code write_ec;
                    boost::asio::write(self->socket_,
                        boost::asio::buffer(self->read_buffer_.data(), length), write_ec);
                    if (write_ec) {
                        // The peer (app side) reset mid-echo; stop echoing.
                        return;
                    }
                    self->DoRead();
                });
        }

        boost::asio::ip::tcp::socket socket_;
        EchoServer& owner_;
        std::array<char, 16384> read_buffer_{};
    };

    void DoAccept() {
        acceptor_.async_accept([self = shared_from_this()](
                const boost::system::error_code& ec,
                boost::asio::ip::tcp::socket socket) noexcept {
            if (ec) {
                return;
            }
            std::make_shared<Session>(std::move(socket), *self)->Start();
            self->DoAccept();
        });
    }

    boost::asio::ip::tcp::acceptor acceptor_;
    std::atomic<std::uint64_t> echoed_bytes_{0};
    std::atomic<std::uint64_t> eof_count_{0};
};

struct AcceptedFlow final {
    boost::asio::ip::tcp::endpoint first_leg_local;   // tunnel client endpoint
    boost::asio::ip::tcp::endpoint first_leg_remote;  // tunnel service endpoint
    std::uint16_t source_port = 0;
    std::uint64_t runtime_generation = 0;
    std::uint64_t flow_generation = 0;
    std::weak_ptr<XtcpFirstLegHooks> hooks;
};

// Owns the io_context thread, the runtime under test, the captured L3 output
// and the accepted/cancelled flow records.
class Bridge final {
public:
    Bridge() {
        context_ = std::make_shared<boost::asio::io_context>();
        work_guard_ = std::make_unique<boost::asio::io_context::work>(*context_);
        io_thread_ = std::thread([this]() noexcept {
            boost::system::error_code ec;
            context_->run(ec);
        });
    }

    ~Bridge() {
        if (runtime_) {
            runtime_->Stop();
        }
        work_guard_.reset();
        context_->stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    }

    bool StartRuntime(std::uint16_t echo_port) {
        echo_ = std::make_shared<EchoServer>(*context_, echo_port);
        echo_->Start();
        const auto listener_endpoint = echo_->Endpoint();
        runtime_ = std::make_shared<XtcpRuntime>(
            context_,
            [this](std::shared_ptr<Byte>&& data, int length) noexcept {
                std::lock_guard<std::mutex> lock(output_mutex_);
                const Byte* bytes = data ? data.get() : nullptr;
                if (bytes != nullptr && length > 0) {
                    output_.emplace_back(bytes, bytes + length);
                }
                return true;
            },
            [listener_endpoint]() noexcept { return listener_endpoint; },
            [this](const boost::asio::ip::tcp::endpoint& local,
                   const boost::asio::ip::tcp::endpoint& remote,
                   std::uint16_t source_port, std::uint64_t runtime_generation,
                   std::uint64_t flow_generation,
                   const std::weak_ptr<XtcpFirstLegHooks>& hooks, int fd) noexcept {
                if (fd >= 0) {
                    // XTCP-VNET-BRIDGE-BYPASS-001: socketpair 路径 - 用阻塞双线程
                    // 中继 fd <-> echo server, 模拟 netstack 泵的 socket 语义。
                    std::thread([this, fd]() noexcept {
                        const int srv = ::socket(AF_INET, SOCK_STREAM, 0);
                        if (srv < 0) {
                            ::close(fd);
                            return;
                        }
                        sockaddr_in addr{};
                        addr.sin_family = AF_INET;
                        addr.sin_port = htons(static_cast<uint16_t>(echo_->Endpoint().port()));
                        addr.sin_addr.s_addr = htonl(0x7F000001);
                        if (::connect(srv, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
                            ::close(srv);
                            ::close(fd);
                            return;
                        }
                        auto pump = [](int a, int b) noexcept {
                            char buf[65536];
                            ssize_t n;
                            while ((n = ::read(a, buf, sizeof(buf))) > 0) {
                                std::size_t off = 0;
                                while (off < static_cast<std::size_t>(n)) {
                                    ssize_t w = ::send(b, buf + off, static_cast<std::size_t>(n) - off, MSG_NOSIGNAL);
                                    if (w <= 0) return;
                                    off += static_cast<std::size_t>(w);
                                }
                            }
                            ::shutdown(b, SHUT_WR);
                        };
                        std::thread up([pump, fd, srv]() noexcept { pump(fd, srv); ::close(srv); });
                        std::thread down([pump, fd, srv]() noexcept { pump(srv, fd); ::close(fd); });
                        up.detach();
                        down.detach();
                    }).detach();
                }
                std::lock_guard<std::mutex> lock(flows_mutex_);
                AcceptedFlow flow;
                flow.first_leg_local = local;
                flow.first_leg_remote = remote;
                flow.source_port = source_port;
                flow.runtime_generation = runtime_generation;
                flow.flow_generation = flow_generation;
                flow.hooks = hooks;
                accepted_.push_back(flow);
                return true;
            },
            [this](std::uint16_t source_port, std::uint64_t) noexcept {
                std::lock_guard<std::mutex> lock(flows_mutex_);
                cancelled_.push_back(source_port);
            });
        if (!runtime_->Start()) {
            return false;
        }
        runtime_->MarkReady();
        return runtime_->IsReady();
    }

    void Submit(const std::vector<Byte>& packet) {
        runtime_->SubmitIPv4Tcp(packet.data(), static_cast<int>(packet.size()));
    }

    // Pops the first captured output packet matching `predicate`.
    bool WaitOutput(const std::function<bool(const TcpView&)>& predicate, TcpView& view,
            int timeout_ms = 5000) {
        bool found = false;
        const bool ok = WaitFor([&]() {
            std::lock_guard<std::mutex> lock(output_mutex_);
            for (auto it = output_.begin(); it != output_.end(); ++it) {
                TcpView candidate;
                if (ParseTcp(*it, candidate) && predicate(candidate)) {
                    view = std::move(candidate);
                    output_.erase(it);
                    found = true;
                    return true;
                }
            }
            return false;
        }, timeout_ms);
        return ok && found;
    }

    int OutputCount() {
        std::lock_guard<std::mutex> lock(output_mutex_);
        return static_cast<int>(output_.size());
    }

    AcceptedFlow LastAccepted() {
        std::lock_guard<std::mutex> lock(flows_mutex_);
        return accepted_.back();
    }

    std::size_t AcceptedCount() {
        std::lock_guard<std::mutex> lock(flows_mutex_);
        return accepted_.size();
    }

    std::size_t CancelledCount() {
        std::lock_guard<std::mutex> lock(flows_mutex_);
        return cancelled_.size();
    }

    void ReadyLastFlow() {
        AcceptedFlow flow = LastAccepted();
        if (const std::shared_ptr<XtcpFirstLegHooks> hooks = flow.hooks.lock()) {
            std::fprintf(stderr, "[bp-dbg] test ReadyLastFlow hooks_locked=%d gen=%llu\n",
                (int)(flow.hooks.lock() != nullptr), (unsigned long long)flow.flow_generation);
            hooks->OnFirstLegReady(flow.runtime_generation, flow.flow_generation);
        }
    }

    void CloseLastFlowFirstLeg() {
        AcceptedFlow flow = LastAccepted();
        if (const std::shared_ptr<XtcpFirstLegHooks> hooks = flow.hooks.lock()) {
            hooks->OnFirstLegClosed(flow.runtime_generation, flow.flow_generation);
        }
    }

    RuntimeXtcpStats Stats() const {
        return runtime_->SnapshotStats();
    }

    std::shared_ptr<EchoServer> echo_;

private:
    std::shared_ptr<boost::asio::io_context> context_;
    std::unique_ptr<boost::asio::io_context::work> work_guard_;
    std::thread io_thread_;
    std::shared_ptr<XtcpRuntime> runtime_;
    std::mutex output_mutex_;
    std::deque<std::vector<Byte>> output_;
    std::mutex flows_mutex_;
    std::vector<AcceptedFlow> accepted_;
    std::vector<std::uint16_t> cancelled_;
};

// Drives one full handshake and returns the established sequence numbers.
bool Handshake(Bridge& bridge, std::uint16_t client_port,
        std::uint32_t& client_next, std::uint32_t& server_next) {
    const std::size_t accepted_before = bridge.AcceptedCount();
    const std::uint32_t client_isn = 1000 + client_port;
    bridge.Submit(xtcp::harness::BuildIp4Tcp(
        kClientIp, kServiceIp, client_port, kServicePort, client_isn, 0, kSyn));
    if (!WaitFor([&]() { return bridge.AcceptedCount() > accepted_before; })) {
        return false;
    }
    bridge.ReadyLastFlow();
    TcpView syn_ack;
    if (!bridge.WaitOutput([](const TcpView& view) {
            return (view.flags & (kSyn | kAck)) == (kSyn | kAck);
        }, syn_ack)) {
        return false;
    }
    const std::uint32_t server_isn = syn_ack.seq;
    if (syn_ack.ack != client_isn + 1) {
        return false;
    }
    bridge.Submit(xtcp::harness::BuildIp4Tcp(
        kClientIp, kServiceIp, client_port, kServicePort,
        client_isn + 1, server_isn + 1, kAck));
    client_next = client_isn + 1;
    server_next = server_isn + 1;
    return true;
}

void TestEndpointByteOrder() {
    Bridge bridge;
    CHECK(bridge.StartRuntime(0));
    bridge.Submit(xtcp::harness::BuildIp4Tcp(
        kClientIp, kServiceIp, kClientPort, kServicePort, 5000, 0, kSyn));
    CHECK(WaitFor([&]() { return bridge.AcceptedCount() != 0; }));
    const AcceptedFlow flow = bridge.LastAccepted();
    // The client endpoint must read 10.0.0.2:40000, not 2.0.0.10.
    CHECK(flow.first_leg_local.address().to_string() == "10.0.0.2");
    CHECK(flow.first_leg_local.port() == kClientPort);
    CHECK(flow.first_leg_remote.address().to_string() == "10.0.0.1");
    CHECK(flow.first_leg_remote.port() == kServicePort);
    CHECK(flow.source_port != 0);
}

void TestHandshakeAndBidirectionalData() {
    Bridge bridge;
    CHECK(bridge.StartRuntime(0));
    std::uint32_t client_next = 0;
    std::uint32_t server_next = 0;
    CHECK(Handshake(bridge, kClientPort, client_next, server_next));

    const char payload[] = "hello-xtcp-bridge";
    constexpr std::uint32_t payload_len = sizeof(payload) - 1;
    bridge.Submit(xtcp::harness::BuildIp4Tcp(
        kClientIp, kServiceIp, kClientPort, kServicePort,
        client_next, server_next, kAck,
        reinterpret_cast<const Byte*>(payload), payload_len));

    // The echo must come back byte-exact through the stack's L3 output.
    TcpView echo;
    CHECK(bridge.WaitOutput([&](const TcpView& view) {
        return view.dport == kClientPort && !view.payload.empty();
    }, echo));
    CHECK(echo.payload.size() == payload_len);
    CHECK(echo.payload.size() == payload_len &&
        std::memcmp(echo.payload.data(), payload, payload_len) == 0);

    const RuntimeXtcpStats stats = bridge.Stats();
    CHECK(stats.ingress_submitted >= 3);
    CHECK(stats.ingress_injected >= 3);
    CHECK(stats.flows_opened == 1);
    CHECK(stats.flows_active == 1);
    CHECK(stats.output_packets >= 2);  // SYN+ACK, echo data (ACKs may coalesce)
    CHECK(stats.connector_read_bytes == payload_len);
    CHECK(stats.connector_written_bytes == payload_len);
    // The data path is fully event-driven, so no timer may have fired yet;
    // the idle watchdog must still keep the deadline-driven poll loop alive.
    CHECK(WaitFor([&]() { return bridge.Stats().timer_polls != 0; }, 1000));
}

void TestHalfCloseBothDirections() {
    Bridge bridge;
    CHECK(bridge.StartRuntime(0));
    std::uint32_t client_next = 0;
    std::uint32_t server_next = 0;
    CHECK(Handshake(bridge, kClientPort, client_next, server_next));

    // data+FIN in one segment: the payload must be delivered to the second
    // leg BEFORE the EOF (shutdown SHUT_WR after the write queue drains).
    const char tail[] = "tail-bytes-before-fin";
    constexpr std::uint32_t tail_len = sizeof(tail) - 1;
    bridge.Submit(xtcp::harness::BuildIp4Tcp(
        kClientIp, kServiceIp, kClientPort, kServicePort,
        client_next, server_next, kAck | kFin,
        reinterpret_cast<const Byte*>(tail), tail_len));

    CHECK(WaitFor([&]() { return bridge.echo_->EchoedBytes() == tail_len; }));
    CHECK(WaitFor([&]() { return bridge.echo_->EofCount() == 1; }));

    // After our FIN the reverse direction must still work: the echo of the
    // tail bytes arrives through the first leg.
    TcpView echo;
    CHECK(bridge.WaitOutput([&](const TcpView& view) {
        return view.dport == kClientPort && !view.payload.empty();
    }, echo));
    CHECK(echo.payload.size() == tail_len &&
        std::memcmp(echo.payload.data(), tail, tail_len) == 0);
}

void TestRstBeforeReadyCancelsFlow() {
    Bridge bridge;
    CHECK(bridge.StartRuntime(0));
    const std::uint32_t client_isn = 7000;
    bridge.Submit(xtcp::harness::BuildIp4Tcp(
        kClientIp, kServiceIp, kClientPort, kServicePort, client_isn, 0, kSyn));
    CHECK(WaitFor([&]() { return bridge.AcceptedCount() != 0; }));
    const std::uint16_t source_port = bridge.LastAccepted().source_port;

    // RST while the first leg is not ready: the pending flow is cancelled
    // and the deferred SYN must never reach the stack (no SYN+ACK output).
    bridge.Submit(xtcp::harness::BuildIp4Tcp(
        kClientIp, kServiceIp, kClientPort, kServicePort, client_isn + 1, 0, kRst));
    CHECK(WaitFor([&]() { return bridge.CancelledCount() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(bridge.OutputCount() == 0);
    const RuntimeXtcpStats stats = bridge.Stats();
    CHECK(stats.flows_opened == 1);
    CHECK(stats.flows_closed == 1);
    CHECK(stats.flows_active == 0);
    (void)source_port;
}

void TestConnectCloseChurn() {
    Bridge bridge;
    CHECK(bridge.StartRuntime(0));
    constexpr int kIterations = 256;
    for (int i = 0; i < kIterations; ++i) {
        const std::uint16_t client_port = static_cast<std::uint16_t>(41000 + i);
        std::uint32_t client_next = 0;
        std::uint32_t server_next = 0;
        CHECK(Handshake(bridge, client_port, client_next, server_next));
        if (failures != 0) {
            return;
        }
        // FIN closes the flow; the second leg half-close then lets the stack
        // run the full close handshake to kClosed/kTimeWait.
        bridge.Submit(xtcp::harness::BuildIp4Tcp(
            kClientIp, kServiceIp, client_port, kServicePort,
            client_next, server_next, kAck | kFin));
        CHECK(WaitFor([&]() { return bridge.echo_->EofCount() >= static_cast<std::uint64_t>(i + 1); }));
        // The ppp-side close notification marks the second leg gone; the echo
        // session's socket close then drives the connector to EOF and the
        // runtime closes the first leg gracefully: the stack emits its FIN
        // and the app ACKs it, completing the close handshake.
        bridge.CloseLastFlowFirstLeg();
        TcpView fin;
        CHECK(bridge.WaitOutput([&](const TcpView& view) {
            return view.dport == client_port && (view.flags & kFin) != 0;
        }, fin));
        if (failures != 0) {
            return;
        }
        bridge.Submit(xtcp::harness::BuildIp4Tcp(
            kClientIp, kServiceIp, client_port, kServicePort,
            client_next + 1, fin.seq + 1, kAck));
    }
    CHECK(WaitFor([&]() {
        const RuntimeXtcpStats stats = bridge.Stats();
        return stats.flows_opened == kIterations && stats.flows_closed == kIterations;
    }));
    const RuntimeXtcpStats stats = bridge.Stats();
    CHECK(stats.flows_active == 0);
    // Churn is a FIN-only lifecycle test (no data flows), so the queued-byte
    // ledger must simply never have been touched.
    CHECK(bridge.Stats().queued_bytes == 0);
}

// Regression for the queued-byte ledger double debit: tearing a flow down
// while a chunk is inside the in-flight async_write used to refund those
// bytes twice (teardown + late completion), wrapping the global gauge. Every
// teardown path must land the ledger back at exactly zero.
void TestQueuedBytesAccounting() {
    {
        // Normal path: after an echo completes, the ledger is drained but the
        // highwater peak proves bytes were accounted on the way through.
        Bridge bridge;
        CHECK(bridge.StartRuntime(0));
        std::uint32_t client_next = 0;
        std::uint32_t server_next = 0;
        CHECK(Handshake(bridge, kClientPort, client_next, server_next));
        const char payload[] = "accounting-probe";
        constexpr std::uint32_t payload_len = sizeof(payload) - 1;
        bridge.Submit(xtcp::harness::BuildIp4Tcp(
            kClientIp, kServiceIp, kClientPort, kServicePort,
            client_next, server_next, kAck,
            reinterpret_cast<const Byte*>(payload), payload_len));
        CHECK(WaitFor([&]() { return bridge.echo_->EchoedBytes() >= payload_len; }));
        CHECK(WaitFor([&]() { return bridge.Stats().queued_bytes == 0; }));
        CHECK(bridge.Stats().queued_bytes_highwater > 0);
    }
    {
        // Mid-flight teardown: queue a burst, then drop the second leg before
        // it drains. HandlePeerGone refunds only the unsubmitted tail and the
        // late completion debits its own chunk; the RST then closes the flow.
        Bridge bridge;
        CHECK(bridge.StartRuntime(0));
        std::uint32_t client_next = 0;
        std::uint32_t server_next = 0;
        CHECK(Handshake(bridge, kClientPort, client_next, server_next));
        static const std::vector<Byte> bulk(32000, Byte{0xAB});
        for (int i = 0; i < 18; ++i) {
            bridge.Submit(xtcp::harness::BuildIp4Tcp(
                kClientIp, kServiceIp, kClientPort, kServicePort,
                client_next, server_next, kAck, bulk.data(),
                static_cast<std::uint32_t>(bulk.size())));
            client_next += static_cast<std::uint32_t>(bulk.size());
        }
        bridge.CloseLastFlowFirstLeg();
        bridge.Submit(xtcp::harness::BuildIp4Tcp(
            kClientIp, kServiceIp, kClientPort, kServicePort, client_next, 0, kRst));
        CHECK(WaitFor([&]() { return bridge.Stats().flows_closed == 1; }));
        CHECK(WaitFor([&]() { return bridge.Stats().queued_bytes == 0; }));
        CHECK(bridge.Stats().queued_bytes_highwater > 0);
    }
}

} // namespace

int main() {
    ::setenv("OPENPPP2_XTCP_UNIX_BRIDGE", "1", 1);
    xtcp::buf::InitPools();
    TestEndpointByteOrder();
    TestHandshakeAndBidirectionalData();
    TestHalfCloseBothDirections();
    TestRstBeforeReadyCancelsFlow();
    TestConnectCloseChurn();
    TestQueuedBytesAccounting();
    xtcp::buf::ShutdownPools();

    if (failures != 0) {
        std::fprintf(stderr, "xtcp_runtime_bridge_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("xtcp_runtime_bridge_test: passed");
    return 0;
}
