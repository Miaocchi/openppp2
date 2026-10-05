#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/configurations/AppConfiguration.h>
#include <ppp/app/P2PCandidateAdapter.h>
#include <ppp/p2p/P2PV2Codec.h>
#include <ppp/p2p/P2PCapabilityGate.h>
#include <ppp/p2p/P2PSocketProtector.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/Socket.h>
#include <ppp/threading/Executors.h>

namespace ppp::app::client {
namespace {
using namespace ppp::p2p;
using ppp::threading::Executors;
using ppp::net::Ipep;

P2PCandidateEndpoint Candidate(const boost::asio::ip::udp::endpoint& endpoint) {
    P2PCandidateEndpoint result;
    if (!endpoint.address().is_v4()) return result;
    result.address_family = 4; result.port = endpoint.port();
    result.address[10] = result.address[11] = 0xff;
    const auto bytes = endpoint.address().to_v4().to_bytes();
    std::copy(bytes.begin(), bytes.end(), result.address.begin() + 12);
    return result;
}
boost::asio::ip::udp::endpoint Endpoint(const P2PCandidateEndpoint& candidate) {
    if (candidate.address_family != 4) return {};
    boost::asio::ip::address_v4::bytes_type bytes{};
    std::copy(candidate.address.begin() + 12, candidate.address.end(), bytes.begin());
    return {boost::asio::ip::address_v4(bytes), candidate.port};
}
bool Candidates(const ppp::vector<ppp::app::protocol::P2PEndpointCandidate>& values,
    std::vector<P2PCandidateEndpoint>& out) {
    if (values.empty() || values.size() > 2) return false;
    for (const auto& value : values) {
        auto ep = Ipep::ParseEndPoint(value.endpoint);
        if (!ep.address().is_v4() || ep.address().is_unspecified() || ep.address().is_multicast() || !ep.port()) return false;
        auto candidate = Candidate(ep);
        if (std::find(out.begin(), out.end(), candidate) != out.end()) return false;
        out.push_back(candidate);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.address < b.address || (a.address == b.address && a.port < b.port);
    });
    return true;
}
std::string Hex(const P2POfferHash& hash) {
    static const char digits[] = "0123456789abcdef";
    std::string value(64, '0');
    for (std::size_t i = 0; i < hash.size(); ++i) {
        value[i*2] = digits[hash[i] >> 4]; value[i*2+1] = digits[hash[i] & 15];
    }
    return value;
}
}

void VEthernetExchanger::HandleP2PV2RelayOffer(const ITransmissionPtr& tx,
    const ppp::app::protocol::P2PControlMessage& message) noexcept {
    if (!tx || !tx->GetContext() || !tx->GetStrand()) return;
    const auto generation = p2p_offer_generation_.load();
    uint64_t registration = 0;
    {
        std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
        registration = p2p_transport_registration_id_;
    }
    auto self = shared_from_this();
    Executors::Post(tx->GetContext(), tx->GetStrand(), [self, this, tx, message, generation, registration]() noexcept {
        const auto now = P2PRecoveryNow();
        {
            std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
            if (disposed_.load() || generation != p2p_offer_generation_.load() ||
                registration != p2p_transport_registration_id_ ||
                p2p_registered_transmission_.lock() != tx || !p2p_candidate_transport_ ||
                !p2p_candidate_transport_->IsReady() || message.virtual_ip != p2p_registered_virtual_ip_) return;
            if (message.action == "status") {
                if (message.enabled && message.reason == "key-active-current" &&
                    message.current_offer_hash == Hex(p2p_v2_channel_.Snapshot().current_offer_hash).c_str())
                    p2p_reported_key_hash_ = p2p_v2_channel_.Snapshot().current_offer_hash;
                return;
            }
            if (!P2PRecoveryAllowed() || !message.enabled || message.action != "offer-v2" ||
                !message.peer_virtual_ip || message.peer_virtual_ip == message.virtual_ip ||
                std::find(message.supported_versions.begin(), message.supported_versions.end(), 2) == message.supported_versions.end()) return;
            if (p2p_peer_virtual_ip_ && p2p_peer_virtual_ip_ != message.peer_virtual_ip) return;
            auto frozen = p2p_candidate_history_.find(message.candidate_revision);
            if (frozen == p2p_candidate_history_.end()) return;
            P2PV2RecipientContext recipient;
            if (!Candidates(message.local_candidates, recipient.local_candidates) ||
                !Candidates(message.candidates, recipient.peer_candidates)) return;
            std::vector<P2PCandidateEndpoint> registered;
            if (!Candidates(frozen->second, registered) || registered != recipient.local_candidates) return;
            Int128ToBytes(GetId(), recipient.local_session_id.data());
            std::memcpy(recipient.local_peer_id.data() + 12, &message.virtual_ip, 4);
            recipient.local_candidate_revision = message.candidate_revision;
            recipient.peer_candidate_revision = message.peer_candidate_revision;
            const auto before = p2p_v2_channel_.Snapshot();
            const auto exporter = [tx](const char* label, const uint8_t* context, std::size_t n, uint8_t* out, std::size_t size) {
                return tx->ExportAuthenticatedSessionKey(label, context, n, out, size);
            };
            if (!p2p_v2_channel_.AcceptOffer(std::string(message.authenticated_offer_v2.data(), message.authenticated_offer_v2.size()),
                recipient, exporter, now, generation)) return;
            const auto after = p2p_v2_channel_.Snapshot();
            p2p_v2_selected_ = true; p2p_peer_virtual_ip_ = message.peer_virtual_ip;
            auto config = GetConfiguration();
            if (config) p2p_v2_channel_.ConfigureLiveness(config->p2p.heartbeat_interval_ms,
                config->p2p.heartbeat_miss_max, config->p2p.suspect_timeout_ms, config->p2p.migration_grace_ms);
            if (after.has_pending && (!before.has_pending || before.pending_offer_hash != after.pending_offer_hash)) {
                p2p_v2_probes_.Begin(after.local_role == P2PPeerRole::Initiator ? P2PProbeRole::Controlling : P2PProbeRole::Controlled,
                    recipient.local_candidates, recipient.peer_candidates, now, generation);
            }
            p2p_state_.store(after.state);
        }
        TickP2PV2(tx, now, generation);
    });
}

bool VEthernetExchanger::SendP2PV2Packets(const ITransmissionPtr& tx,
    const std::vector<P2PV2Outbound>& packets, uint64_t generation) noexcept {
    std::shared_ptr<IP2PDatagramTransport> transport;
    uint64_t registration = 0;
    {
        std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
        if (disposed_.load() || generation != p2p_offer_generation_.load() || p2p_registered_transmission_.lock() != tx) return false;
        transport = p2p_candidate_transport_; registration = p2p_transport_registration_id_;
    }
    if (!transport) return false;
    P2PId session{}; Int128ToBytes(GetId(), session.data());
    for (const auto& packet : packets) {
        P2PV2DataPacketHeader header;
        if (!ParseP2PV2DataPacketHeader(packet.datagram, header) &&
            !p2p_egress_limiter_.AllowSession(session, Executors::GetTickCount())) continue;
        if (!transport->SendTo(packet.datagram.data(), static_cast<int>(packet.datagram.size()), Endpoint(packet.destination))) {
            RecoverP2PTransport(tx, generation, registration); return false;
        }
    }
    return true;
}

void VEthernetExchanger::SendP2PV2Control(const ITransmissionPtr& tx, const char* action,
    const P2POfferHash& current, const P2POfferHash& cancel) noexcept {
#if defined(OPENPPP2_P2P_RECOVERY_TESTING)
    if (recovery_test_hooks_.control) { recovery_test_hooks_.control(action); return; }
#endif
    if (!tx || !tx->GetContext() || !tx->GetStrand()) return;
    InformationEnvelope envelope;
    envelope.Base.Clear();
    auto& message = envelope.Extensions.P2P;
    uint64_t generation = 0, registration = 0;
    {
        std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
        if (disposed_.load() || p2p_registered_transmission_.lock() != tx || !p2p_v2_selected_) return;
        generation = p2p_offer_generation_.load(); registration = p2p_transport_registration_id_;
        message.enabled = true; message.mode = "direct-preferred"; message.action = action;
        message.virtual_ip = p2p_registered_virtual_ip_; message.peer_virtual_ip = p2p_peer_virtual_ip_;
        message.supported_versions = {1, 2}; message.current_offer_hash = Hex(current).c_str();
        if (cancel != P2POfferHash{}) message.cancel_offer_hash = Hex(cancel).c_str();
    }
    envelope.ExtendedJson = envelope.Extensions.ToJson();
    auto self = shared_from_this();
    ppp::coroutines::YieldContext::Spawn(tx->BufferAllocator.get(), *tx->GetContext(), tx->GetStrand().get(),
        [self, this, tx, envelope, generation, registration](ppp::coroutines::YieldContext& y) noexcept {
            {
                std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
                if (disposed_.load() || generation != p2p_offer_generation_.load() ||
                    registration != p2p_transport_registration_id_ || p2p_registered_transmission_.lock() != tx) return;
            }
            DoInformation(tx, envelope, y);
        });
}

void VEthernetExchanger::HandleP2PV2Datagram(const ITransmissionPtr& tx, uint64_t generation,
    uint64_t registration, const boost::asio::ip::udp::endpoint& sender,
    const std::vector<uint8_t>& datagram) noexcept {
    P2PV2ControlResult result;
    std::vector<uint8_t> plaintext;
    uint32_t local_ip = 0, peer_ip = 0;
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
        if (disposed_.load() || !p2p_v2_selected_ || generation != p2p_offer_generation_.load() ||
            registration != p2p_transport_registration_id_ || p2p_registered_transmission_.lock() != tx) return;
        const auto source = Candidate(sender);
        const auto now = P2PRecoveryNow();
        P2PV2DataPacketHeader header;
        if (ParseP2PV2DataPacketHeader(datagram, header)) {
            accepted = p2p_v2_channel_.OpenData(datagram, source, now, generation, plaintext);
            if (!accepted) p2p_v2_channel_.HandleNewEndpointData(datagram, source, now, generation, result);
        } else {
            if (!p2p_v2_channel_.HandleControl(datagram, source, now, generation, result)) return;
            P2PProbeCandidatePair pair{result.local_candidate, result.peer_candidate};
            if (result.event == P2PV2ControlEvent::ProbeAck) {
                const auto index = p2p_v2_probes_.FindPair(pair);
                if (index) p2p_v2_probes_.OnAuthenticatedAck(*index, now, generation);
            } else if (result.event == P2PV2ControlEvent::ReverseProbeNeeded) {
                p2p_v2_probes_.NominateResponderPair(pair, now, generation);
            }
        }
        auto snapshot = p2p_v2_channel_.Snapshot();
        if (snapshot.has_current) {
            p2p_local_candidate_ = Endpoint(snapshot.local_candidate);
            p2p_peer_candidate_ = Endpoint(snapshot.peer_candidate);
        }
        p2p_state_.store(snapshot.state);
        local_ip = p2p_registered_virtual_ip_; peer_ip = p2p_peer_virtual_ip_;
    }
    SendP2PV2Packets(tx, result.outbound, generation);
    if (accepted && !plaintext.empty() && P2PDirectDataPath::AllowsInboundPacket(plaintext.data(),
        static_cast<int>(plaintext.size()), local_ip, peer_ip))
        OnNat(tx, plaintext.data(), static_cast<int>(plaintext.size()), ppp::nullof<ppp::coroutines::YieldContext>());
    TickP2PV2(tx, P2PRecoveryNow(), generation);
}

void VEthernetExchanger::TickP2PV2(const ITransmissionPtr& tx, uint64_t now, uint64_t generation) noexcept {
    if (!tx) return;
    P2PV2TickResult tick;
    P2POfferHash report{};
    bool report_key = false, retry = false;
    uint64_t registration = 0;
    {
        std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
        if (disposed_.load() || generation != p2p_offer_generation_.load() || !p2p_v2_selected_ ||
            p2p_registered_transmission_.lock() != tx) return;
        registration = p2p_transport_registration_id_;
        const auto before = p2p_v2_channel_.Snapshot();
        tick = p2p_v2_channel_.Tick(now, generation);
        if (tick.fallback && before.has_pending && tick.cancelled_offer_hash == P2POfferHash{})
            tick.cancelled_offer_hash = before.pending_offer_hash;
        auto batch = p2p_v2_probes_.Poll(now, generation);
        for (std::size_t i = 0; i < batch.size; ++i) {
            P2PV2Outbound outbound; outbound.destination = batch.tasks[i].pair.peer;
            if (p2p_v2_channel_.CreateProbe(batch.tasks[i].pair.local, batch.tasks[i].pair.peer, now, generation, outbound.datagram))
                tick.outbound.push_back(std::move(outbound));
        }
        auto snapshot = p2p_v2_channel_.Snapshot();
        p2p_state_.store(snapshot.state);
        if (snapshot.has_current) {
            p2p_local_candidate_ = Endpoint(snapshot.local_candidate); p2p_peer_candidate_ = Endpoint(snapshot.peer_candidate);
            if (snapshot.current_offer_hash != p2p_reported_key_hash_ &&
                (!p2p_last_key_report_ms_ || now - p2p_last_key_report_ms_ >= 1000)) {
                report_key = true; report = snapshot.current_offer_hash; p2p_last_key_report_ms_ = now;
            }
        } else if (!snapshot.has_pending && (!p2p_last_renew_ms_ || now - p2p_last_renew_ms_ >= 10000)) {
            tick.renew = true; p2p_last_renew_ms_ = now;
        }
        retry = (!p2p_candidate_transport_ || !p2p_candidate_transport_->IsReady()) && now >= p2p_retry_at_ms_;
        if (!snapshot.has_pending) p2p_v2_probes_.Cancel(generation);
    }
    if (retry) { RecoverP2PTransport(tx, generation, registration); return; }
    SendP2PV2Packets(tx, tick.outbound, generation);
    if (report_key) SendP2PV2Control(tx, "key-active", report);
    if (tick.renew || tick.cancelled_offer_hash != P2POfferHash{})
        SendP2PV2Control(tx, "renew", tick.current_offer_hash, tick.cancelled_offer_hash);
}

uint64_t VEthernetExchanger::P2PRecoveryNow() noexcept {
#if defined(OPENPPP2_P2P_RECOVERY_TESTING)
    if (recovery_test_hooks_.now) return recovery_test_hooks_.now();
#endif
    return Executors::GetTickCount();
}
bool VEthernetExchanger::P2PRecoveryAllowed() noexcept {
#if defined(OPENPPP2_P2P_RECOVERY_TESTING)
    if (recovery_test_hooks_.enabled) return true;
#endif
    return ProductionAuthenticatedControlV1Ready;
}
bool VEthernetExchanger::PostP2PRecovery(const ITransmissionPtr& tx, ppp::function<void()> callback) noexcept {
#if defined(OPENPPP2_P2P_RECOVERY_TESTING)
    if (recovery_test_hooks_.post) return recovery_test_hooks_.post(std::move(callback));
#endif
    return Executors::Post(tx->GetContext(), tx->GetStrand(), std::move(callback));
}
bool VEthernetExchanger::SpawnP2PRecovery(const ITransmissionPtr& tx, ppp::coroutines::YieldContext::SpawnHander callback) noexcept {
#if defined(OPENPPP2_P2P_RECOVERY_TESTING)
    if (recovery_test_hooks_.spawn) return recovery_test_hooks_.spawn(std::move(callback));
#endif
    return ppp::coroutines::YieldContext::Spawn(tx->BufferAllocator.get(), *tx->GetContext(), tx->GetStrand().get(), std::move(callback));
}

void VEthernetExchanger::RecoverP2PTransport(const ITransmissionPtr& tx, uint64_t generation, uint64_t registration) noexcept {
    if (!tx || !tx->GetContext() || !tx->GetStrand()) return;
    std::shared_ptr<IP2PDatagramTransport> failed;
    uint32_t local_ip = 0, peer_ip = 0;
    bool selected = false;
    uint64_t attempt = 0;
    {
        std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
        if (disposed_.load() || generation != p2p_offer_generation_.load() ||
            p2p_registered_transmission_.lock() != tx || registration != p2p_transport_registration_id_) return;
        local_ip = p2p_registered_virtual_ip_; peer_ip = p2p_peer_virtual_ip_;
        selected = p2p_v2_selected_;
        failed = std::move(p2p_candidate_transport_);
        if (failed) {
            ++p2p_recovery_attempt_;
            // Invalidate callbacks before Close; rebuilding is deferred to the owner strand.
            p2p_transport_registration_id_ = 0;
            if (p2p_stun_gatherer_) p2p_stun_gatherer_->Cancel();
            p2p_stun_gatherer_.reset();
            p2p_v2_channel_.Reset(generation);
            p2p_v2_probes_.Cancel(generation);
            p2p_offer_session_.ResetGeneration(generation);
            p2p_direct_data_path_.Reset(generation);
            p2p_candidate_history_.clear(); p2p_registered_candidates_.clear();
            p2p_local_candidate_ = {}; p2p_peer_candidate_ = {};
            p2p_reported_key_hash_ = {};
            p2p_last_key_report_ms_ = p2p_last_renew_ms_ = 0;
            p2p_last_heartbeat_tx_ms_ = p2p_suspect_since_ms_ = p2p_migrate_started_ms_ = 0;
            p2p_heartbeat_misses_ = 0;
            static constexpr uint64_t delays[] = {1000, 2000, 4000, 8000, 10000};
            p2p_retry_at_ms_ = P2PRecoveryNow() + delays[std::min(p2p_retry_step_, 4u)];
            p2p_retry_step_ = std::min(p2p_retry_step_ + 1, 5u);
            p2p_recovery_running_ = false;
            p2p_state_.store(P2PState::Relay);
        } else {
            if (p2p_recovery_running_ || P2PRecoveryNow() < p2p_retry_at_ms_ ||
                !P2PRecoveryAllowed()) return;
            p2p_recovery_running_ = true;
            attempt = ++p2p_recovery_attempt_;
        }
    }
    if (failed) { failed->Close(); return; }
    auto self = shared_from_this();
    const bool scheduled = PostP2PRecovery(tx, [self, this, tx, generation, registration, attempt, local_ip, peer_ip, selected]() noexcept {
        {
            std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
            if (disposed_.load() || generation != p2p_offer_generation_.load() ||
                p2p_registered_transmission_.lock() != tx || p2p_candidate_transport_ ||
                p2p_transport_registration_id_ != registration || !p2p_recovery_running_ ||
                attempt != p2p_recovery_attempt_) return;
        }
        const bool spawned = SpawnP2PRecovery(tx,
            [self, this, tx, generation, registration, attempt, local_ip, peer_ip, selected](ppp::coroutines::YieldContext& y) noexcept {
                {
                    std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
                    if (disposed_.load() || generation != p2p_offer_generation_.load() ||
                        p2p_registered_transmission_.lock() != tx ||
                        registration != p2p_transport_registration_id_ ||
                        attempt != p2p_recovery_attempt_ || !p2p_recovery_running_) return;
                }
                const bool registered = SendRequestedIPv6Configuration(tx, y, true);
                if (!registered) {
                    uint64_t failed_registration = 0;
                    {
                        std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
                        if (disposed_.load() || generation != p2p_offer_generation_.load() || GetTransmission() != tx ||
                            attempt != p2p_recovery_attempt_) return;
                        failed_registration = p2p_transport_registration_id_;
                    }
                    if (failed_registration) {
                        RecoverP2PTransport(tx, generation, failed_registration);
                        return;
                    }
                }
                bool ready = false;
                {
                    std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
                    if (disposed_.load() || generation != p2p_offer_generation_.load() || GetTransmission() != tx ||
                        attempt != p2p_recovery_attempt_) return;
                    p2p_v2_selected_ = selected; p2p_peer_virtual_ip_ = peer_ip;
                    p2p_registered_transmission_ = tx;
                    if (!p2p_registered_virtual_ip_) p2p_registered_virtual_ip_ = local_ip;
                    p2p_recovery_running_ = false;
                    if (p2p_candidate_transport_ && p2p_candidate_transport_->IsReady()) {
                        p2p_retry_step_ = 0; p2p_retry_at_ms_ = 0;
                        ready = true;
                    } else {
                        static constexpr uint64_t delays[] = {1000, 2000, 4000, 8000, 10000};
                        p2p_retry_at_ms_ = P2PRecoveryNow() + delays[std::min(p2p_retry_step_, 4u)];
                        p2p_retry_step_ = std::min(p2p_retry_step_ + 1, 5u);
                    }
                    p2p_last_renew_ms_ = 0;
                }
                if (ready) SendP2PV2Control(tx, "renew", {});
            });
        if (!spawned) {
            std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
            if (!disposed_.load() && generation == p2p_offer_generation_.load() &&
                p2p_transport_registration_id_ == registration &&
                p2p_recovery_attempt_ == attempt &&
                p2p_registered_transmission_.lock() == tx && !p2p_candidate_transport_) {
                p2p_recovery_running_ = false;
                p2p_retry_at_ms_ = P2PRecoveryNow() + 1000;
            }
        }
    });
    if (!scheduled) {
        std::lock_guard<std::mutex> lock(p2p_offer_mutex_);
        if (!disposed_.load() && generation == p2p_offer_generation_.load() &&
            p2p_transport_registration_id_ == registration &&
            p2p_recovery_attempt_ == attempt &&
            p2p_registered_transmission_.lock() == tx && !p2p_candidate_transport_) {
            p2p_recovery_running_ = false;
            p2p_retry_at_ms_ = P2PRecoveryNow() + 1000;
        }
    }
}
}
