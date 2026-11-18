#include <ppp/cryptography/noise/MultipathSecureChannel.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#include <openssl/params.h>
#else
#include <openssl/hmac.h>
#endif

#include <cstring>
#include <limits>
#include <utility>

namespace ppp::cryptography::noise {
namespace {

constexpr std::uint8_t Domain[] = "openppp2-multipath-secure-channel-v1";
constexpr std::uint8_t ExportDomain[] = "openppp2-multipath-export-v1";
constexpr std::size_t MaxIdentity = 255;
constexpr std::size_t MaxCapabilities = 1024;
constexpr std::size_t TagSize = 16;
constexpr std::size_t HeaderSize = 4;

struct Segment final { const std::uint8_t* data; std::size_t size; };

bool IsValidUtf8(const std::string& value) noexcept {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
    std::size_t position = 0;
    while (position < value.size()) {
        const std::uint8_t first = bytes[position++];
        if (first <= 0x7f) continue;

        std::uint32_t codepoint = 0;
        std::size_t continuation_count = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1f;
            continuation_count = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0f;
            continuation_count = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07;
            continuation_count = 3;
            minimum = 0x10000;
        } else {
            return false;
        }

        if (continuation_count > value.size() - position) return false;
        for (std::size_t i = 0; i < continuation_count; ++i) {
            const std::uint8_t next = bytes[position++];
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    }
    return true;
}

bool HmacSha256(const std::uint8_t* key, std::size_t key_size,
                const Segment* segments, std::size_t count, Bytes32& output) noexcept {
    bool ok = key != nullptr && segments != nullptr;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    EVP_MAC* mac = ok ? EVP_MAC_fetch(nullptr, "HMAC", nullptr) : nullptr;
    EVP_MAC_CTX* ctx = mac != nullptr ? EVP_MAC_CTX_new(mac) : nullptr;
    char digest[] = "SHA256";
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_end(),
    };
    ok = ctx != nullptr && EVP_MAC_init(ctx, key, key_size, parameters) > 0;
    for (std::size_t i = 0; ok && i < count; ++i)
        ok = segments[i].size == 0 || (segments[i].data != nullptr &&
            EVP_MAC_update(ctx, segments[i].data, segments[i].size) > 0);
    std::size_t length = 0;
    ok = ok && EVP_MAC_final(ctx, output.data(), &length, output.size()) > 0 && length == output.size();
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
#else
    HMAC_CTX* ctx = ok ? HMAC_CTX_new() : nullptr;
    unsigned int length = 0;
    ok = ctx != nullptr && HMAC_Init_ex(ctx, key, static_cast<int>(key_size), EVP_sha256(), nullptr) > 0;
    for (std::size_t i = 0; ok && i < count; ++i)
        ok = segments[i].size == 0 || (segments[i].data != nullptr &&
            HMAC_Update(ctx, segments[i].data, segments[i].size) > 0);
    ok = ok && HMAC_Final(ctx, output.data(), &length) > 0 && length == output.size();
    HMAC_CTX_free(ctx);
#endif
    if (!ok) OPENSSL_cleanse(output.data(), output.size());
    return ok;
}

void Put16(std::vector<std::uint8_t>& out, std::size_t n) {
    out.push_back(static_cast<std::uint8_t>(n >> 8));
    out.push_back(static_cast<std::uint8_t>(n));
}
void Put32(std::uint8_t* out, std::uint32_t n) noexcept {
    out[0] = static_cast<std::uint8_t>(n >> 24);
    out[1] = static_cast<std::uint8_t>(n >> 16);
    out[2] = static_cast<std::uint8_t>(n >> 8);
    out[3] = static_cast<std::uint8_t>(n);
}
std::uint32_t Get32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
        (static_cast<std::uint32_t>(p[1]) << 16) |
        (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
void Put64(std::uint8_t* out, std::uint64_t n) noexcept {
    for (std::size_t i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(n >> (56 - 8 * i));
}

bool Derive(const Secret32& input, const char* label, const Bytes32& transcript,
            Bytes32& output) noexcept {
    const std::uint8_t label_length = static_cast<std::uint8_t>(std::strlen(label));
    const Segment segments[] = {
        {Domain, sizeof(Domain) - 1}, {&label_length, 1},
        {reinterpret_cast<const std::uint8_t*>(label), label_length},
        {transcript.data(), transcript.size()},
    };
    return HmacSha256(input.data(), input.size(), segments,
        sizeof(segments) / sizeof(segments[0]), output);
}

bool Aead(bool encrypt, const Secret32& key, std::uint64_t sequence,
          const std::uint8_t header[4], const std::uint8_t* input,
          std::size_t input_size, std::uint8_t* output, std::uint8_t tag[16]) noexcept {
    std::uint8_t nonce[12]{};
    std::uint8_t aad[12]{};
    std::memcpy(aad, header, 4);
    Put64(aad + 4, sequence);
    Put64(nonce + 4, sequence);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int written = 0, final_size = 0;
    bool ok = ctx != nullptr;
    if (encrypt) {
        ok = ok && EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, sizeof(nonce), nullptr) == 1 &&
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce) == 1 &&
            EVP_EncryptUpdate(ctx, nullptr, &written, aad, sizeof(aad)) == 1 &&
            EVP_EncryptUpdate(ctx, output, &written, input, static_cast<int>(input_size)) == 1 &&
            EVP_EncryptFinal_ex(ctx, output + written, &final_size) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, TagSize, tag) == 1;
    } else {
        ok = ok && EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, sizeof(nonce), nullptr) == 1 &&
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce) == 1 &&
            EVP_DecryptUpdate(ctx, nullptr, &written, aad, sizeof(aad)) == 1 &&
            EVP_DecryptUpdate(ctx, output, &written, input, static_cast<int>(input_size)) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, TagSize, tag) == 1 &&
            EVP_DecryptFinal_ex(ctx, output + written, &final_size) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(nonce, sizeof(nonce));
    OPENSSL_cleanse(aad, sizeof(aad));
    return ok && static_cast<std::size_t>(written + final_size) == input_size;
}

}

MultipathSecureChannel::~MultipathSecureChannel() noexcept { Clear(); }
MultipathSecureChannel::MultipathSecureChannel(MultipathSecureChannel&& other) noexcept { *this = std::move(other); }
MultipathSecureChannel& MultipathSecureChannel::operator=(MultipathSecureChannel&& other) noexcept {
    if (this != &other) {
        Clear(); role_ = other.role_; parameters_ = std::move(other.parameters_); handshake_ = std::move(other.handshake_);
        client_to_server_ = std::move(other.client_to_server_);
        server_to_client_ = std::move(other.server_to_client_); exporter_ = std::move(other.exporter_);
        deterministic_private_key_ = std::move(other.deterministic_private_key_);
        send_sequence_ = other.send_sequence_; receive_sequence_ = other.receive_sequence_;
        started_ = other.started_; ready_ = other.ready_; failed_ = other.failed_;
        other.Clear();
    }
    return *this;
}

bool MultipathSecureChannel::BuildPrologue(const MultipathHandshakeParameters& p,
                                            std::vector<std::uint8_t>& out) noexcept {
    out.clear();
    if (p.client_identity.empty() || p.client_identity.size() > MaxIdentity ||
        !IsValidUtf8(p.client_identity) ||
        p.server_identity.empty() || p.server_identity.size() > MaxIdentity ||
        !IsValidUtf8(p.server_identity) ||
        p.key_id.empty() || p.key_id.size() > NoiseKeyIdMaxSize || !IsValidUtf8(p.key_id) ||
        p.capabilities.size() > MaxCapabilities) return false;
    try {
        std::vector<std::uint8_t> v;
        v.reserve(sizeof(Domain) + 2 + 2 + p.client_identity.size() + 2 + p.server_identity.size() +
                   2 + p.key_id.size() + 2 + p.capabilities.size());
        v.insert(v.end(), std::begin(Domain), std::end(Domain) - 1);
        Put16(v, MultipathSecureProtocolVersion);
        auto field = [&v](const std::uint8_t* b, std::size_t n) {
            Put16(v, n);
            if (n != 0) v.insert(v.end(), b, b + n);
        };
        field(reinterpret_cast<const std::uint8_t*>(p.client_identity.data()), p.client_identity.size());
        field(reinterpret_cast<const std::uint8_t*>(p.server_identity.data()), p.server_identity.size());
        field(reinterpret_cast<const std::uint8_t*>(p.key_id.data()), p.key_id.size());
        field(p.capabilities.data(), p.capabilities.size());
        out.swap(v);
        return true;
    } catch (...) { out.clear(); return false; }
}

bool MultipathSecureChannel::Start(HandshakeRole role, Secret32&& psk,
    const MultipathHandshakeParameters& params, std::vector<std::uint8_t>& outbound) noexcept {
    outbound.clear();
    if (started_ || failed_ || !psk.IsSet() ||
        (role != HandshakeRole::NetworkClientInitiator && role != HandshakeRole::NetworkServerResponder))
        return Fail();
    std::vector<std::uint8_t> prologue;
    if (!BuildPrologue(params, prologue)) return Fail();
    role_ = role;
    try { parameters_ = params; }
    catch (...) { return Fail(); }
    handshake_ = NoisePskHandshake(role, std::move(psk), prologue);
    OPENSSL_cleanse(prologue.data(), prologue.size());
    if (deterministic_private_key_.IsSet() &&
        !handshake_.SetDeterministicEphemeralPrivateKeyForTesting(std::move(deterministic_private_key_))) return Fail();
    if (!handshake_.IsValid()) return Fail();
    started_ = true;
    if (role_ == HandshakeRole::NetworkClientInitiator && !handshake_.WriteMessage1(outbound)) return Fail();
    return true;
}

bool MultipathSecureChannel::SetDeterministicEphemeralPrivateKeyForTesting(
    Secret32&& key) noexcept {
    if (started_ || failed_ || deterministic_private_key_.IsSet() || !key.IsSet()) return false;
    deterministic_private_key_ = std::move(key);
    return true;
}

bool MultipathSecureChannel::ProcessHandshakeMessage(const std::uint8_t* message,
    std::size_t size, std::vector<std::uint8_t>& outbound) noexcept {
    outbound.clear();
    if (!started_ || ready_ || failed_ || message == nullptr || size != NoiseHandshakeMessageSize) return Fail();
    const bool ok = role_ == HandshakeRole::NetworkServerResponder
        ? handshake_.ReadMessage1(message, size) && handshake_.WriteMessage2(outbound)
        : handshake_.ReadMessage2(message, size);
    if (!ok || !FinishHandshake(outbound)) return Fail();
    return true;
}

bool MultipathSecureChannel::FinishHandshake(std::vector<std::uint8_t>&) noexcept {
    NoisePskHandshakeResult result;
    if (!handshake_.TakeResult(result)) return false;
    Bytes32 transcript{};
    Secret32 base;
    Bytes32 c2s{}, s2c{}, exported{};
    const bool ok = result.GetHandshakeHash(transcript) && result.TakeExporterSecret(base) &&
        Derive(base, "client-to-server", transcript, c2s) &&
        Derive(base, "server-to-client", transcript, s2c) &&
        Derive(base, "exporter", transcript, exported);
    if (ok) {
        client_to_server_.Assign(std::move(c2s));
        server_to_client_.Assign(std::move(s2c));
        exporter_.Assign(std::move(exported));
    }
    OPENSSL_cleanse(c2s.data(), c2s.size());
    OPENSSL_cleanse(s2c.data(), s2c.size());
    OPENSSL_cleanse(exported.data(), exported.size());
    OPENSSL_cleanse(transcript.data(), transcript.size());
    if (!ok) return false;
    ready_ = true;
    return true;
}

bool MultipathSecureChannel::IsReady() const noexcept { return ready_ && !failed_; }
bool MultipathSecureChannel::IsFailed() const noexcept { return failed_; }

bool MultipathSecureChannel::Export(const std::uint8_t* label, std::size_t size,
                                    Secret32& output) const noexcept {
    if (output.IsSet()) return false;
    Bytes32 value{};
    const bool ok = Export(label, size, nullptr, 0, value.data(), value.size());
    if (ok) output.Assign(std::move(value));
    OPENSSL_cleanse(value.data(), value.size());
    return ok;
}

bool MultipathSecureChannel::Export(const std::uint8_t* label, std::size_t label_size,
    const std::uint8_t* context, std::size_t context_size, std::uint8_t* output,
    std::size_t output_size) const noexcept {
    constexpr std::size_t MaxContext = 4096;
    constexpr std::size_t MaxOutput = 255 * 32;
    if (!IsReady() || label == nullptr || label_size == 0 || label_size > 128 ||
        (context_size != 0 && context == nullptr) || context_size > MaxContext ||
        output == nullptr || output_size == 0 || output_size > MaxOutput) return false;
    std::vector<std::uint8_t> info;
    try {
        info.reserve(sizeof(ExportDomain) - 1 + 1 + label_size + 4 + context_size);
        info.insert(info.end(), std::begin(ExportDomain), std::end(ExportDomain) - 1);
        info.push_back(static_cast<std::uint8_t>(label_size));
        info.insert(info.end(), label, label + label_size);
        const std::uint32_t encoded_size = static_cast<std::uint32_t>(context_size);
        const std::uint8_t encoded_context_size[] = {
            static_cast<std::uint8_t>(encoded_size >> 24), static_cast<std::uint8_t>(encoded_size >> 16),
            static_cast<std::uint8_t>(encoded_size >> 8), static_cast<std::uint8_t>(encoded_size),
        };
        info.insert(info.end(), std::begin(encoded_context_size), std::end(encoded_context_size));
        if (context_size) info.insert(info.end(), context, context + context_size);
    } catch (...) { return false; }

    Bytes32 previous{};
    std::size_t previous_size = 0, offset = 0;
    std::uint8_t counter = 1;
    while (offset < output_size) {
        const Segment segments[] = {
            {previous.data(), previous_size}, {info.data(), info.size()}, {&counter, 1},
        };
        Bytes32 block{};
        if (!HmacSha256(exporter_.data(), exporter_.size(), segments, 3, block)) {
            OPENSSL_cleanse(previous.data(), previous.size());
            OPENSSL_cleanse(block.data(), block.size());
            OPENSSL_cleanse(info.data(), info.size());
            OPENSSL_cleanse(output, output_size);
            return false;
        }
        previous = block;
        previous_size = block.size();
        const std::size_t take = std::min(block.size(), output_size - offset);
        std::memcpy(output + offset, block.data(), take);
        offset += take;
        OPENSSL_cleanse(block.data(), block.size());
        ++counter;
    }
    OPENSSL_cleanse(previous.data(), previous.size());
    OPENSSL_cleanse(info.data(), info.size());
    return true;
}

bool MultipathSecureChannel::Seal(std::uint8_t type, const std::uint8_t* plain,
    std::size_t size, std::vector<std::uint8_t>& record) noexcept {
    record.clear();
    if (!IsReady() || size > MultipathRecordMaxPlaintext || (size && plain == nullptr) ||
        send_sequence_ == std::numeric_limits<std::uint64_t>::max()) return Fail();
    try {
        const std::uint32_t cipher_size = static_cast<std::uint32_t>(size + 1 + TagSize);
        record.resize(HeaderSize + cipher_size);
        Put32(record.data(), cipher_size);
        std::vector<std::uint8_t> inner(size + 1);
        inner[0] = type;
        if (size) std::memcpy(inner.data() + 1, plain, size);
        Secret32& key = role_ == HandshakeRole::NetworkClientInitiator ? client_to_server_ : server_to_client_;
        const bool ok = Aead(true, key, send_sequence_, record.data(), inner.data(), inner.size(),
                             record.data() + HeaderSize, record.data() + HeaderSize + inner.size());
        OPENSSL_cleanse(inner.data(), inner.size());
        if (!ok) { record.clear(); return Fail(); }
        ++send_sequence_;
        return true;
    } catch (...) { record.clear(); return Fail(); }
}

bool MultipathSecureChannel::Open(const std::uint8_t* record, std::size_t size,
    std::uint8_t& type, std::vector<std::uint8_t>& plain) noexcept {
    plain.clear(); type = 0;
    if (!IsReady() || record == nullptr || size < HeaderSize + TagSize + 1) return Fail();
    const std::uint32_t cipher_size = Get32(record);
    if (cipher_size != size - HeaderSize || cipher_size > MultipathRecordMaxPlaintext + 1 + TagSize ||
        cipher_size < TagSize + 1 || receive_sequence_ == std::numeric_limits<std::uint64_t>::max()) return Fail();
    const std::size_t encrypted_size = cipher_size - TagSize;
    try {
        std::vector<std::uint8_t> inner(encrypted_size);
        Secret32& key = role_ == HandshakeRole::NetworkClientInitiator ? server_to_client_ : client_to_server_;
        auto* tag = const_cast<std::uint8_t*>(record + HeaderSize + encrypted_size);
        const bool ok = Aead(false, key, receive_sequence_, record, record + HeaderSize,
                             encrypted_size, inner.data(), tag);
        if (!ok) { OPENSSL_cleanse(inner.data(), inner.size()); return Fail(); }
        type = inner[0]; plain.assign(inner.begin() + 1, inner.end());
        OPENSSL_cleanse(inner.data(), inner.size());
        ++receive_sequence_;
        return true;
    } catch (...) { plain.clear(); return Fail(); }
}

bool MultipathSecureChannel::Fail() noexcept { Clear(); failed_ = true; return false; }
void MultipathSecureChannel::Clear() noexcept {
    handshake_.Clear(); client_to_server_.Clear(); server_to_client_.Clear(); exporter_.Clear();
    deterministic_private_key_.Clear();
    send_sequence_ = receive_sequence_ = 0; started_ = ready_ = false; failed_ = false;
}

}
