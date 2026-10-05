#define BOOST_TEST_MODULE p2p_v2_exchanger_recovery_test
#include <boost/test/included/unit_test.hpp>
#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/app/client/ClientFrpRegistry.h>
#include <ppp/configurations/AppConfiguration.h>
#include <ppp/p2p/P2PCapabilityGate.h>
#include <ppp/threading/Executors.h>
#include "support/p2p_v2_fixture.h"

struct IsolatedRuntime {
    IsolatedRuntime() { ppp::global::cctor(); }
    ~IsolatedRuntime() { ppp::threading::Executors::Exit(); }
};
BOOST_GLOBAL_FIXTURE(IsolatedRuntime);

namespace ppp::app::client {
namespace {
class Relay final : public ppp::transmissions::ITransmission {
public:
    unsigned disposals = 0;
    ppp::p2p::P2PSessionExporter exporter = p2p_v2_test::Exporter(7);
    Relay(const ContextPtr& c, const AppConfigurationPtr& config)
        : ITransmission(c, std::make_shared<boost::asio::strand<boost::asio::io_context::executor_type>>(c->get_executor()), config) {}
    bool ShiftToScheduler() noexcept override { return false; }
    void Dispose() noexcept override { ++disposals; ITransmission::Dispose(); }
    boost::asio::ip::tcp::endpoint GetRemoteEndPoint() noexcept override { return {}; }
    std::shared_ptr<Byte> DoReadBytes(YieldContext&, int) noexcept override { return {}; }
    bool DoWriteBytes(std::shared_ptr<Byte>, int, int, const AsynchronousWriteBytesCallback& cb) noexcept override { cb(true); return true; }
    bool ExportAuthenticatedSessionKey(const char* label, const uint8_t* ctx, std::size_t n,
        uint8_t* out, std::size_t size) noexcept override { return exporter(label, ctx, n, out, size); }
};
class Datagram final : public ppp::p2p::IP2PDatagramTransport {
public:
    bool ready = true;
    unsigned closes = 0;
    ppp::p2p::P2PDatagramReceiveCallback receive;
    std::vector<std::vector<uint8_t>> sent;
    bool IsReady() const noexcept override { return ready; }
    bool Start(const ppp::p2p::P2PDatagramReceiveCallback& cb) noexcept override { receive = cb; return ready; }
    boost::asio::ip::udp::endpoint LocalEndpoint() const noexcept override {
        return {boost::asio::ip::make_address("192.0.2.1"), 4001};
    }
    bool SendTo(const uint8_t* bytes, int n, const boost::asio::ip::udp::endpoint&) noexcept override {
        if (!ready) return false;
        sent.emplace_back(bytes, bytes + n); return true;
    }
    void Close() noexcept override { ready = false; ++closes; }
};
class Mapping final : public ppp::app::protocol::VirtualEthernetMappingPort {
public:
    unsigned ticks = 0;
    Mapping(const std::shared_ptr<ppp::app::protocol::VirtualEthernetLinklayer>& e,
        const std::shared_ptr<Relay>& tx) : VirtualEthernetMappingPort(e, tx, true, true, 4321) {}
    bool Update(UInt64) noexcept override { ++ticks; return true; }
};
}

// Only dependency boundaries are replaced; recovery and callback validation run
// on the actual exchanger compiled from the production translation units.
struct P2PExchangerRecoveryTestAccess {
    using Exchanger = VEthernetExchanger;
    std::shared_ptr<boost::asio::io_context> context = std::make_shared<boost::asio::io_context>();
    std::shared_ptr<ppp::configurations::AppConfiguration> config = std::make_shared<ppp::configurations::AppConfiguration>();
    std::shared_ptr<Relay> tx = std::make_shared<Relay>(context, config);
    std::shared_ptr<Exchanger> exchanger = std::make_shared<Exchanger>(nullptr, config, context, Int128(1));
    std::shared_ptr<Datagram> socket;
    uint64_t now = 100000;
    unsigned registrations = 0, renews = 0;
    bool registration_ok = true, install_socket = true;
    ppp::coroutines::YieldContext* suspended = nullptr;
    bool suspend_registration = false;
    P2PExchangerRecoveryTestAccess() {
        auto& e = *exchanger;
        e.transmission_ = tx;
        e.p2p_offer_generation_ = 7;
        e.p2p_v2_selected_ = true;
        e.p2p_registered_virtual_ip_ = 1;
        e.p2p_peer_virtual_ip_ = 2;
        e.recovery_test_hooks_.enabled = true;
        e.recovery_test_hooks_.now = [this] { return now; };
        e.recovery_test_hooks_.control = [this](const char* action) { if (std::string(action) == "renew") ++renews; };
        e.recovery_test_hooks_.register_candidates = [this](const Exchanger::ITransmissionPtr&, Exchanger::YieldContext& y) {
            ++registrations;
            if (install_socket) Install();
            if (suspend_registration) { suspended = &y; y.Suspend(); }
            return registration_ok;
        };
        Install();
    }
    ~P2PExchangerRecoveryTestAccess() {
        exchanger->Finalize();
        Pump();
    }
    void Pump() { context->restart(); context->poll(); context->restart(); }
    void Install() {
        auto& e = *exchanger;
        socket = std::make_shared<Datagram>();
        e.p2p_candidate_transport_ = socket;
        e.p2p_registered_transmission_ = tx;
        e.p2p_transport_registration_id_ = ++e.p2p_transport_registration_sequence_;
        const auto registration = e.p2p_transport_registration_id_;
        socket->Start([this, registration](ppp::p2p::P2PDatagramReceiveStatus status,
            const boost::asio::ip::udp::endpoint& sender, const uint8_t* bytes, int n) {
            exchanger->HandleP2PDatagram(tx, 7, registration, status, sender, bytes, n);
        });
    }
    void Error() { socket->receive(ppp::p2p::P2PDatagramReceiveStatus::Error, {}, nullptr, 0); }
    void Retry() { exchanger->RecoverP2PTransport(tx, 7, exchanger->p2p_transport_registration_id_); }
    uint64_t Deadline() const { return exchanger->p2p_retry_at_ms_; }
    bool Running() const { return exchanger->p2p_recovery_running_; }
    uint64_t Registration() const { return exchanger->p2p_transport_registration_id_; }
    bool Selected() const { return exchanger->p2p_v2_selected_; }
    void CheckRelay() {
        BOOST_TEST(exchanger->GetTransmission() == tx);
        BOOST_TEST(Selected());
        BOOST_TEST(!exchanger->disposed_.load());
    }
    void DisableGate() { exchanger->recovery_test_hooks_.enabled = false; }
    void FailPost() { exchanger->recovery_test_hooks_.post = [](ppp::function<void()>) { return false; }; }
    void FailSpawn() { exchanger->recovery_test_hooks_.spawn = [](ppp::coroutines::YieldContext::SpawnHander) { return false; }; }
    void ReplaceAttempt() {
        exchanger->p2p_recovery_running_ = false;
        ++exchanger->p2p_recovery_attempt_;
    }
    void CheckEmptyChannel() {
        BOOST_TEST(!exchanger->p2p_v2_channel_.Snapshot().has_current);
        BOOST_TEST(!exchanger->p2p_v2_channel_.Snapshot().has_pending);
    }
    void Handshake() {
        using namespace ppp::p2p;
        p2p_v2_test::Pair peer;
        auto& e = *exchanger;
        Int128ToBytes(e.GetId(), peer.contexts[0].local_session_id.data());
        peer.contexts[0].local_peer_id = {};
        std::memcpy(peer.contexts[0].local_peer_id.data() + 12, &e.p2p_registered_virtual_ip_, 4);
        peer.Offer(now);
        ppp::app::protocol::P2PControlMessage offer;
        offer.enabled = true; offer.action = "offer-v2";
        offer.virtual_ip = e.p2p_registered_virtual_ip_; offer.peer_virtual_ip = e.p2p_peer_virtual_ip_;
        offer.supported_versions = {1, 2}; offer.candidate_revision = offer.peer_candidate_revision = 1;
        ppp::app::protocol::P2PEndpointCandidate local, remote;
        local.endpoint = "192.0.2.1:4001"; remote.endpoint = "192.0.2.2:4002";
        offer.local_candidates = {local}; offer.candidates = {remote};
        offer.authenticated_offer_v2 = peer.encoded[0].c_str();
        e.p2p_candidate_history_[1] = offer.local_candidates;
        e.HandleP2PV2RelayOffer(tx, offer); Pump();
        BOOST_REQUIRE(e.p2p_v2_channel_.Snapshot().has_pending);
        auto reverse = peer.Probe(1, now);
        const boost::asio::ip::udp::endpoint endpoint(boost::asio::ip::make_address("192.0.2.2"), 4002);
        socket->receive(P2PDatagramReceiveStatus::Packet, endpoint, reverse.data(), static_cast<int>(reverse.size()));
        Pump();
        std::size_t cursor = 0;
        while (cursor < socket->sent.size()) {
            BOOST_REQUIRE(cursor < 20u);
            const auto packet = socket->sent[cursor++];
            P2PV2DataPacketHeader header;
            if (ParseP2PV2DataPacketHeader(packet, header)) {
                std::vector<uint8_t> plaintext;
                BOOST_REQUIRE(peer.channels[1].OpenData(packet, peer.contexts[0].local_candidates[0], now, 7, plaintext));
                continue;
            }
            P2PV2ControlResult result;
            BOOST_REQUIRE(peer.channels[1].HandleControl(packet, peer.contexts[0].local_candidates[0], now, 7, result));
            for (const auto& reply : result.outbound) {
                socket->receive(P2PDatagramReceiveStatus::Packet, endpoint,
                    reply.datagram.data(), static_cast<int>(reply.datagram.size()));
                Pump();
            }
        }
        BOOST_TEST(e.p2p_v2_channel_.Snapshot().has_current);
        BOOST_TEST(peer.channels[1].Snapshot().has_current);
        BOOST_TEST(static_cast<int>(e.p2p_state_.load()) == static_cast<int>(P2PState::Direct));
        std::vector<uint8_t> legacy(158);
        legacy[0] = 1; legacy[1] = 1;
        const auto sent_before = socket->sent.size();
        socket->receive(P2PDatagramReceiveStatus::Packet, endpoint, legacy.data(), static_cast<int>(legacy.size()));
        Pump();
        BOOST_TEST(socket->sent.size() == sent_before);
        BOOST_TEST(e.p2p_v2_channel_.Snapshot().has_current); BOOST_TEST(Selected());
    }
    void UpdateWithFrp() {
        auto& e = *exchanger;
        e.switcher_ = std::make_shared<VEthernetNetworkSwitcher>(context, false, false, false, config);
        e.network_state_ = Exchanger::NetworkState_Established;
        e.keepalive_policy_.OnConnected(ppp::threading::Executors::GetTickCount(), 50000);
        auto mapping = std::make_shared<Mapping>(exchanger, tx);
        BOOST_REQUIRE(e.frp_registry_->Add(true, true, 4321, mapping));
        BOOST_REQUIRE(e.Update()); Pump();
        BOOST_TEST(mapping->ticks == 1u);
        BOOST_TEST(tx->disposals == 0u);
        CheckRelay();
    }
};
}

using Fixture = ppp::app::client::P2PExchangerRecoveryTestAccess;

BOOST_AUTO_TEST_CASE(socket_error_closes_immediately_and_invalidates_registration) {
    Fixture f;
    f.Handshake();
    auto old = f.socket;
    f.Error();
    BOOST_TEST(old->closes == 1u);
    BOOST_TEST(f.Registration() == 0u);
    BOOST_TEST(f.Deadline() == f.now + 1000);
    f.CheckEmptyChannel(); f.CheckRelay();
    old->receive(ppp::p2p::P2PDatagramReceiveStatus::Error, {}, nullptr, 0);
    BOOST_TEST(old->closes == 1u);
    f.now = f.Deadline(); f.Retry(); f.Pump();
    BOOST_TEST(f.registrations == 1u);
    BOOST_TEST(f.Registration() != 0u);
    BOOST_TEST(!f.Running()); f.CheckRelay();
}

BOOST_AUTO_TEST_CASE(failed_registration_obeys_one_two_four_eight_ten_second_backoff) {
    Fixture f; f.registration_ok = false;
    f.Error();
    const uint64_t expected[] = {1000, 2000, 4000, 8000, 10000, 10000};
    for (auto delay : expected) {
        BOOST_TEST(f.Deadline() == f.now + delay);
        f.now = f.Deadline() - 1; f.Retry(); f.Pump();
        const auto before = f.registrations;
        ++f.now; f.Retry(); f.Pump();
        BOOST_TEST(f.registrations == before + 1);
        BOOST_TEST(f.Registration() == 0u); f.CheckRelay();
    }
}

BOOST_AUTO_TEST_CASE(production_gate_stays_closed_and_test_capability_is_explicit) {
    BOOST_TEST(!ppp::p2p::ProductionAuthenticatedControlV1Ready);
    Fixture f; f.DisableGate(); f.Error();
    f.now = f.Deadline(); f.Retry(); f.Pump();
    BOOST_TEST(f.registrations == 0u); BOOST_TEST(!f.Running());
}

BOOST_AUTO_TEST_CASE(registration_failure_without_socket_waits_and_does_not_send_renew) {
    Fixture f; f.registration_ok = false; f.install_socket = false;
    f.Error(); f.now = f.Deadline(); f.Retry(); f.Pump();
    BOOST_TEST(f.registrations == 1u); BOOST_TEST(!f.Running());
    BOOST_TEST(f.Deadline() == f.now + 2000);
    BOOST_TEST(f.renews == 0u); BOOST_TEST(f.Registration() == 0u); f.CheckRelay();
}

BOOST_AUTO_TEST_CASE(post_and_spawn_failure_release_running_and_retry) {
    for (bool post : {false, true}) {
        Fixture f; f.Error(); f.now = f.Deadline();
        if (post) f.FailPost(); else f.FailSpawn();
        f.Retry(); f.Pump();
        BOOST_TEST(!f.Running()); BOOST_TEST(f.Deadline() == f.now + 1000);
        BOOST_TEST(f.registrations == 0u); f.CheckRelay();
    }
}

BOOST_AUTO_TEST_CASE(same_generation_old_queued_callback_cannot_rebuild) {
    Fixture f; f.Error(); f.now = f.Deadline();
    f.Retry(); f.ReplaceAttempt(); f.Pump();
    BOOST_TEST(f.registrations == 0u); BOOST_TEST(f.Registration() == 0u);
}

BOOST_AUTO_TEST_CASE(same_generation_old_coroutine_completion_cannot_overwrite_new_socket) {
    Fixture f; f.suspend_registration = true; f.Error(); f.now = f.Deadline();
    f.Retry(); f.Pump();
    BOOST_REQUIRE(f.suspended);
    auto old = f.socket; f.Error();
    f.suspend_registration = false; f.now = f.Deadline(); f.Retry(); f.Pump();
    const auto current = f.socket;
    const auto registration = f.Registration();
    const auto renews = f.renews;
    f.suspended->R(); f.Pump();
    BOOST_TEST(f.socket == current); BOOST_TEST(f.Registration() == registration);
    BOOST_TEST(f.renews == renews); BOOST_TEST(old->closes == 1u); f.CheckRelay();
}

BOOST_AUTO_TEST_CASE(rebuilt_transport_completes_authenticated_v2_handshake_and_rejects_v1) {
    Fixture f; f.Error(); f.now = f.Deadline(); f.Retry(); f.Pump();
    BOOST_TEST(f.renews == 1u); f.Handshake();
}

BOOST_AUTO_TEST_CASE(actual_update_keeps_frp_maintenance_during_recovery_and_v2_selection) {
    {
        Fixture f; f.Error(); f.UpdateWithFrp();
        BOOST_TEST(f.registrations == 0u);
    }
    {
        Fixture f; f.UpdateWithFrp();
    }
}
