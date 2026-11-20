#pragma once

#include <ppp/cryptography/noise/NoisePsk.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ppp::cryptography::noise {

struct MultipathSecureChannelTestAccess;

inline constexpr std::size_t MultipathRecordMaxPlaintext = 16 * 1024;
inline constexpr std::size_t MultipathRecordOverhead = 4 + 1 + 16;
inline constexpr std::uint16_t MultipathSecureProtocolVersion = 1;

struct MultipathHandshakeParameters final {
    std::string client_identity;
    std::string server_identity;
    std::string key_id;
    std::vector<std::uint8_t> capabilities;
};

/** Noise PSK session and ordered ChaCha20-Poly1305 record layer for multipath. */
class MultipathSecureChannel final {
public:
    MultipathSecureChannel() noexcept = default;
    ~MultipathSecureChannel() noexcept;
    MultipathSecureChannel(const MultipathSecureChannel&) = delete;
    MultipathSecureChannel& operator=(const MultipathSecureChannel&) = delete;
    MultipathSecureChannel(MultipathSecureChannel&&) noexcept;
    MultipathSecureChannel& operator=(MultipathSecureChannel&&) noexcept;

    /** Client emits message 1; server waits for it. */
    bool Start(HandshakeRole role, Secret32&& psk,
               const MultipathHandshakeParameters& parameters,
               std::vector<std::uint8_t>& outbound) noexcept;
    bool SetDeterministicEphemeralPrivateKeyForTesting(Secret32&& private_key) noexcept;
    /** Consumes exactly one Noise handshake message and emits message 2 on server. */
    bool ProcessHandshakeMessage(const std::uint8_t* message, std::size_t size,
                                 std::vector<std::uint8_t>& outbound) noexcept;
    bool IsReady() const noexcept;
    bool IsFailed() const noexcept;
    HandshakeRole LocalRole() const noexcept { return role_; }
    const MultipathHandshakeParameters& AuthenticatedParameters() const noexcept { return parameters_; }
    bool Export(const std::uint8_t* label, std::size_t label_size,
                Secret32& output) const noexcept;
    bool Export(const std::uint8_t* label, std::size_t label_size,
                const std::uint8_t* context, std::size_t context_size,
                std::uint8_t* output, std::size_t output_size) const noexcept;

    /** Wire record = uint32_be(ciphertext length) || ciphertext(type || payload) || tag. */
    bool Seal(std::uint8_t type, const std::uint8_t* plaintext, std::size_t size,
              std::vector<std::uint8_t>& record) noexcept;
    bool Open(const std::uint8_t* record, std::size_t size, std::uint8_t& type,
              std::vector<std::uint8_t>& plaintext) noexcept;
    void Clear() noexcept;

    /** Stable v1 prologue encoder, exposed for protocol vector tests. */
    static bool BuildPrologue(const MultipathHandshakeParameters& parameters,
                              std::vector<std::uint8_t>& output) noexcept;

private:
    friend struct MultipathSecureChannelTestAccess;
    bool FinishHandshake(std::vector<std::uint8_t>& outbound) noexcept;
    bool Fail() noexcept;

    HandshakeRole role_ = HandshakeRole::NetworkClientInitiator;
    MultipathHandshakeParameters parameters_;
    NoisePskHandshake handshake_{HandshakeRole::NetworkClientInitiator, Secret32{}, {}};
    Secret32 client_to_server_;
    Secret32 server_to_client_;
    Secret32 exporter_;
    std::uint64_t send_sequence_ = 0;
    std::uint64_t receive_sequence_ = 0;
    bool started_ = false;
    bool ready_ = false;
    bool failed_ = false;
    Secret32 deterministic_private_key_;
};

}
