#define BOOST_TEST_MODULE multipath_transmission_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/configurations/AppConfiguration.h>
#include <ppp/coroutines/YieldContext.h>
#include <ppp/transmissions/ITcpipTransmission.h>
#include <ppp/transmissions/IWebsocketTransmission.h>
#include <ppp/transmissions/MultipathTransmission.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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
        std::uint8_t server_psk = 7, bool corrupt_noise = false)
        : context_(std::make_shared<asio::io_context>())
        , guard_(asio::make_work_guard(*context_))
        , websocket_(websocket)
        , wss_(certificate != nullptr)
        , client_psk_(client_psk)
        , server_psk_(server_psk)
        , corrupt_noise_(corrupt_noise) {
        tcp::acceptor acceptor(*context_, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        auto client_socket = std::make_shared<tcp::socket>(*context_);
        auto server_socket = std::make_shared<tcp::socket>(*context_);
        client_socket->connect(acceptor.local_endpoint());
        acceptor.accept(*server_socket);
        client_strand_ = std::make_shared<ppp::transmissions::ITransmission::StrandPtr::element_type>(asio::make_strand(*context_));
        server_strand_ = std::make_shared<ppp::transmissions::ITransmission::StrandPtr::element_type>(asio::make_strand(*context_));
        auto client_config = std::make_shared<ppp::configurations::AppConfiguration>();
        auto server_config = std::make_shared<ppp::configurations::AppConfiguration>();
        client_config->key.kh = client_config->key.kl = 0;
        server_config->key.kh = server_config->key.kl = 0;
        client_config->tcp.connect.timeout = server_config->tcp.connect.timeout = 5;
        if (wss_) {
            client_config->websocket.host = server_config->websocket.host = "localhost";
            client_config->websocket.path = server_config->websocket.path = "/multipath";
            server_config->websocket.ssl.certificate_file = certificate->CertificateFile();
            server_config->websocket.ssl.certificate_key_file = certificate->KeyFile();
            server_config->websocket.ssl.certificate_chain_file = certificate->CertificateFile();
            client_ssl_ = std::make_shared<ppp::transmissions::ISslWebsocketTransmission>(context_, client_strand_,
                client_socket, client_config);
            server_ssl_ = std::make_shared<ppp::transmissions::ISslWebsocketTransmission>(context_, server_strand_,
                server_socket, server_config);
            client_ssl_->TlsServerName = std::move(tls_server_name);
            client_ssl_->HttpHost = "edge.example.test";
            client_ssl_->Path = "/multipath";
            client_ssl_->ForceVerifyPeer = true;
            server_ssl_->Path = "/multipath";
            server_ssl_->HttpHost = "localhost";
            client_ = client_ssl_;
            server_ = server_ssl_;
        } else if (websocket_) {
            client_ws_ = std::make_shared<ppp::transmissions::IWebsocketTransmission>(context_, client_strand_, client_socket,
                client_config);
            server_ws_ = std::make_shared<ppp::transmissions::IWebsocketTransmission>(context_, server_strand_, server_socket,
                server_config);
            client_ws_->Host = server_ws_->Host = "localhost";
            client_ws_->Path = server_ws_->Path = "/multipath";
            client_ = client_ws_;
            server_ = server_ws_;
        } else {
            client_ = std::make_shared<ppp::transmissions::ITcpipTransmission>(context_, client_strand_, client_socket,
                client_config, ppp::transmissions::TcpTransmissionRole::Main);
            server_ = std::make_shared<ppp::transmissions::ITcpipTransmission>(context_, server_strand_, server_socket,
                server_config, ppp::transmissions::TcpTransmissionRole::Server);
        }
    }

    ~TcpMultipathPair() { Dispose(); }

    bool Run() {
        for (int i = 0; i < 4; ++i) threads_.emplace_back([context = context_]() { context->run(); });
        if (websocket_ && !UpgradeWebsocket()) return false;
        pending_.store(2);
        const bool server_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                int frame_length = 0;
                auto first_frame = server_->Read(y, frame_length);
                if (first_frame) {
                    if (corrupt_noise_ && frame_length > 0) first_frame.get()[frame_length - 1] ^= 1;
                    server_secure_ = std::make_shared<MultipathTransmission>(server_, TestParameters(), TestSecret(server_psk_),
                        ppp::cryptography::noise::HandshakeRole::NetworkServerResponder, first_frame, frame_length);
                    server_secure_ok_.store(server_secure_->StartSecureServer(y));
                    if (!server_secure_ok_.load()) server_->Dispose();
                    std::uint8_t type = 0;
                    std::vector<std::uint8_t> control;
                    server_control_ok_.store(server_secure_ok_.load() &&
                        server_secure_->ReadSecureRecord(y, type, control) &&
                        type == MultipathTransmission::ControlRecord && control == std::vector<std::uint8_t>{'c','r','e','a','t','e'} &&
                        server_secure_->WriteSecureRecord(y, MultipathTransmission::ControlRecord, "ready", 5));
                    bool mux = false;
                    server_handshake_ok_.store(server_control_ok_.load() &&
                        server_secure_->HandshakeClient(y, mux) == static_cast<ppp::Int128>(0x1234) && !mux);
                    server_application_ok_.store(server_handshake_ok_.load() && server_secure_->BeginApplicationIO());
                }
                pending_.fetch_sub(1);
            });
        const bool client_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [this](ppp::coroutines::YieldContext& y) noexcept {
                client_secure_ = std::make_shared<MultipathTransmission>(client_, TestParameters(), TestSecret(client_psk_),
                    ppp::cryptography::noise::HandshakeRole::NetworkClientInitiator);
                client_secure_ok_.store(client_secure_->StartSecureClient(y));
                std::uint8_t type = 0;
                std::vector<std::uint8_t> control;
                client_control_ok_.store(client_secure_ok_.load() &&
                    client_secure_->WriteSecureRecord(y, MultipathTransmission::ControlRecord, "create", 6) &&
                    client_secure_->ReadSecureRecord(y, type, control) &&
                    type == MultipathTransmission::ControlRecord && control == std::vector<std::uint8_t>{'r','e','a','d','y'});
                client_handshake_ok_.store(client_control_ok_.load() &&
                    client_secure_->HandshakeServer(y, static_cast<ppp::Int128>(0x1234), false));
                client_application_ok_.store(client_handshake_ok_.load() && client_secure_->BeginApplicationIO());
                pending_.fetch_sub(1);
            });
        return server_spawned && client_spawned && WaitFor([this]() { return pending_.load() == 0; }) &&
            server_secure_ok_.load() && client_secure_ok_.load() && server_control_ok_.load() &&
            client_control_ok_.load() && server_handshake_ok_.load() && client_handshake_ok_.load() &&
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

    bool LogicalSubchannelsRoundTrip() {
        client_mux_ = client_secure_->CreateMuxSubchannel();
        server_mux_ = server_secure_->CreateMuxSubchannel();
        auto client_mux = client_mux_;
        auto server_mux = server_mux_;
        if (!client_mux || !server_mux || client_mux == client_secure_ || server_mux == server_secure_ ||
            client_secure_->CreateMuxSubchannel() != client_mux ||
            server_secure_->CreateMuxSubchannel() != server_mux) return false;

        std::atomic<int> pending{2};
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
            !client_handshake.load() || !server_handshake.load()) return false;

        const std::array<std::string, 4> payloads = {
            "main client to server", "main server to client",
            "mux client to server", "mux server to client"
        };
        std::array<std::atomic<bool>, 4> reads{}, writes{};
        std::atomic<bool> control_seen{false};
        if (!server_secure_->SetControlRecordHandler([&control_seen](std::uint8_t type,
            const std::shared_ptr<const std::vector<std::uint8_t>>& record) noexcept {
            const std::vector<std::uint8_t> expected{ 'i', 'n', 't', 'e', 'r', 'l', 'e', 'a', 'v', 'e' };
            const bool ok = type == MultipathTransmission::ControlRecord && record && *record == expected;
            control_seen.store(ok);
            return ok;
        })) return false;

        pending.store(9);
        const auto spawn_read = [this, &pending, &reads, &payloads](int index,
            const std::shared_ptr<ppp::transmissions::ITransmission>& stream,
            const ppp::transmissions::ITransmission::StrandPtr& strand) noexcept {
            return ppp::coroutines::YieldContext::Spawn(nullptr, *context_, strand.get(),
                [&, index, stream](ppp::coroutines::YieldContext& y) noexcept {
                    int length = 0;
                    auto bytes = stream->Read(y, length);
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
        const bool complete = spawned && WaitFor([&]() { return pending.load() == 0; });
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

    bool MuxQueueSaturationClosesCarrier() {
        auto client_mux = client_secure_->CreateMuxSubchannel();
        auto server_mux = server_secure_->CreateMuxSubchannel();
        if (!client_mux || !server_mux) return false;

        // Drive the mux receive-queue bound directly.  The server application pump
        // moves records from the carrier into the mux channel but nothing consumes
        // that channel, so saturation is decided by the queue bound itself
        // (MaxLogicalRecords = 128 queued records) instead of loopback socket
        // buffer luck.  Small payloads keep the whole in-flight amount far below
        // any TCP receive window, so the carrier write path never stalls on flow
        // control and the saturation exit becomes deterministic.  Write failures
        // after the carrier closes are part of the expected outcome, not a flake.
        std::atomic<bool> write_done{false};
        const std::string payload(1024, 'b');
        const bool write_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, client_strand_.get(),
            [client_mux, &write_done, &payload](ppp::coroutines::YieldContext& y) noexcept {
                for (int i = 0; i < 256; ++i) {
                    if (!client_mux->Write(y, payload.data(), static_cast<int>(payload.size()))) break;
                }
                write_done.store(true);
            });
        if (!write_spawned) return false;

        // Contract: mux receive-queue saturation disposes the whole carrier, which
        // clears the secure channel and flips IsSecureReady to false.
        const bool closed = WaitFor([&]() { return !server_secure_->IsSecureReady(); });

        // The original case spawned one reader before the flood and waited for that
        // single read attempt to fail.  That raced in two ways: if Dispose woke the
        // parked read too late the waiter never observed the failure, and a live
        // reader also drains the mux queue as records arrive, so the receive bound
        // may never trip at all.  A reader spawned after saturation is deterministic
        // instead: ReadLogicalBytes reports the closed channel immediately, so this
        // read can never park waiting for data, while still verifying that a mux
        // reader on the saturated side observes the closure.
        std::atomic<bool> read_failed{false};
        const bool read_spawned = ppp::coroutines::YieldContext::Spawn(nullptr, *context_, server_strand_.get(),
            [server_mux, &read_failed](ppp::coroutines::YieldContext& y) noexcept {
                int length = 0;
                read_failed.store(!server_mux->Read(y, length));
            });

        // Wait for the writer coroutine to finish before returning so it can never
        // outlive this scope (its captured locals are on this frame).
        return read_spawned && closed && WaitFor([&]() { return read_failed.load(); }) &&
            WaitFor([&]() { return write_done.load(); }) && !server_secure_->IsSecureReady();
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

private:
    std::shared_ptr<asio::io_context> context_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    ppp::transmissions::ITransmission::StrandPtr client_strand_, server_strand_;
    std::shared_ptr<ppp::transmissions::ITransmission> client_, server_;
    std::shared_ptr<ppp::transmissions::IWebsocketTransmission> client_ws_, server_ws_;
    std::shared_ptr<ppp::transmissions::ISslWebsocketTransmission> client_ssl_, server_ssl_;
    std::shared_ptr<MultipathTransmission> client_secure_, server_secure_;
    std::shared_ptr<ppp::transmissions::ITransmission> client_mux_, server_mux_;
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

BOOST_AUTO_TEST_CASE(dispose_wakes_main_and_mux_readers) {
    TcpMultipathPair pair;
    BOOST_REQUIRE(pair.Run());
    BOOST_TEST(pair.DisposeWakesBothLogicalReaders());
}

BOOST_AUTO_TEST_CASE(mux_receive_queue_saturation_closes_the_carrier) {
    TcpMultipathPair pair;
    BOOST_REQUIRE(pair.Run());
    BOOST_TEST(pair.MuxQueueSaturationClosesCarrier());
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
