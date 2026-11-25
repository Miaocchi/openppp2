#define BOOST_TEST_MODULE multipath_transmission_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/configurations/AppConfiguration.h>
#include <ppp/coroutines/YieldContext.h>
#include <ppp/transmissions/ITcpipTransmission.h>
#include <ppp/transmissions/ITransmissionStatistics.h>
#include <ppp/transmissions/IWebsocketTransmission.h>
#include <ppp/transmissions/MultipathTransmission.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/post.hpp>

#if defined(__linux__)
#include <sys/socket.h>
#endif

using ppp::transmissions::MultipathTransmission;

namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

bool WaitFor(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

ppp::cryptography::noise::Secret32 TestSecret(std::uint8_t value) {
    ppp::cryptography::noise::Bytes32 bytes{};
    bytes.fill(value);
    return ppp::cryptography::noise::Secret32(std::move(bytes));
}

ppp::cryptography::noise::MultipathHandshakeParameters TestParameters() {
    ppp::cryptography::noise::MultipathHandshakeParameters parameters;
    parameters.client_identity = "test-client";
    parameters.server_identity = "test-server";
    parameters.key_id = "test-key";
    parameters.capabilities = { 1, 0, 1 };
    return parameters;
}

// Upgraded clients advertise binary framing by carrying the distinct carrier ID
// prefix in the otherwise opaque admission request.  The prefix is only a
// capability marker: the admission payload stays a single opaque identifier for
// proof and lookup purposes.
std::vector<std::uint8_t> AdmissionRequest(bool supports_binary_framing) {
    const std::string value = supports_binary_framing
        ? std::string(MultipathTransmission::BinaryFramingCarrierIdPrefix) + "create"
        : std::string("create");
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

bool IsAdmissionRequest(const std::vector<std::uint8_t>& control) {
    static constexpr char suffix[] = "create";
    const std::string value(control.begin(), control.end());
    return value.size() >= sizeof(suffix) - 1 &&
        value.compare(value.size() - (sizeof(suffix) - 1), sizeof(suffix) - 1, suffix) == 0;
}

bool AdvertisesBinaryFraming(const std::vector<std::uint8_t>& control) {
    const std::string value(control.begin(), control.end());
    const std::string prefix(MultipathTransmission::BinaryFramingCarrierIdPrefix);
    return value.size() > prefix.size() && value.compare(0, prefix.size(), prefix) == 0 &&
        IsAdmissionRequest(control);
}


class EphemeralCa final {
public:
    EphemeralCa() {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        directory_ = std::filesystem::temp_directory_path() / ("openppp2-multipath-" + suffix);
        std::filesystem::create_directory(directory_);
        std::filesystem::permissions(directory_, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
        ca_file_ = (directory_ / "ca.pem").string();
        certificate_file_ = (directory_ / "server.pem").string();
        key_file_ = (directory_ / "server.key").string();
        if (const char* previous = std::getenv("SSL_CERT_FILE")) {
            previous_ssl_cert_file_ = previous;
            had_previous_ssl_cert_file_ = true;
        }
        EVP_PKEY* ca_key = GenerateKey();
        EVP_PKEY* server_key = GenerateKey();
        X509* ca = MakeCertificate(ca_key, nullptr, nullptr, true);
        X509* server = MakeCertificate(server_key, ca, ca_key, false);
        if (!ca_key || !server_key || !ca || !server ||
            !WriteCertificate(ca_file_, ca) || !WriteCertificate(certificate_file_, server) ||
            !WritePrivateKey(key_file_, server_key)) {
            EVP_PKEY_free(ca_key);
            EVP_PKEY_free(server_key);
            X509_free(ca);
            X509_free(server);
            std::filesystem::remove_all(directory_);
            throw std::runtime_error("test certificate generation failed");
        }
        EVP_PKEY_free(ca_key);
        EVP_PKEY_free(server_key);
        X509_free(ca);
        X509_free(server);
        std::filesystem::permissions(key_file_, std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write, std::filesystem::perm_options::replace);
#if defined(_WIN32)
        _putenv_s("SSL_CERT_FILE", ca_file_.c_str());
#else
        setenv("SSL_CERT_FILE", ca_file_.c_str(), 1);
#endif
    }

    ~EphemeralCa() {
#if defined(_WIN32)
        _putenv_s("SSL_CERT_FILE", had_previous_ssl_cert_file_ ? previous_ssl_cert_file_.c_str() : "");
#else
        if (had_previous_ssl_cert_file_) setenv("SSL_CERT_FILE", previous_ssl_cert_file_.c_str(), 1);
        else unsetenv("SSL_CERT_FILE");
#endif
        std::filesystem::remove_all(directory_);
    }

    const std::string& CaFile() const noexcept { return ca_file_; }
    const std::string& CertificateFile() const noexcept { return certificate_file_; }
    const std::string& KeyFile() const noexcept { return key_file_; }

private:
    static EVP_PKEY* GenerateKey() {
        EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        EVP_PKEY* key = nullptr;
        const bool ok = context && EVP_PKEY_keygen_init(context) > 0 &&
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context, NID_X9_62_prime256v1) > 0 &&
            EVP_PKEY_keygen(context, &key) > 0;
        EVP_PKEY_CTX_free(context);
        if (!ok) { EVP_PKEY_free(key); return nullptr; }
        return key;
    }

    static bool AddExtension(X509* certificate, X509* issuer, int nid, const char* value) {
        X509V3_CTX context;
        X509V3_set_ctx(&context, issuer ? issuer : certificate, certificate, nullptr, nullptr, 0);
        X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, const_cast<char*>(value));
        if (!extension) return false;
        const bool ok = X509_add_ext(certificate, extension, -1) == 1;
        X509_EXTENSION_free(extension);
        return ok;
    }

    static X509* MakeCertificate(EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key, bool is_ca) {
        if (!key) return nullptr;
        X509* certificate = X509_new();
        if (!certificate) return nullptr;
        X509_set_version(certificate, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), is_ca ? 1 : 2);
        X509_gmtime_adj(X509_getm_notBefore(certificate), -60);
        X509_gmtime_adj(X509_getm_notAfter(certificate), 3600);
        X509_set_pubkey(certificate, key);
        X509_NAME* subject = X509_get_subject_name(certificate);
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>(is_ca ? "OpenPPP2 Test CA" : "localhost"), -1, -1, 0);
        X509_set_issuer_name(certificate, issuer ? X509_get_subject_name(issuer) : subject);
        bool ok = AddExtension(certificate, issuer, NID_basic_constraints, is_ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
        if (is_ca) ok = ok && AddExtension(certificate, issuer, NID_key_usage, "critical,keyCertSign,cRLSign");
        else ok = ok && AddExtension(certificate, issuer, NID_key_usage, "critical,digitalSignature,keyEncipherment") &&
            AddExtension(certificate, issuer, NID_ext_key_usage, "serverAuth") &&
            AddExtension(certificate, issuer, NID_subject_alt_name, "DNS:localhost");
        if (!ok || X509_sign(certificate, issuer_key ? issuer_key : key, EVP_sha256()) <= 0) {
            X509_free(certificate);
            return nullptr;
        }
        return certificate;
    }

    static bool WriteCertificate(const std::string& path, X509* certificate) {
        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file) return false;
        bool ok = PEM_write_X509(file, certificate) == 1;
        std::fclose(file);
        return ok;
    }

    static bool WritePrivateKey(const std::string& path, EVP_PKEY* key) {
        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file) return false;
        bool ok = PEM_write_PrivateKey(file, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
        std::fclose(file);
        return ok;
    }

    std::filesystem::path directory_;
    std::string ca_file_, certificate_file_, key_file_, previous_ssl_cert_file_;
    bool had_previous_ssl_cert_file_ = false;
};

class TcpMultipathPair final {
public:
    explicit TcpMultipathPair(bool websocket = false, const EphemeralCa* certificate = nullptr,
        std::string tls_server_name = "localhost", std::uint8_t client_psk = 7,
        std::uint8_t server_psk = 7, bool corrupt_noise = false,
        bool client_supports_binary_framing = true, bool server_supports_binary_framing = true,
        bool plaintext = false)
        : context_(std::make_shared<asio::io_context>())
        , guard_(asio::make_work_guard(*context_))
        , websocket_(websocket)
        , wss_(certificate != nullptr)
        , client_psk_(client_psk)
        , server_psk_(server_psk)
        , corrupt_noise_(corrupt_noise)
        , client_supports_binary_framing_(client_supports_binary_framing)
        , server_supports_binary_framing_(server_supports_binary_framing)
        , plaintext_(plaintext) {
        tcp::acceptor acceptor(*context_, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        client_socket_ = std::make_shared<tcp::socket>(*context_);
        server_socket_ = std::make_shared<tcp::socket>(*context_);
        client_socket_->connect(acceptor.local_endpoint());
        acceptor.accept(*server_socket_);
        client_strand_ = std::make_shared<ppp::transmissions::ITransmission::StrandPtr::element_type>(asio::make_strand(*context_));
        server_strand_ = std::make_shared<ppp::transmissions::ITransmission::StrandPtr::element_type>(asio::make_strand(*context_));
        auto client_config = std::make_shared<ppp::configurations::AppConfiguration>();
        auto server_config = std::make_shared<ppp::configurations::AppConfiguration>();
        client_config->key.kh = client_config->key.kl = 0;
        server_config->key.kh = server_config->key.kl = 0;
        client_config->key.plaintext = server_config->key.plaintext = plaintext_;
        client_config->tcp.connect.timeout = server_config->tcp.connect.timeout = 5;
        if (wss_) {
            client_config->websocket.host = server_config->websocket.host = "localhost";
            client_config->websocket.path = server_config->websocket.path = "/multipath";
            server_config->websocket.ssl.certificate_file = certificate->CertificateFile();
            server_config->websocket.ssl.certificate_key_file = certificate->KeyFile();
            server_config->websocket.ssl.certificate_chain_file = certificate->CertificateFile();
            client_ssl_ = std::make_shared<ppp::transmissions::ISslWebsocketTransmission>(context_, client_strand_,
                client_socket_, client_config);
            server_ssl_ = std::make_shared<ppp::transmissions::ISslWebsocketTransmission>(context_, server_strand_,
                server_socket_, server_config);
            client_ssl_->TlsServerName = std::move(tls_server_name);
            client_ssl_->HttpHost = "edge.example.test";
            client_ssl_->Path = "/multipath";
            client_ssl_->ForceVerifyPeer = true;
            server_ssl_->Path = "/multipath";
            server_ssl_->HttpHost = "localhost";
            client_ = client_ssl_;
            server_ = server_ssl_;
        } else if (websocket_) {
            client_ws_ = std::make_shared<ppp::transmissions::IWebsocketTransmission>(context_, client_strand_, client_socket_,
                client_config);
            server_ws_ = std::make_shared<ppp::transmissions::IWebsocketTransmission>(context_, server_strand_, server_socket_,
                server_config);
            client_ws_->Host = server_ws_->Host = "localhost";
            client_ws_->Path = server_ws_->Path = "/multipath";
            client_ = client_ws_;
            server_ = server_ws_;
        } else {
            client_ = std::make_shared<ppp::transmissions::ITcpipTransmission>(context_, client_strand_, client_socket_,
                client_config, ppp::transmissions::TcpTransmissionRole::Main);
            server_ = std::make_shared<ppp::transmissions::ITcpipTransmission>(context_, server_strand_, server_socket_,
                server_config, ppp::transmissions::TcpTransmissionRole::Server);
        }
        client_statistics_ = std::make_shared<ppp::transmissions::ITransmissionStatistics>();
        server_statistics_ = std::make_shared<ppp::transmissions::ITransmissionStatistics>();
        client_->Statistics = client_statistics_;
        server_->Statistics = server_statistics_;
    }

    ~TcpMultipathPair() { Dispose(); }

    bool Run(std::chrono::milliseconds response_delay = {}) {
        for (int i = 0; i < 4; ++i) threads_.emplace_back([context = context_]() { context->run(); });
        if (websocket_ && !UpgradeWebsocket()) return false;
        if (!StartNoise(response_delay)) return false;
        if (!Admit()) return false;
        return CompleteApplicationHandshake();
    }

    bool StartNoise(std::chrono::milliseconds response_delay = {}) {
        pending_.store(2);
        const bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this, response_delay](ppp::coroutines::YieldContext& y) noexcept {
                int frame_length = 0;
                auto first_frame = server_->Read(y, frame_length);
                if (first_frame) {
                    if (response_delay.count() > 0) std::this_thread::sleep_for(response_delay);
                    if (corrupt_noise_ && frame_length > 0) first_frame.get()[frame_length - 1] ^= 1;
                    server_secure_ = std::make_shared<MultipathTransmission>(server_, TestParameters(), TestSecret(server_psk_),
                        ppp::cryptography::noise::HandshakeRole::NetworkServerResponder, first_frame, frame_length);
                    server_secure_ok_.store(server_secure_->StartSecureServer(y));
                    if (!server_secure_ok_.load()) server_->Dispose();
                }
                pending_.fetch_sub(1);
            });
        const bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                client_secure_ = std::make_shared<MultipathTransmission>(client_, TestParameters(), TestSecret(client_psk_),
                    ppp::cryptography::noise::HandshakeRole::NetworkClientInitiator);
                client_secure_ok_.store(client_secure_->StartSecureClient(y));
                pending_.fetch_sub(1);
            });
        return server_spawned && client_spawned && WaitFor([this]() { return pending_.load() == 0; }) &&
            server_secure_ok_.load() && client_secure_ok_.load();
    }

    // Authenticated post-Noise admission: a legacy client sends its ordinary
    // create request and reads the ordinary ready response, while an upgraded
    // client advertises binary framing and consumes the responder's offer.
    bool Admit() {
        pending_.store(2);
        const bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                std::uint8_t type = 0;
                std::vector<std::uint8_t> control;
                const bool read = server_secure_ && server_secure_->ReadSecureRecord(y, type, control) &&
                    type == MultipathTransmission::ControlRecord && IsAdmissionRequest(control);
                const bool advertised = read && AdvertisesBinaryFraming(control);
                const bool negotiate = !server_supports_binary_framing_ || !advertised ||
                    server_secure_->NegotiateBinaryFramingServer(y);
                server_control_ok_.store(read && negotiate &&
                    server_secure_->WriteSecureRecord(y, MultipathTransmission::ControlRecord, "ready", 5));
                pending_.fetch_sub(1);
            });
        const bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                const std::vector<std::uint8_t> request =
                    AdmissionRequest(client_supports_binary_framing_);
                const bool write = client_secure_ && client_secure_->WriteSecureRecord(y,
                    MultipathTransmission::ControlRecord, request.data(), request.size());
                std::uint8_t type = 0;
                std::vector<std::uint8_t> control;
                const bool read = client_secure_ && (client_supports_binary_framing_
                    ? client_secure_->ReadAdmissionResponse(y, type, control)
                    : client_secure_->ReadSecureRecord(y, type, control));
                client_control_ok_.store(write && read &&
                    type == MultipathTransmission::ControlRecord &&
                    control == std::vector<std::uint8_t>{'r','e','a','d','y'});
                pending_.fetch_sub(1);
            });
        return server_spawned && client_spawned && WaitFor([this]() { return pending_.load() == 0; }) &&
            server_control_ok_.load() && client_control_ok_.load();
    }

    bool CompleteApplicationHandshake() {
        pending_.store(2);
        const bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                bool mux = false;
                server_handshake_ok_.store(server_control_ok_.load() &&
                    server_secure_->HandshakeClient(y, mux) == static_cast<ppp::Int128>(0x1234) && !mux);
                server_application_ok_.store(server_handshake_ok_.load() && server_secure_->BeginApplicationIO());
                pending_.fetch_sub(1);
            });
        const bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                client_handshake_ok_.store(client_control_ok_.load() &&
                    client_secure_->HandshakeServer(y, static_cast<ppp::Int128>(0x1234), false));
                client_application_ok_.store(client_handshake_ok_.load() && client_secure_->BeginApplicationIO());
                pending_.fetch_sub(1);
            });
        return server_spawned && client_spawned && WaitFor([this]() { return pending_.load() == 0; }) &&
            server_handshake_ok_.load() && client_handshake_ok_.load() &&
            server_application_ok_.load() && client_application_ok_.load();
    }

    bool RoundTrip(const std::string& payload, bool reverse = false) {
        auto reader = reverse ? client_secure_ : server_secure_;
        auto writer = reverse ? server_secure_ : client_secure_;
        std::atomic<int> pending{2};
        std::atomic<bool> read_ok{false}, write_ok{false};
        const auto& read_strand = reverse ? client_strand_ : server_strand_;
        const auto& write_strand = reverse ? server_strand_ : client_strand_;
        bool read_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, read_strand.get(),
            [&, reader](ppp::coroutines::YieldContext& y) noexcept {
                int length = 0;
                auto bytes = reader->Read(y, length);
                read_ok.store(bytes && length == static_cast<int>(payload.size()) &&
                    std::memcmp(bytes.get(), payload.data(), payload.size()) == 0);
                pending.fetch_sub(1);
            });
        bool write_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, write_strand.get(),
            [&, writer](ppp::coroutines::YieldContext& y) noexcept {
                write_ok.store(writer->Write(y, payload.data(), static_cast<int>(payload.size())));
                pending.fetch_sub(1);
            });
        return read_spawned && write_spawned && WaitFor([&]() { return pending.load() == 0; }) &&
            read_ok.load() && write_ok.load();
    }

    bool CreateMuxPair() {
        if (!client_secure_ || !server_secure_) return false;
        client_mux_ = client_secure_->CreateMuxSubchannel();
        server_mux_ = server_secure_->CreateMuxSubchannel();
        return client_mux_ && server_mux_;
    }

    bool MuxRoundTrip(const std::string& payload, bool reverse = false) {
        if (!client_mux_ || !server_mux_) return false;
        auto reader = reverse ? client_mux_ : server_mux_;
        auto writer = reverse ? server_mux_ : client_mux_;
        std::atomic<int> pending{2};
        std::atomic<bool> read_ok{false}, write_ok{false};
        const auto& read_strand = reverse ? client_strand_ : server_strand_;
        const auto& write_strand = reverse ? server_strand_ : client_strand_;
        bool read_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, read_strand.get(),
            [&, reader](ppp::coroutines::YieldContext& y) noexcept {
                int length = 0;
                auto bytes = reader->Read(y, length);
                read_ok.store(bytes && length == static_cast<int>(payload.size()) &&
                    std::memcmp(bytes.get(), payload.data(), payload.size()) == 0);
                pending.fetch_sub(1);
            });
        bool write_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, write_strand.get(),
            [&, writer](ppp::coroutines::YieldContext& y) noexcept {
                write_ok.store(writer->Write(y, payload.data(), static_cast<int>(payload.size())));
                pending.fetch_sub(1);
            });
        return read_spawned && write_spawned && WaitFor([&]() { return pending.load() == 0; }) &&
            read_ok.load() && write_ok.load();
    }

    bool CheckFullMuxReceiveQueue(bool dispose_while_full) {
        // Each small frame seals into one MuxRecord. With no logical reader,
        // the last of these records crosses the 128-record receive bound.
        constexpr int frames = 129;
        constexpr int frame_size = 256;
        client_statistics_->Clear();
        server_statistics_->Clear();
        std::atomic<bool> sent{false}, send_done{false};
        if (!ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this, &sent, &send_done](ppp::coroutines::YieldContext& y) noexcept {
                std::array<char, frame_size> payload;
                int i = 0;
                for (; i < frames; ++i) {
                    payload.fill(static_cast<char>(i));
                    if (!client_mux_->Write(y, payload.data(), frame_size)) break;
                }
                sent.store(i == frames);
                send_done.store(true);
            }) || !WaitFor([&]() { return send_done.load(); })) {
            Dispose();
            return false;
        }
        if (!sent || !WaitFor([&]() {
            return server_statistics_->IncomingTraffic.load() >= client_statistics_->OutgoingTraffic.load();
        })) return false;
        if (!server_secure_->IsSecureReady()) return false;
        if (dispose_while_full) {
            server_secure_->Dispose();
            return WaitFor([this]() { return server_secure_->IsApplicationPumpStopped(); });
        }

        std::atomic<bool> received{false}, read_done{false};
        if (!ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this, &received, &read_done](ppp::coroutines::YieldContext& y) noexcept {
                std::array<char, frame_size> expected;
                int i = 0;
                for (; i < frames; ++i) {
                    expected.fill(static_cast<char>(i));
                    int length = 0;
                    const auto payload = server_mux_->Read(y, length);
                    if (!payload || length != frame_size ||
                        std::memcmp(payload.get(), expected.data(), frame_size) != 0) break;
                }
                received.store(i == frames);
                read_done.store(true);
            }) || !WaitFor([&]() { return read_done.load(); })) {
            Dispose();
            return false;
        }
        return received && server_secure_->IsSecureReady();
    }

    bool LogicalSubchannelsRoundTrip(bool handshake_subchannel = true) {
        client_mux_ = client_secure_->CreateMuxSubchannel();
        server_mux_ = server_secure_->CreateMuxSubchannel();
        auto client_mux = client_mux_;
        auto server_mux = server_mux_;
        if (!client_mux || !server_mux || client_mux == client_secure_ || server_mux == server_secure_ ||
            client_secure_->CreateMuxSubchannel() != client_mux ||
            server_secure_->CreateMuxSubchannel() != server_mux) return false;

        std::atomic<int> pending{2};
        if (handshake_subchannel) {
            std::atomic<bool> client_handshake{false}, server_handshake{false};
            const bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
                [this, server_mux, &pending, &server_handshake](ppp::coroutines::YieldContext& y) noexcept {
                    bool mux = false;
                    server_handshake.store(server_mux->HandshakeClient(y, mux) == static_cast<ppp::Int128>(0x1234) && mux);
                    pending.fetch_sub(1);
                });
            const bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
                [client_mux, &pending, &client_handshake](ppp::coroutines::YieldContext& y) noexcept {
                    client_handshake.store(client_mux->HandshakeServer(y, static_cast<ppp::Int128>(0x1234), true));
                    pending.fetch_sub(1);
                });
            if (!server_spawned || !client_spawned || !WaitFor([&]() { return pending.load() == 0; }) ||
                !client_handshake.load() || !server_handshake.load()) {
                Dispose();
                return false;
            }
        }

        std::array<std::string, 4> payloads = {
            "main client to server", "main server to client",
            "mux client to server", "mux server to client"
        };
        if (!handshake_subchannel) {
            // Primary MUXON uses the logical stream directly. Bytewise Base94
            // expands this legal payload past the decoded 64 KiB ceiling.
            payloads[2].resize(60000);
            for (std::size_t n = 0; n < payloads[2].size(); ++n)
                payloads[2][n] = static_cast<char>(n & 255);
            payloads[3] = payloads[2];
        }
        std::array<std::atomic<bool>, 4> reads{}, writes{};
        std::array<int, 4> lengths{}, errors{};
        std::atomic<bool> control_seen{false};
        if (!server_secure_->SetControlRecordHandler([&control_seen](std::uint8_t type,
            const std::shared_ptr<const std::vector<std::uint8_t>>& record) noexcept {
            const std::vector<std::uint8_t> expected{ 'i', 'n', 't', 'e', 'r', 'l', 'e', 'a', 'v', 'e' };
            const bool ok = type == MultipathTransmission::ControlRecord && record && *record == expected;
            control_seen.store(ok);
            return ok;
        })) return false;

        pending.store(9);
        const auto spawn_read = [this, &pending, &reads, &lengths, &errors, &payloads](int index,
            const std::shared_ptr<ppp::transmissions::ITransmission>& stream,
            const ppp::transmissions::ITransmission::StrandPtr& strand) noexcept {
            return ppp::coroutines::YieldContext::Spawn(nullptr, *context_, strand.get(),
                [&, index, stream](ppp::coroutines::YieldContext& y) noexcept {
                    int length = 0;
                    auto bytes = stream->Read(y, length);
                    lengths[index] = length;
                    errors[index] = static_cast<int>(ppp::diagnostics::GetLastErrorCode());
                    reads[index].store(bytes && length == static_cast<int>(payloads[index].size()) &&
                        std::memcmp(bytes.get(), payloads[index].data(), payloads[index].size()) == 0);
                    pending.fetch_sub(1);
                });
        };
        const auto spawn_write = [this, &pending, &writes, &payloads](int index,
            const std::shared_ptr<ppp::transmissions::ITransmission>& stream,
            const ppp::transmissions::ITransmission::StrandPtr& strand) noexcept {
            return ppp::coroutines::YieldContext::Spawn(nullptr, *context_, strand.get(),
                [&, index, stream](ppp::coroutines::YieldContext& y) noexcept {
                    writes[index].store(stream->Write(y, payloads[index].data(),
                        static_cast<int>(payloads[index].size())));
                    pending.fetch_sub(1);
                });
        };
        const bool spawned =
            spawn_read(0, server_secure_, server_strand_) &&
            spawn_read(1, client_secure_, client_strand_) &&
            spawn_read(2, server_mux, server_strand_) &&
            spawn_read(3, client_mux, client_strand_) &&
            spawn_write(0, client_secure_, client_strand_) &&
            spawn_write(1, server_secure_, server_strand_) &&
            spawn_write(2, client_mux, client_strand_) &&
            spawn_write(3, server_mux, server_strand_) &&
            ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
                [this, &pending](ppp::coroutines::YieldContext& y) noexcept {
                    const std::uint8_t control[] = { 'i', 'n', 't', 'e', 'r', 'l', 'e', 'a', 'v', 'e' };
                    (void)client_secure_->WriteSecureRecord(y, MultipathTransmission::ControlRecord,
                        control, sizeof(control));
                    pending.fetch_sub(1);
                });
        const bool complete = spawned && WaitFor([&]() { return pending.load() == 0 && control_seen.load(); });
        if (!complete) Dispose();
        BOOST_CHECK_MESSAGE(complete, "pending=" << pending.load() << " control=" << control_seen.load());
        for (int i = 0; i < 4; ++i) {
            BOOST_CHECK_MESSAGE(reads[i].load(), "logical read " << i << " length=" << lengths[i] << " error=" << errors[i]);
            BOOST_CHECK_MESSAGE(writes[i].load(), "logical write " << i);
        }
        server_secure_->ClearControlRecordHandler();
        return complete && reads[0] && reads[1] && reads[2] && reads[3] &&
            writes[0] && writes[1] && writes[2] && writes[3] && control_seen;
    }

    bool MuxChildDisposeKeepsMainAlive() {
        auto client_mux = client_mux_;
        auto server_mux = server_mux_;
        if (!client_mux || !server_mux) return false;
        std::atomic<int> pending{1};
        std::atomic<bool> read_failed{false};
        const bool spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [server_mux, &pending, &read_failed](ppp::coroutines::YieldContext& y) noexcept {
                int length = 0;
                read_failed.store(!server_mux->Read(y, length));
                pending.fetch_sub(1);
            });
        if (!spawned) return false;
        std::this_thread::sleep_for(20ms);
        server_mux->Dispose();
        const bool woke = WaitFor([&]() { return pending.load() == 0; });
        const bool parent_ready = server_secure_->IsSecureReady();
        return woke && read_failed && parent_ready;
    }

    bool DisposeWakesBothLogicalReaders() {
        auto server_mux = server_secure_->CreateMuxSubchannel();
        if (!server_mux) return false;
        std::atomic<int> pending{2};
        std::atomic<int> failures{0};
        const bool main_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this, &pending, &failures](ppp::coroutines::YieldContext& y) noexcept {
                int length = 0;
                if (!server_secure_->Read(y, length)) failures.fetch_add(1);
                pending.fetch_sub(1);
            });
        const bool mux_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [server_mux, &pending, &failures](ppp::coroutines::YieldContext& y) noexcept {
                int length = 0;
                if (!server_mux->Read(y, length)) failures.fetch_add(1);
                pending.fetch_sub(1);
            });
        if (!main_spawned || !mux_spawned) return false;
        std::this_thread::sleep_for(20ms);
        client_secure_->Dispose();
        return WaitFor([&]() { return pending.load() == 0; }) && failures == 2;
    }


    bool ExportersMatch() {
        std::array<std::uint8_t, 32> client_export{}, server_export{}, changed{};
        const std::uint8_t context_a[] = { 1, 2, 3 };
        const std::uint8_t context_b[] = { 1, 2, 4 };
        return client_secure_->ExportAuthenticatedSessionKey("test-export", context_a, sizeof(context_a),
                client_export.data(), client_export.size()) &&
            server_secure_->ExportAuthenticatedSessionKey("test-export", context_a, sizeof(context_a),
                server_export.data(), server_export.size()) && client_export == server_export &&
            client_secure_->ExportAuthenticatedSessionKey("test-export", context_b, sizeof(context_b),
                changed.data(), changed.size()) && client_export != changed;
    }

    bool NoAuthenticatedPPPHandshake() const noexcept {
        return client_secure_ && server_secure_ &&
            !client_secure_->IsHandshakeComplete() && !server_secure_->IsHandshakeComplete();
    }

    bool UpgradeWebsocket() {
        std::atomic<int> pending{2};
        std::atomic<bool> client_ok{false}, server_ok{false};
        bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this, &pending, &client_ok](ppp::coroutines::YieldContext& y) noexcept {
                client_ok.store(wss_ ? client_ssl_->UpgradeOnly(true, y) : client_ws_->UpgradeOnly(true, y));
                pending.fetch_sub(1);
            });
        bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this, &pending, &server_ok](ppp::coroutines::YieldContext& y) noexcept {
                server_ok.store(wss_ ? server_ssl_->UpgradeOnly(false, y) : server_ws_->UpgradeOnly(false, y));
                pending.fetch_sub(1);
            });
        return client_spawned && server_spawned && WaitFor([&]() { return pending.load() == 0; }) &&
            client_ok.load() && server_ok.load();
    }

    bool ControlDemux(const std::string& payload) {
        std::atomic<bool> control_seen{false};
        std::atomic<int> pending{2};
        std::atomic<bool> read_ok{false}, write_ok{false};
        if (!server_secure_->SetControlRecordHandler([&control_seen](std::uint8_t type,
            const std::shared_ptr<const std::vector<std::uint8_t>>& record) noexcept {
            const std::vector<std::uint8_t> expected{ 'p', 'i', 'n', 'g' };
            bool ok = type == MultipathTransmission::ControlRecord && record && *record == expected;
            control_seen.store(ok);
            return ok;
        })) return false;
        bool read_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this, &pending, &read_ok, &payload](ppp::coroutines::YieldContext& y) noexcept {
                int length = 0;
                auto bytes = server_secure_->Read(y, length);
                read_ok.store(bytes && length == static_cast<int>(payload.size()) &&
                    std::memcmp(bytes.get(), payload.data(), payload.size()) == 0);
                pending.fetch_sub(1);
            });
        bool write_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this, &pending, &write_ok, &payload](ppp::coroutines::YieldContext& y) noexcept {
                const std::uint8_t ping[] = { 'p', 'i', 'n', 'g' };
                const bool control = client_secure_->WriteSecureRecord(y,
                    MultipathTransmission::ControlRecord, ping, sizeof(ping));
                write_ok.store(control && client_secure_->Write(y, payload.data(), static_cast<int>(payload.size())));
                pending.fetch_sub(1);
            });
        const bool complete = read_spawned && write_spawned && WaitFor([&]() { return pending.load() == 0; });
        server_secure_->ClearControlRecordHandler();
        return complete && control_seen.load() && read_ok.load() && write_ok.load();
    }

    bool DisposeDuringQueuedWrites() {
        std::atomic<int> completed{0};
        int accepted = 0;
        const std::string payload(50000, 'q');
        for (int i = 0; i < 96; ++i) {
            if (client_secure_->Write(payload.data(), static_cast<int>(payload.size()),
                [&completed](bool) noexcept { completed.fetch_add(1); })) {
                ++accepted;
            }
        }
        client_secure_->Dispose();
        return accepted > 0 && WaitFor([&]() { return completed.load() == accepted; });
    }

    void Dispose() noexcept {
        if (client_secure_) client_secure_->Dispose();
        else if (client_ssl_) client_ssl_->Dispose();
        else if (client_ws_) client_ws_->Dispose();
        else if (client_) client_->Dispose();
        if (server_secure_) server_secure_->Dispose();
        else if (server_ssl_) server_ssl_->Dispose();
        else if (server_ws_) server_ws_->Dispose();
        else if (server_) server_->Dispose();
        if (client_secure_ && server_secure_) {
            (void)WaitFor([this]() {
                return client_secure_->IsApplicationPumpStopped() &&
                    server_secure_->IsApplicationPumpStopped();
            });
        }
        guard_.reset();
        // Do not restart and drain callbacks after stopped coroutines have been reclaimed.
        if (context_) context_->stop();
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
        threads_.clear();
    }

    const std::shared_ptr<MultipathTransmission>& ClientSecure() const noexcept { return client_secure_; }
    const std::shared_ptr<MultipathTransmission>& ServerSecure() const noexcept { return server_secure_; }
    const std::shared_ptr<tcp::socket>& ClientSocket() const noexcept { return client_socket_; }
    const std::shared_ptr<ppp::transmissions::ITransmission>& ClientTransmission() const noexcept { return client_; }
    const std::shared_ptr<ppp::transmissions::ITransmission>& ServerTransmission() const noexcept { return server_; }
    const std::shared_ptr<ppp::transmissions::ITransmission>& ClientMux() const noexcept { return client_mux_; }
    const std::shared_ptr<ppp::transmissions::ITransmission>& ServerMux() const noexcept { return server_mux_; }
    const std::shared_ptr<ppp::transmissions::ITransmissionStatistics>& ClientStatistics() const noexcept {
        return client_statistics_;
    }
    const std::shared_ptr<ppp::transmissions::ITransmissionStatistics>& ServerStatistics() const noexcept {
        return server_statistics_;
    }

private:
    std::shared_ptr<asio::io_context> context_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    ppp::transmissions::ITransmission::StrandPtr client_strand_, server_strand_;
    std::shared_ptr<tcp::socket> client_socket_, server_socket_;
    std::shared_ptr<ppp::transmissions::ITransmission> client_, server_;
    std::shared_ptr<ppp::transmissions::IWebsocketTransmission> client_ws_, server_ws_;
    std::shared_ptr<ppp::transmissions::ISslWebsocketTransmission> client_ssl_, server_ssl_;
    std::shared_ptr<MultipathTransmission> client_secure_, server_secure_;
    std::shared_ptr<ppp::transmissions::ITransmission> client_mux_, server_mux_;
    std::shared_ptr<ppp::transmissions::ITransmissionStatistics> client_statistics_, server_statistics_;
    std::vector<std::thread> threads_;
    std::atomic<int> pending_{0};
    std::atomic<bool> client_secure_ok_{false}, server_secure_ok_{false};
    std::atomic<bool> client_control_ok_{false}, server_control_ok_{false};
    std::atomic<bool> client_handshake_ok_{false}, server_handshake_ok_{false};
    std::atomic<bool> client_application_ok_{false}, server_application_ok_{false};
    bool websocket_ = false;
    bool wss_ = false;
    std::uint8_t client_psk_ = 7, server_psk_ = 7;
    bool corrupt_noise_ = false;
    bool client_supports_binary_framing_ = true;
    bool server_supports_binary_framing_ = true;
    bool plaintext_ = false;
};

bool AllPrintableAscii(const std::vector<ppp::Byte>& bytes) {
    return !bytes.empty() && std::all_of(bytes.begin(), bytes.end(), [](ppp::Byte value) {
        return value >= 0x20 && value <= 0x7e;
    });
}

bool ContainsNonPrintable(const std::vector<ppp::Byte>& bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](ppp::Byte value) {
        return value < 0x20 || value > 0x7e;
    });
}

// Stream carrier whose DoWriteBytes observes the bytes after ITransmission
// framing (Base94 envelope included). Two carriers are wired back-to-back so a
// test can measure exactly what each framing state puts on the wire.
class WireCarrier final : public ppp::transmissions::ITransmission {
public:
    using Base = ppp::transmissions::ITransmission;

    WireCarrier(const std::shared_ptr<boost::asio::io_context>& context,
        const std::shared_ptr<boost::asio::strand<boost::asio::io_context::executor_type>>& strand,
        const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration)
        : Base(context, strand, configuration) {
    }

    void Connect(WireCarrier* peer) noexcept { peer_ = peer; }

    void Dispose() noexcept override {
        if (disposed_.exchange(true, std::memory_order_acq_rel)) return;
        YieldContext::Anchor wake;
        {
            std::lock_guard<std::mutex> lock(read_mutex_);
            wake = waiter_;
            waiter_ = {};
        }
        wake.R();
        Base::Dispose();
    }

    bool ShiftToScheduler() noexcept override { return false; }
    boost::asio::ip::tcp::endpoint GetRemoteEndPoint() noexcept override { return {}; }

    void ResetWireCounters() noexcept {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        wire_bytes_ = 0;
        first_write_.clear();
        capturing_ = false;
        captured_.clear();
    }

    std::uint64_t WireBytes() const noexcept {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        return wire_bytes_;
    }

    std::vector<ppp::Byte> FirstWriteBytes() const {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        return first_write_;
    }

    void StartCapture() noexcept {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        capturing_ = true;
        captured_.clear();
    }

    std::vector<ppp::Byte> CapturedBytes() const {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        return captured_;
    }

protected:
    std::shared_ptr<ppp::Byte> DoReadBytes(YieldContext& y, int length) noexcept override {
        if (length < 1) return nullptr;
        for (;;) {
            YieldContext::Anchor anchor;
            {
                std::lock_guard<std::mutex> lock(read_mutex_);
                if (disposed_.load(std::memory_order_acquire)) return nullptr;
                if (static_cast<int>(inbound_.size()) >= length) {
                    auto buffer = ppp::make_shared_alloc<ppp::Byte>(length);
                    if (!buffer) return nullptr;
                    std::memcpy(buffer.get(), inbound_.data(), static_cast<std::size_t>(length));
                    inbound_.erase(inbound_.begin(), inbound_.begin() + length);
                    return buffer;
                }
                anchor = y.Arm();
                if (!anchor) return nullptr;
                waiter_ = anchor;
            }
            // Publish the anchor before the final check so no delivered byte is lost.
            {
                std::lock_guard<std::mutex> lock(read_mutex_);
                if (disposed_.load(std::memory_order_acquire)) {
                    waiter_ = {};
                    return nullptr;
                }
                if (static_cast<int>(inbound_.size()) >= length) {
                    waiter_ = {};
                    continue;
                }
            }
            if (!y.Suspend(anchor)) {
                std::lock_guard<std::mutex> lock(read_mutex_);
                waiter_ = {};
                return nullptr;
            }
        }
    }

    bool DoWriteBytes(std::shared_ptr<ppp::Byte> packet, int offset, int packet_length,
        const AsynchronousWriteBytesCallback& cb) noexcept override {
        if (!packet || offset < 0 || packet_length < 1 || !cb) return false;
        WireCarrier* peer = peer_;
        if (!peer) return false;
        std::vector<ppp::Byte> slice;
        try {
            slice.assign(packet.get() + offset, packet.get() + offset + packet_length);
        } catch (...) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(telemetry_mutex_);
            wire_bytes_ += static_cast<std::uint64_t>(packet_length);
            if (first_write_.empty()) first_write_ = slice;
            if (capturing_) captured_.insert(captured_.end(), slice.begin(), slice.end());
        }
        peer->Deliver(std::move(slice));
        auto strand = GetStrand();
        if (strand) {
            try {
                boost::asio::post(*strand, [cb]() noexcept { cb(true); });
                return true;
            } catch (...) {
            }
        }
        cb(true);
        return true;
    }

private:
    void Deliver(std::vector<ppp::Byte>&& bytes) noexcept {
        YieldContext::Anchor wake;
        {
            std::lock_guard<std::mutex> lock(read_mutex_);
            if (disposed_.load(std::memory_order_acquire)) return;
            try {
                inbound_.insert(inbound_.end(), bytes.begin(), bytes.end());
            } catch (...) {
                return;
            }
            wake = waiter_;
            waiter_ = {};
        }
        wake.R();
    }

    WireCarrier* peer_ = nullptr;
    std::atomic_bool disposed_{false};
    mutable std::mutex read_mutex_;
    std::vector<ppp::Byte> inbound_;
    YieldContext::Anchor waiter_;
    mutable std::mutex telemetry_mutex_;
    std::uint64_t wire_bytes_ = 0;
    std::vector<ppp::Byte> first_write_;
    bool capturing_ = false;
    std::vector<ppp::Byte> captured_;
};

// Same multipath exchange as TcpMultipathPair, but over WireCarrier so the
// test can count post-framing bytes on each direction.
class LoopbackMultipathPair final {
public:
    LoopbackMultipathPair(bool client_supports_binary_framing = true,
        bool server_supports_binary_framing = true)
        : context_(std::make_shared<asio::io_context>())
        , guard_(asio::make_work_guard(*context_))
        , client_supports_binary_framing_(client_supports_binary_framing)
        , server_supports_binary_framing_(server_supports_binary_framing) {
        client_strand_ = std::make_shared<ppp::transmissions::ITransmission::StrandPtr::element_type>(
            asio::make_strand(*context_));
        server_strand_ = std::make_shared<ppp::transmissions::ITransmission::StrandPtr::element_type>(
            asio::make_strand(*context_));
        auto client_config = std::make_shared<ppp::configurations::AppConfiguration>();
        auto server_config = std::make_shared<ppp::configurations::AppConfiguration>();
        client_config->key.kh = client_config->key.kl = 0;
        server_config->key.kh = server_config->key.kl = 0;
        client_config->key.plaintext = server_config->key.plaintext = false;
        client_config->tcp.connect.timeout = server_config->tcp.connect.timeout = 5;
        client_carrier_ = std::make_shared<WireCarrier>(context_, client_strand_, client_config);
        server_carrier_ = std::make_shared<WireCarrier>(context_, server_strand_, server_config);
        client_carrier_->Connect(server_carrier_.get());
        server_carrier_->Connect(client_carrier_.get());
        client_ = client_carrier_;
        server_ = server_carrier_;
    }

    ~LoopbackMultipathPair() { Dispose(); }

    void StartThreads() {
        if (threads_started_) return;
        threads_started_ = true;
        for (int i = 0; i < 4; ++i) threads_.emplace_back([context = context_]() { context->run(); });
    }

    bool Run(bool complete_application_handshake = true) {
        if (!StartNoise()) return false;
        if (!Admit()) return false;
        return !complete_application_handshake || CompleteApplicationHandshake();
    }

    bool StartNoise() {
        StartThreads();
        pending_.store(2);
        const bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                int frame_length = 0;
                auto first_frame = server_->Read(y, frame_length);
                if (first_frame) {
                    server_secure_ = std::make_shared<MultipathTransmission>(server_, TestParameters(),
                        TestSecret(server_psk_), ppp::cryptography::noise::HandshakeRole::NetworkServerResponder,
                        first_frame, frame_length);
                    server_secure_ok_.store(server_secure_->StartSecureServer(y));
                    if (!server_secure_ok_.load()) server_->Dispose();
                }
                pending_.fetch_sub(1);
            });
        const bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                client_secure_ = std::make_shared<MultipathTransmission>(client_, TestParameters(),
                    TestSecret(client_psk_), ppp::cryptography::noise::HandshakeRole::NetworkClientInitiator);
                client_secure_ok_.store(client_secure_->StartSecureClient(y));
                pending_.fetch_sub(1);
            });
        return server_spawned && client_spawned && WaitFor([this]() { return pending_.load() == 0; }) &&
            server_secure_ok_.load() && client_secure_ok_.load();
    }

    bool Admit() {
        pending_.store(2);
        const bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                std::uint8_t type = 0;
                std::vector<std::uint8_t> control;
                const bool read = server_secure_ && server_secure_->ReadSecureRecord(y, type, control) &&
                    type == MultipathTransmission::ControlRecord && IsAdmissionRequest(control);
                const bool advertised = read && AdvertisesBinaryFraming(control);
                const bool negotiate = !server_supports_binary_framing_ || !advertised ||
                    server_secure_->NegotiateBinaryFramingServer(y);
                server_control_ok_.store(read && negotiate &&
                    server_secure_->WriteSecureRecord(y, MultipathTransmission::ControlRecord, "ready", 5));
                pending_.fetch_sub(1);
            });
        const bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                const std::vector<std::uint8_t> request =
                    AdmissionRequest(client_supports_binary_framing_);
                const bool write = client_secure_ && client_secure_->WriteSecureRecord(y,
                    MultipathTransmission::ControlRecord, request.data(), request.size());
                std::uint8_t type = 0;
                std::vector<std::uint8_t> control;
                const bool read = client_secure_ && (client_supports_binary_framing_
                    ? client_secure_->ReadAdmissionResponse(y, type, control)
                    : client_secure_->ReadSecureRecord(y, type, control));
                client_control_ok_.store(write && read &&
                    type == MultipathTransmission::ControlRecord &&
                    control == std::vector<std::uint8_t>{'r','e','a','d','y'});
                pending_.fetch_sub(1);
            });
        return server_spawned && client_spawned && WaitFor([this]() { return pending_.load() == 0; }) &&
            server_control_ok_.load() && client_control_ok_.load();
    }

    bool CompleteApplicationHandshake() {
        pending_.store(2);
        const bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                bool mux = false;
                server_handshake_ok_.store(server_control_ok_.load() &&
                    server_secure_->HandshakeClient(y, mux) == static_cast<ppp::Int128>(0x1234) && !mux);
                server_application_ok_.store(server_handshake_ok_.load() && server_secure_->BeginApplicationIO());
                pending_.fetch_sub(1);
            });
        const bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                client_handshake_ok_.store(client_control_ok_.load() &&
                    client_secure_->HandshakeServer(y, static_cast<ppp::Int128>(0x1234), false));
                client_application_ok_.store(client_handshake_ok_.load() && client_secure_->BeginApplicationIO());
                pending_.fetch_sub(1);
            });
        return server_spawned && client_spawned && WaitFor([this]() { return pending_.load() == 0; }) &&
            server_handshake_ok_.load() && client_handshake_ok_.load() &&
            server_application_ok_.load() && client_application_ok_.load();
    }

    bool LegacyHandshake() {
        StartThreads();
        std::atomic<int> pending{2};
        std::atomic<bool> client_ok{false}, server_ok{false};
        const bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this, &pending, &server_ok](ppp::coroutines::YieldContext& y) noexcept {
                bool mux = false;
                server_ok.store(server_->HandshakeClient(y, mux) == static_cast<ppp::Int128>(0x1234));
                pending.fetch_sub(1);
            });
        const bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this, &pending, &client_ok](ppp::coroutines::YieldContext& y) noexcept {
                client_ok.store(client_->HandshakeServer(y, static_cast<ppp::Int128>(0x1234), false));
                pending.fetch_sub(1);
            });
        return server_spawned && client_spawned && WaitFor([&]() { return pending.load() == 0; }) &&
            client_ok.load() && server_ok.load();
    }

    bool CreateMuxPair() {
        if (!client_secure_ || !server_secure_) return false;
        client_mux_ = client_secure_->CreateMuxSubchannel();
        server_mux_ = server_secure_->CreateMuxSubchannel();
        return client_mux_ && server_mux_;
    }

    bool SecureRecordRoundTrip(const std::string& payload) {
        if (!client_secure_ || !server_secure_) return false;
        std::atomic<int> pending{2};
        std::atomic<bool> read_ok{false}, write_ok{false};
        const bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this, &payload, &pending, &read_ok](ppp::coroutines::YieldContext& y) noexcept {
                std::uint8_t type = 0;
                std::vector<std::uint8_t> data;
                const bool read = server_secure_->ReadSecureRecord(y, type, data);
                read_ok.store(read && type == MultipathTransmission::ApplicationRecord &&
                    data.size() == payload.size() &&
                    std::memcmp(data.data(), payload.data(), payload.size()) == 0);
                pending.fetch_sub(1);
            });
        const bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this, &payload, &pending, &write_ok](ppp::coroutines::YieldContext& y) noexcept {
                write_ok.store(client_secure_->WriteSecureRecord(y,
                    MultipathTransmission::ApplicationRecord, payload.data(), payload.size()));
                pending.fetch_sub(1);
            });
        return server_spawned && client_spawned && WaitFor([&]() { return pending.load() == 0; }) &&
            read_ok.load() && write_ok.load();
    }

    bool RawRoundTrip(const std::string& payload) {
        return Transfer(server_, server_strand_, client_, client_strand_, payload);
    }

    bool SecureRoundTrip(const std::string& payload) {
        if (!client_secure_ || !server_secure_) return false;
        return Transfer(server_secure_, server_strand_, client_secure_, client_strand_, payload);
    }

    bool MuxRoundTrip(const std::string& payload, bool reverse = false) {
        if (!client_mux_ || !server_mux_) return false;
        auto reader = reverse ? client_mux_ : server_mux_;
        auto writer = reverse ? server_mux_ : client_mux_;
        const auto& reader_strand = reverse ? client_strand_ : server_strand_;
        const auto& writer_strand = reverse ? server_strand_ : client_strand_;
        return Transfer(reader, reader_strand, writer, writer_strand, payload);
    }

    std::shared_ptr<WireCarrier> ClientCarrier() const noexcept { return client_carrier_; }
    std::shared_ptr<WireCarrier> ServerCarrier() const noexcept { return server_carrier_; }
    const std::shared_ptr<ppp::transmissions::ITransmission>& ClientTransmission() const noexcept { return client_; }
    const std::shared_ptr<ppp::transmissions::ITransmission>& ServerTransmission() const noexcept { return server_; }
    const std::shared_ptr<MultipathTransmission>& ClientSecure() const noexcept { return client_secure_; }
    const std::shared_ptr<MultipathTransmission>& ServerSecure() const noexcept { return server_secure_; }
    const std::shared_ptr<ppp::transmissions::ITransmission>& ClientMux() const noexcept { return client_mux_; }
    const std::shared_ptr<ppp::transmissions::ITransmission>& ServerMux() const noexcept { return server_mux_; }

    void Dispose() noexcept {
        if (client_secure_) client_secure_->Dispose();
        else if (client_) client_->Dispose();
        if (server_secure_) server_secure_->Dispose();
        else if (server_) server_->Dispose();
        if (client_secure_ && server_secure_) {
            (void)WaitFor([this]() {
                return client_secure_->IsApplicationPumpStopped() &&
                    server_secure_->IsApplicationPumpStopped();
            });
        }
        guard_.reset();
        if (context_) context_->stop();
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
        threads_.clear();
    }

private:
    bool Transfer(const std::shared_ptr<ppp::transmissions::ITransmission>& reader,
        const ppp::transmissions::ITransmission::StrandPtr& reader_strand,
        const std::shared_ptr<ppp::transmissions::ITransmission>& writer,
        const ppp::transmissions::ITransmission::StrandPtr& writer_strand,
        const std::string& payload) {
        std::atomic<int> pending{2};
        std::atomic<bool> read_ok{false}, write_ok{false};
        const bool read_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, reader_strand.get(),
            [&, reader](ppp::coroutines::YieldContext& y) noexcept {
                int length = 0;
                auto bytes = reader->Read(y, length);
                read_ok.store(bytes && length == static_cast<int>(payload.size()) &&
                    std::memcmp(bytes.get(), payload.data(), payload.size()) == 0);
                pending.fetch_sub(1);
            });
        const bool write_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, writer_strand.get(),
            [&, writer](ppp::coroutines::YieldContext& y) noexcept {
                write_ok.store(writer->Write(y, payload.data(), static_cast<int>(payload.size())));
                pending.fetch_sub(1);
            });
        return read_spawned && write_spawned && WaitFor([&]() { return pending.load() == 0; }) &&
            read_ok.load() && write_ok.load();
    }

    std::shared_ptr<asio::io_context> context_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    ppp::transmissions::ITransmission::StrandPtr client_strand_, server_strand_;
    std::shared_ptr<WireCarrier> client_carrier_, server_carrier_;
    std::shared_ptr<ppp::transmissions::ITransmission> client_, server_;
    std::shared_ptr<MultipathTransmission> client_secure_, server_secure_;
    std::shared_ptr<ppp::transmissions::ITransmission> client_mux_, server_mux_;
    std::vector<std::thread> threads_;
    std::atomic<int> pending_{0};
    std::atomic<bool> client_secure_ok_{false}, server_secure_ok_{false};
    std::atomic<bool> client_control_ok_{false}, server_control_ok_{false};
    std::atomic<bool> client_handshake_ok_{false}, server_handshake_ok_{false};
    std::atomic<bool> client_application_ok_{false}, server_application_ok_{false};
    bool threads_started_ = false;
    std::uint8_t client_psk_ = 7, server_psk_ = 7;
    bool client_supports_binary_framing_ = true;
    bool server_supports_binary_framing_ = true;
};

// Message-framed carrier whose write completions stay under test control. It
// stands in for a carrier transport queue: the bytes reach the peer when a
// write is submitted, while the completion that tells the adapter a record is
// finished is released explicitly. Every message is exactly one secure record
// (or one raw handshake frame), so submission order and record boundaries are
// observable without decrypting anything.
class ManualCarrier final : public ppp::transmissions::ITransmission {
public:
    using Base = ppp::transmissions::ITransmission;
    using WriteCallback = Base::AsynchronousWriteCallback;

    ManualCarrier(const std::shared_ptr<boost::asio::io_context>& context,
        const std::shared_ptr<boost::asio::strand<boost::asio::io_context::executor_type>>& strand,
        const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration)
        : Base(context, strand, configuration) {
    }

    void Connect(ManualCarrier* peer) noexcept { peer_ = peer; }

    void Dispose() noexcept override {
        if (disposed_.exchange(true, std::memory_order_acq_rel)) return;
        std::deque<WriteCallback> pending;
        YieldContext::Anchor wake;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(pending_writes_);
            wake = waiter_;
            waiter_ = {};
        }
        wake.R();
        for (WriteCallback& callback : pending) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++completed_writes_;
            }
            callback(false);
        }
        Base::Dispose();
    }

    bool ShiftToScheduler() noexcept override { return false; }
    boost::asio::ip::tcp::endpoint GetRemoteEndPoint() noexcept override { return {}; }

    // Hold every later write completion until the test releases it.
    void Hold() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        holding_ = true;
    }

    void ResetTelemetry() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        submitted_.clear();
        submitted_bytes_ = 0;
        completed_writes_ = 0;
    }

    std::size_t PendingWrites() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_writes_.size();
    }

    std::vector<std::vector<ppp::Byte>> Submitted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return submitted_;
    }

    std::size_t SubmittedBytes() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return submitted_bytes_;
    }

    std::size_t CompletedWrites() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_writes_;
    }

    bool ReleaseOne(bool ok) noexcept {
        WriteCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_writes_.empty()) return false;
            callback = std::move(pending_writes_.front());
            pending_writes_.pop_front();
            ++completed_writes_;
        }
        callback(ok);
        return true;
    }

    bool ReleaseAll(bool ok) noexcept {
        bool released = false;
        while (ReleaseOne(ok)) released = true;
        return released;
    }

    std::shared_ptr<ppp::Byte> Read(YieldContext& y, int& outlen) noexcept override {
        outlen = 0;
        for (;;) {
            YieldContext::Anchor anchor;
            std::vector<ppp::Byte> message;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (disposed_.load(std::memory_order_acquire)) return nullptr;
                if (!inbound_.empty()) {
                    message = std::move(inbound_.front());
                    inbound_.pop_front();
                } else {
                    anchor = y.Arm();
                    if (!anchor) return nullptr;
                    waiter_ = anchor;
                }
            }
            if (!message.empty()) {
                auto buffer = ppp::make_shared_alloc<ppp::Byte>(static_cast<int>(message.size()));
                if (!buffer) return nullptr;
                std::memcpy(buffer.get(), message.data(), message.size());
                outlen = static_cast<int>(message.size());
                return buffer;
            }
            if (!y.Suspend(anchor)) {
                std::lock_guard<std::mutex> lock(mutex_);
                waiter_ = {};
                return nullptr;
            }
        }
    }

    bool Write(YieldContext&, const void* packet, int packet_length) noexcept override {
        return Submit(packet, packet_length, WriteCallback());
    }

    bool Write(const void* packet, int packet_length, const WriteCallback& cb) noexcept override {
        return Submit(packet, packet_length, cb);
    }

protected:
    std::shared_ptr<ppp::Byte> DoReadBytes(YieldContext&, int) noexcept override { return nullptr; }
    bool DoWriteBytes(std::shared_ptr<ppp::Byte>, int, int,
        const AsynchronousWriteBytesCallback&) noexcept override { return false; }

private:
    bool Submit(const void* packet, int packet_length, const WriteCallback& cb) noexcept {
        if (!packet || packet_length < 1) return false;
        ManualCarrier* peer = peer_;
        if (!peer || disposed_.load(std::memory_order_acquire)) return false;
        std::vector<ppp::Byte> message;
        bool held = false;
        try {
            const auto* bytes = reinterpret_cast<const ppp::Byte*>(packet);
            message.assign(bytes, bytes + packet_length);
            std::lock_guard<std::mutex> lock(mutex_);
            submitted_.push_back(message);
            submitted_bytes_ += static_cast<std::size_t>(packet_length);
            if (cb && holding_) {
                pending_writes_.push_back(cb);
                held = true;
            }
        } catch (...) { return false; }
        peer->Deliver(message);
        if (cb && !held) cb(true);
        return true;
    }

    void Deliver(const std::vector<ppp::Byte>& bytes) noexcept {
        YieldContext::Anchor wake;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (disposed_.load(std::memory_order_acquire)) return;
            try { inbound_.push_back(bytes); }
            catch (...) { return; }
            wake = waiter_;
            waiter_ = {};
        }
        wake.R();
    }

    ManualCarrier* peer_ = nullptr;
    std::atomic_bool disposed_{false};
    mutable std::mutex mutex_;
    YieldContext::Anchor waiter_;
    std::deque<std::vector<ppp::Byte>> inbound_;
    std::deque<WriteCallback> pending_writes_;
    std::vector<std::vector<ppp::Byte>> submitted_;
    std::size_t submitted_bytes_ = 0;
    std::size_t completed_writes_ = 0;
    bool holding_ = false;
};

// Noise handshake plus the ordinary application handshake over ManualCarrier.
// The application handshake switches the adapter to binary outer framing, so a
// 64 KiB logical write becomes a four-record run (no Base94 envelope).
class ManualMultipathPair final {
public:
    ManualMultipathPair()
        : context_(std::make_shared<asio::io_context>())
        , guard_(asio::make_work_guard(*context_)) {
        client_strand_ = std::make_shared<ppp::transmissions::ITransmission::StrandPtr::element_type>(
            asio::make_strand(*context_));
        server_strand_ = std::make_shared<ppp::transmissions::ITransmission::StrandPtr::element_type>(
            asio::make_strand(*context_));
        auto client_config = std::make_shared<ppp::configurations::AppConfiguration>();
        auto server_config = std::make_shared<ppp::configurations::AppConfiguration>();
        client_config->key.kh = client_config->key.kl = 0;
        server_config->key.kh = server_config->key.kl = 0;
        client_config->key.plaintext = server_config->key.plaintext = false;
        client_config->tcp.connect.timeout = server_config->tcp.connect.timeout = 5;
        client_carrier_ = std::make_shared<ManualCarrier>(context_, client_strand_, client_config);
        server_carrier_ = std::make_shared<ManualCarrier>(context_, server_strand_, server_config);
        client_carrier_->Connect(server_carrier_.get());
        server_carrier_->Connect(client_carrier_.get());
    }

    ~ManualMultipathPair() { Dispose(); }

    bool Run() {
        for (int i = 0; i < 4; ++i) threads_.emplace_back([context = context_]() { context->run(); });
        return StartNoise() && Handshake();
    }

    const std::shared_ptr<asio::io_context>& Context() const noexcept { return context_; }
    const ppp::transmissions::ITransmission::StrandPtr& ClientStrand() const noexcept { return client_strand_; }
    const ppp::transmissions::ITransmission::StrandPtr& ServerStrand() const noexcept { return server_strand_; }
    const std::shared_ptr<ManualCarrier>& ClientCarrier() const noexcept { return client_carrier_; }
    const std::shared_ptr<MultipathTransmission>& ClientSecure() const noexcept { return client_secure_; }
    const std::shared_ptr<MultipathTransmission>& ServerSecure() const noexcept { return server_secure_; }

    void Dispose() noexcept {
        if (client_secure_) client_secure_->Dispose();
        else if (client_carrier_) client_carrier_->Dispose();
        if (server_secure_) server_secure_->Dispose();
        else if (server_carrier_) server_carrier_->Dispose();
        if (client_secure_ && server_secure_) {
            (void)WaitFor([this]() {
                return client_secure_->IsApplicationPumpStopped() &&
                    server_secure_->IsApplicationPumpStopped();
            });
        }
        guard_.reset();
        if (context_) context_->stop();
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
        threads_.clear();
    }

private:
    bool StartNoise() {
        pending_.store(2);
        const bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                int frame_length = 0;
                auto first_frame = server_carrier_->Read(y, frame_length);
                if (first_frame) {
                    server_secure_ = std::make_shared<MultipathTransmission>(server_carrier_, TestParameters(),
                        TestSecret(server_psk_), ppp::cryptography::noise::HandshakeRole::NetworkServerResponder,
                        first_frame, frame_length);
                    server_secure_ok_.store(server_secure_->StartSecureServer(y));
                }
                pending_.fetch_sub(1);
            });
        const bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                client_secure_ = std::make_shared<MultipathTransmission>(client_carrier_, TestParameters(),
                    TestSecret(client_psk_), ppp::cryptography::noise::HandshakeRole::NetworkClientInitiator);
                client_secure_ok_.store(client_secure_->StartSecureClient(y));
                pending_.fetch_sub(1);
            });
        return server_spawned && client_spawned && WaitFor([this]() { return pending_.load() == 0; }) &&
            server_secure_ok_.load() && client_secure_ok_.load();
    }

    bool Handshake() {
        pending_.store(2);
        const bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                bool mux = false;
                server_handshake_ok_.store(server_secure_ &&
                    server_secure_->HandshakeClient(y, mux) == static_cast<ppp::Int128>(0x1234) && !mux);
                pending_.fetch_sub(1);
            });
        const bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                client_handshake_ok_.store(client_secure_ &&
                    client_secure_->HandshakeServer(y, static_cast<ppp::Int128>(0x1234), false));
                pending_.fetch_sub(1);
            });
        return server_spawned && client_spawned && WaitFor([this]() { return pending_.load() == 0; }) &&
            server_handshake_ok_.load() && client_handshake_ok_.load();
    }

    std::shared_ptr<asio::io_context> context_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    ppp::transmissions::ITransmission::StrandPtr client_strand_, server_strand_;
    std::shared_ptr<ManualCarrier> client_carrier_, server_carrier_;
    std::shared_ptr<MultipathTransmission> client_secure_, server_secure_;
    std::vector<std::thread> threads_;
    std::atomic<int> pending_{0};
    std::atomic<bool> client_secure_ok_{false}, server_secure_ok_{false};
    std::atomic<bool> client_handshake_ok_{false}, server_handshake_ok_{false};
    std::uint8_t client_psk_ = 7, server_psk_ = 7;
};
}

BOOST_AUTO_TEST_CASE(preface_recognition_is_versioned_and_bounded) {
    const unsigned char valid[] = { 'M', 'P', 'T', 'H', 0, 1, 3, 'k', 'e', 'y', 9 };
    bool supported = false;
    ppp::string key_id;
    std::size_t message_offset = 0;
    BOOST_TEST(MultipathTransmission::ParsePreface(valid, sizeof(valid), supported, key_id, message_offset));
    BOOST_TEST(supported);
    BOOST_TEST(key_id == "key");
    BOOST_TEST(message_offset == 10);

    const unsigned char unknown_version[] = { 'M', 'P', 'T', 'H', 0, 2, 1, 'k', 9 };
    BOOST_TEST(MultipathTransmission::IsPreface(unknown_version, sizeof(unknown_version), supported));
    BOOST_TEST(!supported);

    const unsigned char short_magic[] = { 'M', 'P', 'T', 'H' };
    BOOST_TEST(MultipathTransmission::ParsePreface(short_magic, sizeof(short_magic), supported, key_id, message_offset));
    BOOST_TEST(!supported);
    BOOST_TEST(key_id.empty());

    const unsigned char legacy[] = { 'P', 'P', 'P', 0, 1, 2 };
    BOOST_TEST(!MultipathTransmission::IsPreface(legacy, sizeof(legacy), supported));
    BOOST_TEST(!supported);
}

BOOST_AUTO_TEST_CASE(tcp_adapter_secure_control_legacy_handshake_and_fragmented_records) {
    TcpMultipathPair pair;
    BOOST_REQUIRE(pair.Run());
    BOOST_TEST(pair.ExportersMatch());
    const std::string payload(20000, 'x');
    BOOST_TEST(pair.RoundTrip(payload));
    BOOST_TEST(pair.RoundTrip(payload, true));
    BOOST_TEST(pair.ControlDemux("payload after control"));
    BOOST_TEST(pair.DisposeDuringQueuedWrites());
}

BOOST_AUTO_TEST_CASE(main_and_mux_logical_streams_share_one_authenticated_carrier) {
    TcpMultipathPair pair;
    BOOST_REQUIRE(pair.Run());
    BOOST_TEST(pair.LogicalSubchannelsRoundTrip());
    BOOST_TEST(pair.MuxChildDisposeKeepsMainAlive());
    BOOST_TEST(pair.RoundTrip("main survives local mux child disposal"));
}

BOOST_AUTO_TEST_CASE(primary_mux_reads_large_encoded_frames_without_a_second_handshake) {
    TcpMultipathPair pair;
    BOOST_REQUIRE(pair.Run());
    BOOST_TEST(pair.LogicalSubchannelsRoundTrip(false));
}

BOOST_AUTO_TEST_CASE(dispose_wakes_main_and_mux_readers) {
    TcpMultipathPair pair;
    BOOST_REQUIRE(pair.Run());
    BOOST_TEST(pair.DisposeWakesBothLogicalReaders());
}

BOOST_AUTO_TEST_CASE(full_mux_receive_queue_resumes_without_disposing_carrier) {
    TcpMultipathPair pair;
    BOOST_REQUIRE(pair.Run());
    BOOST_REQUIRE(pair.CreateMuxPair());
    BOOST_TEST(pair.CheckFullMuxReceiveQueue(false));
}

BOOST_AUTO_TEST_CASE(dispose_wakes_pump_waiting_for_mux_receive_queue_space) {
    TcpMultipathPair pair;
    BOOST_REQUIRE(pair.Run());
    BOOST_REQUIRE(pair.CreateMuxPair());
    BOOST_TEST(pair.CheckFullMuxReceiveQueue(true));
}

BOOST_AUTO_TEST_CASE(websocket_upgrade_precedes_the_same_secure_adapter) {
    TcpMultipathPair pair(true);
    BOOST_REQUIRE(pair.Run());
    BOOST_TEST(pair.ExportersMatch());
    BOOST_TEST(pair.RoundTrip(std::string(20000, 'w')));
}

BOOST_AUTO_TEST_CASE(wss_forces_certificate_and_hostname_verification_with_split_host) {
    EphemeralCa certificate;
    TcpMultipathPair pair(true, &certificate);
    BOOST_REQUIRE(pair.Run());
    BOOST_TEST(pair.ExportersMatch());
    BOOST_TEST(pair.RoundTrip(std::string(20000, 's')));
}

BOOST_AUTO_TEST_CASE(wss_rejects_untrusted_ca_even_when_legacy_verification_is_off) {
    EphemeralCa trusted_ca;
    EphemeralCa unrelated_ca;
    TcpMultipathPair pair(true, &trusted_ca);
    BOOST_TEST(!pair.Run());
}

BOOST_AUTO_TEST_CASE(wss_rejects_wrong_tls_server_name) {
    EphemeralCa certificate;
    TcpMultipathPair pair(true, &certificate, "wrong.example.test");
    BOOST_TEST(!pair.Run());
}

BOOST_AUTO_TEST_CASE(noise_rejects_bad_psk_or_corrupted_handshake_before_ppp) {
    {
        TcpMultipathPair pair(false, nullptr, "localhost", 7, 8);
        BOOST_TEST(!pair.Run());
        BOOST_TEST(pair.NoAuthenticatedPPPHandshake());
    }
    {
        TcpMultipathPair pair(false, nullptr, "localhost", 7, 7, true);
        BOOST_TEST(!pair.Run());
        BOOST_TEST(pair.NoAuthenticatedPPPHandshake());
    }
}

BOOST_AUTO_TEST_CASE(multipath_noise_preface_and_response_stay_base94_until_admission) {
    LoopbackMultipathPair pair;
    BOOST_REQUIRE(pair.StartNoise());
    BOOST_TEST(pair.ClientSecure()->IsSecureReady());
    BOOST_TEST(pair.ServerSecure()->IsSecureReady());
    BOOST_TEST(!pair.ClientSecure()->IsHandshakeComplete());
    // Noise v1 itself is unchanged: no peer switches framing just because the
    // other side can.  The upgrade is authenticated admission after Noise.
    BOOST_TEST(!pair.ClientTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());
    BOOST_TEST(!pair.ServerTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());

    // The client's first frame (preface + Noise message 1) and the server's
    // Noise response must still be the compatibility Base94 envelope: every
    // byte stays inside the Base94 alphabet.  A binary framed handshake would
    // expose the packet header/ciphertext bytes here.
    const auto client_preface = pair.ClientCarrier()->FirstWriteBytes();
    const auto server_response = pair.ServerCarrier()->FirstWriteBytes();
    BOOST_TEST(!client_preface.empty());
    BOOST_TEST(!server_response.empty());
    BOOST_TEST(AllPrintableAscii(client_preface));
    BOOST_TEST(AllPrintableAscii(server_response));
    // Preface(7) + key id(8) + Noise message 1/2(48) = 63 plaintext bytes.
    // Base94 expands that past 84 bytes; binary framing would stay near 67.
    BOOST_TEST(client_preface.size() > 84u);
    BOOST_TEST(server_response.size() > 84u);

    // Two upgraded peers switch only after the authenticated admission
    // exchange, which runs over the same still-Base94 legacy envelope.
    BOOST_REQUIRE(pair.Admit());
    BOOST_TEST(pair.ClientTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());
    BOOST_TEST(pair.ServerTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());
}

BOOST_AUTO_TEST_CASE(multipath_noise_switch_makes_raw_carrier_records_binary_before_ppp) {
    LoopbackMultipathPair pair;
    BOOST_REQUIRE(pair.Run(false));
    BOOST_TEST(pair.ClientSecure()->IsSecureReady());
    BOOST_TEST(!pair.ClientSecure()->IsHandshakeComplete());
    BOOST_TEST(pair.ClientTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());
    BOOST_TEST(pair.ServerTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());

    // Noise and admission are complete but the legacy PPP handshake has not run
    // yet.  A sealed record on the raw carrier must already be binary framed on
    // both sides; otherwise the carrier still adds ~1.33x Base94 overhead.
    const std::string payload(16000, 'b');
    pair.ClientCarrier()->ResetWireCounters();
    pair.ClientCarrier()->StartCapture();
    BOOST_REQUIRE(pair.SecureRecordRoundTrip(payload));

    const std::uint64_t wire_bytes = pair.ClientCarrier()->WireBytes();
    const auto captured = pair.ClientCarrier()->CapturedBytes();
    BOOST_TEST(wire_bytes >= payload.size());
    BOOST_TEST(wire_bytes < payload.size() * 11 / 10);
    BOOST_TEST(ContainsNonPrintable(captured));
}

BOOST_AUTO_TEST_CASE(primary_mux_large_payload_round_trips_over_one_binary_layer) {
    LoopbackMultipathPair pair;
    BOOST_REQUIRE(pair.Run());
    BOOST_REQUIRE(pair.CreateMuxPair());
    BOOST_TEST(pair.ClientMux()->IsAuthenticatedMultipathBinaryFramingEnabled());
    BOOST_TEST(pair.ServerMux()->IsAuthenticatedMultipathBinaryFramingEnabled());

    // The mux subchannel is created after the secure handshake.  Its write
    // path must not add a second Base94 envelope, and the raw carrier must not
    // add one either, so a legal 60 KB payload lands near plaintext size.
    const std::string payload(60000, 'm');
    pair.ClientCarrier()->ResetWireCounters();
    BOOST_REQUIRE(pair.SecureRoundTrip(payload));
    BOOST_TEST(pair.ClientCarrier()->WireBytes() < payload.size() * 11 / 10);

    pair.ClientCarrier()->ResetWireCounters();
    pair.ClientCarrier()->StartCapture();
    BOOST_REQUIRE(pair.MuxRoundTrip(payload));
    BOOST_TEST(pair.ClientCarrier()->WireBytes() < payload.size() * 11 / 10);
    BOOST_TEST(ContainsNonPrintable(pair.ClientCarrier()->CapturedBytes()));

    pair.ServerCarrier()->ResetWireCounters();
    pair.ServerCarrier()->StartCapture();
    BOOST_REQUIRE(pair.MuxRoundTrip(payload, true));
    BOOST_TEST(pair.ServerCarrier()->WireBytes() < payload.size() * 11 / 10);
    BOOST_TEST(ContainsNonPrintable(pair.ServerCarrier()->CapturedBytes()));
}

BOOST_AUTO_TEST_CASE(ordinary_transmission_legacy_handshake_keeps_base94_then_binary) {
    LoopbackMultipathPair pair;
    pair.StartThreads();
    BOOST_REQUIRE(pair.LegacyHandshake());

    // Ordinary transmission handshake frames keep the legacy Base94 envelope.
    BOOST_TEST(AllPrintableAscii(pair.ClientCarrier()->FirstWriteBytes()));
    BOOST_TEST(AllPrintableAscii(pair.ServerCarrier()->FirstWriteBytes()));
    BOOST_TEST(pair.ClientTransmission()->IsHandshakeComplete());
    BOOST_TEST(pair.ServerTransmission()->IsHandshakeComplete());
    BOOST_TEST(!pair.ClientTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());
    BOOST_TEST(!pair.ServerTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());

    // And the pre-existing handshaked_ transition still selects binary framing
    // without the multipath-only switch.
    const std::string payload(16000, 'l');
    pair.ClientCarrier()->ResetWireCounters();
    BOOST_REQUIRE(pair.RawRoundTrip(payload));
    BOOST_TEST(pair.ClientCarrier()->WireBytes() < payload.size() * 11 / 10);
}


BOOST_AUTO_TEST_CASE(legacy_client_or_server_keeps_base94_and_still_round_trips) {
    struct Combination final {
        bool client_supports_binary_framing;
        bool server_supports_binary_framing;
    };
    const Combination combinations[] = {
        { false, true },   // upgraded responder must not offer to a legacy client
        { true, false },   // upgraded client accepts the absent legacy offer
        { false, false }
    };
    for (const Combination& combination : combinations) {
        TcpMultipathPair pair(false, nullptr, "localhost", 7, 7, false,
            combination.client_supports_binary_framing,
            combination.server_supports_binary_framing);
        BOOST_REQUIRE_MESSAGE(pair.Run(), "client=" << combination.client_supports_binary_framing
            << " server=" << combination.server_supports_binary_framing);
        BOOST_TEST(pair.ExportersMatch());
        BOOST_TEST(pair.ClientSecure()->IsSecureReady());
        BOOST_TEST(pair.ServerSecure()->IsSecureReady());
        BOOST_TEST(!pair.ClientTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());
        BOOST_TEST(!pair.ServerTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());
        BOOST_TEST(pair.ClientSecure()->IsHandshakeComplete());
        BOOST_TEST(pair.ServerSecure()->IsHandshakeComplete());

        BOOST_REQUIRE(pair.CreateMuxPair());
        BOOST_TEST(!pair.ClientMux()->IsAuthenticatedMultipathBinaryFramingEnabled());
        BOOST_TEST(!pair.ServerMux()->IsAuthenticatedMultipathBinaryFramingEnabled());
        const std::string payload(20000, 'l');
        pair.ClientStatistics()->Clear();
        BOOST_REQUIRE(pair.MuxRoundTrip(payload));
        BOOST_TEST(pair.ClientStatistics()->OutgoingTraffic.load() > payload.size() * 5 / 4);

        // Still one authenticated Noise session: no extra handshake ran.
        BOOST_TEST(pair.ExportersMatch());
    }
}

BOOST_AUTO_TEST_CASE(tcp_supporting_peers_switch_then_large_mux_record_avoids_base94) {
    TcpMultipathPair pair;
    BOOST_REQUIRE(pair.Run());
    BOOST_TEST(pair.ClientTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());
    BOOST_TEST(pair.ServerTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());
    BOOST_REQUIRE(pair.CreateMuxPair());
    BOOST_TEST(pair.ClientMux()->IsAuthenticatedMultipathBinaryFramingEnabled());
    BOOST_TEST(pair.ServerMux()->IsAuthenticatedMultipathBinaryFramingEnabled());

    // Count the real socket bytes (raw carrier Statistics) for a large MuxRecord
    // in both directions.  Base94 would add ~1.33x; the switched path must stay
    // within one binary record envelope.
    const std::string payload(60000, 'm');
    pair.ClientStatistics()->Clear();
    BOOST_REQUIRE(pair.MuxRoundTrip(payload));
    const std::uint64_t client_outgoing = pair.ClientStatistics()->OutgoingTraffic.load();
    BOOST_TEST(client_outgoing >= payload.size());
    BOOST_TEST(client_outgoing < payload.size() * 11 / 10);

    pair.ServerStatistics()->Clear();
    BOOST_REQUIRE(pair.MuxRoundTrip(payload, true));
    const std::uint64_t server_outgoing = pair.ServerStatistics()->OutgoingTraffic.load();
    BOOST_TEST(server_outgoing >= payload.size());
    BOOST_TEST(server_outgoing < payload.size() * 11 / 10);
}


BOOST_AUTO_TEST_CASE(plaintext_carrier_keeps_base94_even_when_both_peers_support_upgrade) {
    TcpMultipathPair pair(false, nullptr, "localhost", 7, 7, false, true, true, true);
    BOOST_REQUIRE(pair.Run());
    BOOST_TEST(pair.ClientSecure()->IsSecureReady());
    BOOST_TEST(pair.ServerSecure()->IsSecureReady());
    BOOST_TEST(pair.ClientSecure()->GetInitialCarrierRttMs() > 0u);
    BOOST_TEST(pair.ServerSecure()->GetInitialCarrierRttMs() == 0u);
    // key.plaintext forces the legacy envelope, so the responder's negotiation
    // is a no-op success and neither side flips its framing state.
    BOOST_TEST(!pair.ClientTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());
    BOOST_TEST(!pair.ServerTransmission()->IsAuthenticatedMultipathBinaryFramingEnabled());

    const std::string payload(20000, 'p');
    pair.ClientStatistics()->Clear();
    BOOST_REQUIRE(pair.RoundTrip(payload));
    BOOST_TEST(pair.ClientStatistics()->OutgoingTraffic.load() > payload.size() * 5 / 4);
}


BOOST_AUTO_TEST_CASE(mux_rtt_includes_the_authenticated_peer_beyond_a_fast_transport) {
    const auto check = [](TcpMultipathPair& pair) {
        BOOST_TEST(pair.ClientTransmission()->GetInitialCarrierRttMs() == 0u);
        BOOST_TEST(pair.ServerTransmission()->GetInitialCarrierRttMs() == 0u);
        // Add delay beyond the connected loopback socket, as on a proxied carrier.
        constexpr auto upstream_delay = std::chrono::milliseconds(20);
        BOOST_REQUIRE(pair.Run(upstream_delay));
        BOOST_REQUIRE(pair.CreateMuxPair());
        const auto client_rtt = pair.ClientSecure()->GetInitialCarrierRttMs();
        const auto server_rtt = pair.ServerSecure()->GetInitialCarrierRttMs();
        BOOST_TEST(client_rtt >= static_cast<std::uint64_t>(upstream_delay.count()));
        BOOST_TEST(server_rtt > 0u);
        BOOST_TEST(pair.ClientMux()->GetInitialCarrierRttMs() == client_rtt);
        BOOST_TEST(pair.ServerMux()->GetInitialCarrierRttMs() == server_rtt);
    };
    TcpMultipathPair tcp;
    check(tcp);
    TcpMultipathPair ws(true);
    check(ws);
    EphemeralCa ca;
    TcpMultipathPair wss(true, &ca);
    check(wss);
}

BOOST_AUTO_TEST_CASE(tcp_carrier_applies_and_clears_wire_pacing_without_closing) {
    TcpMultipathPair pair;
    BOOST_REQUIRE(pair.Run());
    const auto& carrier = pair.ClientTransmission();
    BOOST_REQUIRE(carrier);

    constexpr std::uint64_t rate = 3u * 1024u * 1024u;
#if defined(__linux__) && defined(SO_MAX_PACING_RATE)
    BOOST_TEST(carrier->SupportsPacingRate());
    BOOST_TEST(static_cast<int>(carrier->ApplyPacingRate(rate)) ==
        static_cast<int>(ppp::transmissions::TransmissionPacingResult::Applied));
    BOOST_TEST(carrier->GetPacingRateBytesPerSecond() == rate);

    std::uint32_t kernel_rate = 0;
    socklen_t kernel_length = sizeof(kernel_rate);
    BOOST_REQUIRE(::getsockopt(pair.ClientSocket()->native_handle(), SOL_SOCKET,
        SO_MAX_PACING_RATE, &kernel_rate, &kernel_length) == 0);
    BOOST_TEST(kernel_rate == rate);

    // The authenticated mux adapter forwards to the same raw socket, and a
    // repeated target must not reach the kernel twice.
    BOOST_TEST(pair.ClientSecure()->SupportsPacingRate());
    BOOST_TEST(static_cast<int>(pair.ClientSecure()->ApplyPacingRate(rate)) ==
        static_cast<int>(ppp::transmissions::TransmissionPacingResult::Unchanged));
    BOOST_TEST(pair.ClientSecure()->GetPacingRateBytesPerSecond() == rate);
    // The scheduler paces the VMUX logical channel, which must forward to the
    // same raw carrier instead of reporting an unpaced channel.
    BOOST_REQUIRE(pair.CreateMuxPair());
    BOOST_TEST(pair.ClientMux()->SupportsPacingRate());
    BOOST_TEST(static_cast<int>(pair.ClientMux()->ApplyPacingRate(rate)) ==
        static_cast<int>(ppp::transmissions::TransmissionPacingResult::Unchanged));
    BOOST_TEST(pair.ClientMux()->GetPacingRateBytesPerSecond() == rate);
    BOOST_TEST(static_cast<int>(carrier->ApplyPacingRate(rate)) ==
        static_cast<int>(ppp::transmissions::TransmissionPacingResult::Unchanged));

    BOOST_TEST(static_cast<int>(carrier->ApplyPacingRate(0)) ==
        static_cast<int>(ppp::transmissions::TransmissionPacingResult::Cleared));
    BOOST_TEST(carrier->GetPacingRateBytesPerSecond() == 0);
    kernel_rate = 1;
    kernel_length = sizeof(kernel_rate);
    BOOST_REQUIRE(::getsockopt(pair.ClientSocket()->native_handle(), SOL_SOCKET,
        SO_MAX_PACING_RATE, &kernel_rate, &kernel_length) == 0);
    // Cleared to the kernel's "no limit" sentinel, never to a zero rate.
    BOOST_TEST(kernel_rate == ~0u);
    BOOST_TEST(static_cast<int>(carrier->ApplyPacingRate(0)) ==
        static_cast<int>(ppp::transmissions::TransmissionPacingResult::Cleared));
#else
    BOOST_TEST(!carrier->SupportsPacingRate());
    BOOST_TEST(static_cast<int>(carrier->ApplyPacingRate(rate)) ==
        static_cast<int>(ppp::transmissions::TransmissionPacingResult::Unsupported));
#endif

    // Pacing is advisory: the carrier must stay usable afterwards.
    BOOST_REQUIRE(pair.RoundTrip(std::string(4096, 'p')));
}

BOOST_AUTO_TEST_CASE(websocket_carrier_reports_pacing_unsupported_without_side_effects) {
    TcpMultipathPair pair(true);
    BOOST_REQUIRE(pair.Run());
    const auto& carrier = pair.ClientTransmission();
    BOOST_REQUIRE(carrier);
    BOOST_TEST(!carrier->SupportsPacingRate());
    BOOST_TEST(static_cast<int>(pair.ClientSecure()->ApplyPacingRate(1024 * 1024)) ==
        static_cast<int>(ppp::transmissions::TransmissionPacingResult::Unsupported));
    BOOST_TEST(pair.ClientSecure()->GetPacingRateBytesPerSecond() == 0);
    BOOST_REQUIRE(pair.RoundTrip(std::string(4096, 'w')));
}

BOOST_AUTO_TEST_CASE(logical_write_submits_its_whole_record_run_before_any_completion) {
    ManualMultipathPair pair;
    BOOST_REQUIRE(pair.Run());

    const auto carrier = pair.ClientCarrier();
    BOOST_REQUIRE(carrier);
    carrier->ResetTelemetry();
    carrier->Hold();

    constexpr std::size_t chunk = ppp::cryptography::noise::MultipathRecordMaxPlaintext;
    constexpr std::size_t record_overhead = ppp::cryptography::noise::MultipathRecordOverhead;
    // After the application handshake the adapter uses its binary outer
    // framing, whose 3-byte header is the only addition. This payload therefore
    // reaches WriteLogicalBytes as exactly one 64 KiB logical write: four full
    // secure records and nothing left over.
    constexpr std::size_t frame_header = 3;
    constexpr std::size_t payload_size = 4 * chunk - frame_header;
    std::vector<ppp::Byte> payload(payload_size);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<ppp::Byte>((i * 31 + 7) & 0xff);
    }

    std::atomic<int> write_completions{0};
    std::atomic<int> write_result{-1};
    std::atomic<bool> write_done{false};
    const bool write_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *pair.Context(),
        pair.ClientStrand().get(), [&](ppp::coroutines::YieldContext& y) noexcept {
            write_result.store(pair.ClientSecure()->Write(y, payload.data(),
                static_cast<int>(payload.size())) ? 1 : 0);
            write_completions.fetch_add(1);
            write_done.store(true);
        });
    BOOST_REQUIRE(write_spawned);

    std::atomic<bool> read_result{false};
    std::atomic<bool> read_done{false};
    const bool read_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *pair.Context(),
        pair.ServerStrand().get(), [&](ppp::coroutines::YieldContext& y) noexcept {
            int length = 0;
            auto bytes = pair.ServerSecure()->Read(y, length);
            read_result.store(bytes && length == static_cast<int>(payload.size()) &&
                std::memcmp(bytes.get(), payload.data(), payload.size()) == 0);
            read_done.store(true);
        });
    BOOST_REQUIRE(read_spawned);

    // The whole run is handed to the carrier before the first record may
    // complete; the previous one-record-at-a-time chain topped out at one.
    BOOST_REQUIRE(WaitFor([&]() { return carrier->PendingWrites() == 4; }));
    BOOST_TEST(carrier->CompletedWrites() == 0u);
    BOOST_TEST(!write_done.load());
    // Submission order is wire order, so the peer reads the complete 64 KiB
    // logical write while no local record completion has been reported yet.
    BOOST_REQUIRE(WaitFor([&]() { return read_done.load(); }));
    BOOST_TEST(read_result.load());
    BOOST_TEST(carrier->CompletedWrites() == 0u);
    BOOST_TEST(!write_done.load());

    const std::vector<std::vector<ppp::Byte>> records = carrier->Submitted();
    BOOST_TEST(records.size() == 4u);
    BOOST_TEST(carrier->SubmittedBytes() == 4 * (chunk + record_overhead));
    for (const std::vector<ppp::Byte>& record : records) {
        BOOST_TEST(record.size() == chunk + record_overhead);
        // Record order is chunk order: every record declares a full 16 KiB
        // ciphertext and the AEAD sequence accepts them only in that order.
        const std::uint8_t* header = reinterpret_cast<const std::uint8_t*>(record.data());
        const std::size_t cipher_size = (static_cast<std::size_t>(header[0]) << 24) |
            (static_cast<std::size_t>(header[1]) << 16) |
            (static_cast<std::size_t>(header[2]) << 8) |
            static_cast<std::size_t>(header[3]);
        BOOST_TEST(cipher_size == chunk + 1 + 16);
    }

    BOOST_TEST(carrier->ReleaseOne(true));
    BOOST_TEST(carrier->ReleaseOne(true));
    BOOST_TEST(carrier->ReleaseOne(true));
    // The last record is still outstanding, so the logical write may not
    // finish yet. The peer read is not part of this contract: the manual
    // carrier delivers every submitted record immediately, so the reader may
    // have completed before any local carrier completion was released.
    BOOST_TEST(!write_done.load());
    BOOST_TEST(carrier->PendingWrites() == 1u);

    BOOST_TEST(carrier->ReleaseOne(true));
    BOOST_REQUIRE(WaitFor([&]() { return write_done.load() && read_done.load(); }));
    BOOST_TEST(write_result.load() == 1);
    BOOST_TEST(read_result.load());
    BOOST_TEST(write_completions.load() == 1);
    BOOST_TEST(carrier->Submitted().size() == 4u);
    BOOST_TEST(carrier->CompletedWrites() == 4u);
}

BOOST_AUTO_TEST_CASE(logical_write_reports_one_failure_and_never_continues_a_partial_run) {
    ManualMultipathPair pair;
    BOOST_REQUIRE(pair.Run());

    const auto carrier = pair.ClientCarrier();
    BOOST_REQUIRE(carrier);
    carrier->ResetTelemetry();
    carrier->Hold();

    constexpr std::size_t chunk = ppp::cryptography::noise::MultipathRecordMaxPlaintext;
    constexpr std::size_t frame_header = 3;
    constexpr std::size_t payload_size = 4 * chunk - frame_header;
    const std::string payload(payload_size, 'f');

    std::atomic<int> write_completions{0};
    std::atomic<int> write_result{-1};
    std::atomic<bool> write_done{false};
    const bool write_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *pair.Context(),
        pair.ClientStrand().get(), [&](ppp::coroutines::YieldContext& y) noexcept {
            write_result.store(pair.ClientSecure()->Write(y, payload.data(),
                static_cast<int>(payload.size())) ? 1 : 0);
            write_completions.fetch_add(1);
            write_done.store(true);
        });
    BOOST_REQUIRE(write_spawned);
    BOOST_REQUIRE(WaitFor([&]() { return carrier->PendingWrites() == 4; }));
    BOOST_TEST(carrier->CompletedWrites() == 0u);
    BOOST_TEST(!write_done.load());

    // One failed record fails the logical write exactly once and the remaining
    // records of the run are failed with it instead of being reported later.
    BOOST_TEST(carrier->ReleaseOne(false));
    BOOST_TEST(carrier->PendingWrites() == 0u);
    BOOST_TEST(carrier->CompletedWrites() == 4u);
    BOOST_TEST(!carrier->ReleaseAll(true));
    BOOST_REQUIRE(WaitFor([&]() { return write_done.load(); }));
    BOOST_TEST(write_result.load() == 0);
    BOOST_TEST(write_completions.load() == 1);
}
