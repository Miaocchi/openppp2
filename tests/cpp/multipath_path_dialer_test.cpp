#define BOOST_TEST_MODULE multipath_path_dialer_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/multipath/PathDialer.h>
#include <ppp/configurations/AppConfiguration.h>
#include <ppp/configurations/MultipathConfiguration.h>
#include <ppp/coroutines/YieldContext.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

int GetIntOption(int handle, int level, int name) noexcept {
#if defined(_WIN32)
    int value = 0;
    int length = sizeof(value);
    if (::getsockopt(static_cast<SOCKET>(handle), level, name,
            reinterpret_cast<char*>(&value), &length) == SOCKET_ERROR) {
        return -1;
    }
    return value;
#else
    int value = 0;
    socklen_t length = sizeof(value);
    if (::getsockopt(handle, level, name, &value, &length) == -1) {
        return -1;
    }
    return value;
#endif
}

bool WaitFor(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}
}

// The multipath dialer builds its own TCP socket instead of going through the
// primary NewAsynchronousSocket() helper.  It must still apply the configured
// TCP window overrides and transport options before connect, otherwise
// multipath carriers run with kernel defaults while the main link does not.
BOOST_AUTO_TEST_CASE(connect_applies_primary_tcp_socket_options) {
    constexpr int window = 64 * 1024;

    auto context = std::make_shared<asio::io_context>();
    auto guard = asio::make_work_guard(*context);
    tcp::acceptor acceptor(*context, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    auto peer = std::make_shared<tcp::socket>(*context);
    acceptor.async_accept(*peer, [](const boost::system::error_code&) noexcept {});

    auto configuration = std::make_shared<ppp::configurations::AppConfiguration>();
    configuration->tcp.cwnd = window;
    configuration->tcp.rwnd = window;
    configuration->tcp.turbo = true;
    configuration->tcp.fast_open = false;

    ppp::configurations::MultipathConfiguration::Path path;
    path.id = "dialer-options";
    path.transport = "tcp";
    path.connect_host = "127.0.0.1";
    path.port = acceptor.local_endpoint().port();

    auto strand = std::make_shared<ppp::threading::Executors::Strand>(asio::make_strand(*context));
    auto attempt = ppp::app::multipath::PathAttempt::Start(context, strand, 5000, []() noexcept {});
    BOOST_REQUIRE(attempt);

    std::atomic<bool> captured{false};
    std::atomic<int> sndbuf{0};
    std::atomic<int> rcvbuf{0};
    std::atomic<int> nodelay{0};
    std::atomic<int> keepalive{0};
    std::atomic<bool> finished{false};

    const bool spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context, strand.get(),
        [&](ppp::coroutines::YieldContext& y) noexcept {
            const ppp::app::multipath::PathDialer::TransmissionFactory factory =
                [&](const ppp::configurations::MultipathConfiguration::Path&,
                    const ppp::app::multipath::PathDialer::SocketPtr& socket) noexcept
                    -> ppp::app::multipath::PathDialer::TransmissionPtr {
                    if (socket && socket->is_open()) {
                        const int handle = socket->native_handle();
                        sndbuf.store(GetIntOption(handle, SOL_SOCKET, SO_SNDBUF));
                        rcvbuf.store(GetIntOption(handle, SOL_SOCKET, SO_RCVBUF));
                        nodelay.store(GetIntOption(handle, IPPROTO_TCP, TCP_NODELAY));
                        keepalive.store(GetIntOption(handle, SOL_SOCKET, SO_KEEPALIVE));
                        captured.store(true);
                    }
                    return nullptr;
                };
            ppp::app::multipath::PathDialer::Connect(context, strand, configuration, path, attempt,
                ppp::app::multipath::PathDialer::AddressLeaseAcquire(),
                ppp::app::multipath::PathDialer::Protector(),
                factory, y);
            finished.store(true);
        });

    std::thread runner;
    if (spawned) {
        runner = std::thread([context]() noexcept { context->run(); });
    }
    const bool completed = spawned && WaitFor([&]() { return finished.load(); });

    guard.reset();
    context->stop();
    if (runner.joinable()) {
        runner.join();
    }

    BOOST_REQUIRE(spawned);
    BOOST_REQUIRE(completed);
    BOOST_TEST(captured.load());
    BOOST_TEST(nodelay.load() == 1);
    BOOST_TEST(keepalive.load() == 1);
    BOOST_TEST(sndbuf.load() >= window);
    BOOST_TEST(rcvbuf.load() >= window);
}
