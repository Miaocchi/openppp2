#pragma once

#include <ppp/transmissions/ITransmission.h>
#include <ppp/cryptography/noise/MultipathSecureChannel.h>

#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace ppp::transmissions {

/** End-to-end authenticated record adapter around an already-established carrier. */
class MultipathTransmission final : public ITransmission {
public:
    using SecureParameters = ppp::cryptography::noise::MultipathHandshakeParameters;
    using SecureRole = ppp::cryptography::noise::HandshakeRole;
    using SecureChannel = ppp::cryptography::noise::MultipathSecureChannel;
    using Secret32 = ppp::cryptography::noise::Secret32;
    using ControlRecordHandler = ppp::function<bool(std::uint8_t,
        const std::shared_ptr<const std::vector<std::uint8_t>>&)>;

    static constexpr std::uint8_t PrefaceMagic[4] = { 'M', 'P', 'T', 'H' };
    static constexpr std::uint16_t PrefaceVersion = ppp::cryptography::noise::MultipathSecureProtocolVersion;
    static constexpr std::uint8_t ApplicationRecord = 0;
    static constexpr std::uint8_t ControlRecord = 1;
    static constexpr std::uint8_t MuxRecord = 2;

    MultipathTransmission(const std::shared_ptr<ITransmission>& carrier,
        SecureParameters parameters, Secret32&& psk, SecureRole role,
        const std::shared_ptr<Byte>& first_handshake_frame = nullptr,
        int first_handshake_frame_length = 0) noexcept;
    ~MultipathTransmission() noexcept override;

    static bool IsPreface(const Byte* data, int length, bool& supported_version) noexcept;
    static bool ParsePreface(const Byte* data, int length, bool& supported_version,
        ppp::string& key_id, std::size_t& message_offset) noexcept;

    /** Starts the inner Noise exchange using network-role names. */
    bool StartSecureClient(YieldContext& y) noexcept;
    bool StartSecureServer(YieldContext& y) noexcept;
    bool IsSecureReady() const noexcept;
    bool IsApplicationPumpStopped() const noexcept {
        return !application_pump_started_.load(std::memory_order_acquire) ||
            application_pump_stopped_.load(std::memory_order_acquire);
    }
    bool WriteSecureRecord(YieldContext& y, std::uint8_t type,
        const void* data, std::size_t length) noexcept;
    bool WriteSecureRecord(std::uint8_t type, const void* data, std::size_t length,
        const AsynchronousWriteCallback& callback) noexcept;
    bool ReadSecureRecord(YieldContext& y, std::uint8_t& type,
        std::vector<std::uint8_t>& data) noexcept;
    /** Permanently closes the pre-join record read API before attaching VMUX. */
    bool BeginApplicationIO() noexcept;
    /** Creates an independent VMUX byte stream over this authenticated carrier. */
    std::shared_ptr<ITransmission> CreateMuxSubchannel() noexcept;
    bool SetControlRecordHandler(const ControlRecordHandler& handler) noexcept;
    void ClearControlRecordHandler() noexcept;

    Int128 HandshakeClient(YieldContext& y, bool& mux) noexcept override;
    bool HandshakeServer(YieldContext& y, const Int128& session_id, bool mux) noexcept override;
    void Dispose() noexcept override;
    bool ShiftToScheduler() noexcept override { return false; }
    boost::asio::ip::tcp::endpoint GetRemoteEndPoint() noexcept override;
    AuthenticatedCarrierKind GetAuthenticatedCarrierKind() const noexcept override;
    AuthenticatedCarrierMethod GetAuthenticatedCarrierMethod() const noexcept override {
        return AuthenticatedCarrierMethod::MultipathNoiseV1;
    }
    bool IsAuthenticatedCarrierBindingActive() const noexcept override;
    bool HasAuthenticatedSessionExporter() const noexcept override;
    bool ExportAuthenticatedSessionKey(const char* label, const std::uint8_t* context,
        std::size_t context_length, std::uint8_t* output, std::size_t output_length) noexcept override;

protected:
    std::shared_ptr<Byte> DoReadBytes(YieldContext& y, int length) noexcept override;
    bool DoWriteBytes(std::shared_ptr<Byte> packet, int offset, int packet_length,
        const AsynchronousWriteBytesCallback& cb) noexcept override;

private:
    friend class MultipathLogicalTransmission;
    bool StartClientHandshake(YieldContext& y) noexcept;
    bool StartServerHandshake(YieldContext& y) noexcept;
    bool SendHandshakeFrame(YieldContext& y, const std::vector<std::uint8_t>& message) noexcept;
    bool ReceiveHandshakeFrame(YieldContext& y, std::vector<std::uint8_t>& message) noexcept;
    bool ReadRecord(YieldContext& y, std::uint8_t& type, std::vector<std::uint8_t>& data) noexcept;
    bool DispatchControlRecord(std::uint8_t type, std::vector<std::uint8_t>&& data) noexcept;
    bool StartApplicationPump() noexcept;
    void ApplicationPump(YieldContext& y) noexcept;
    void CloseLogicalChannel(std::uint8_t type) noexcept;
    std::shared_ptr<Byte> ReadLogicalBytes(YieldContext& y, int length, std::uint8_t type) noexcept;
    bool WriteLogicalBytes(std::shared_ptr<Byte> packet, int offset, int packet_length,
        std::uint8_t type, const AsynchronousWriteBytesCallback& cb) noexcept;
    bool SubmitSealedRecord(std::uint8_t type, const std::uint8_t* data,
        std::size_t length, const AsynchronousWriteCallback& callback) noexcept;

    struct LogicalChannel final {
        std::deque<std::vector<std::uint8_t>> records;
        std::size_t queued_bytes = 0;
        std::vector<std::uint8_t> plaintext;
        std::size_t plaintext_offset = 0;
        YieldContext::Anchor waiter;
        bool closed = false;
        bool reading = false;
    };

    const std::shared_ptr<ITransmission> carrier_;
    SecureParameters parameters_;
    Secret32 psk_;
    SecureChannel secure_;
    SecureRole role_;
    std::shared_ptr<Byte> first_handshake_frame_;
    int first_handshake_frame_length_ = 0;
    std::vector<std::uint8_t> read_plaintext_;
    std::size_t read_plaintext_offset_ = 0;
    mutable std::mutex secure_mutex_;
    mutable std::mutex record_write_mutex_;
    std::atomic_bool reader_active_{false};
    std::atomic_bool application_io_started_{false};
    std::atomic_bool application_pump_started_{false};
    std::atomic_bool application_pump_stopped_{false};
    mutable std::mutex channels_mutex_;
    LogicalChannel main_channel_;
    LogicalChannel mux_channel_;
    std::weak_ptr<ITransmission> mux_subchannel_;
    bool mux_subchannel_created_ = false;
    mutable std::mutex control_mutex_;
    mutable std::mutex control_write_mutex_;
    ControlRecordHandler control_handler_;
    std::deque<std::shared_ptr<const std::vector<std::uint8_t>>> pending_control_records_;
    std::size_t pending_control_bytes_ = 0;
    std::size_t pending_control_writes_ = 0;
    std::size_t pending_control_write_bytes_ = 0;
    bool secure_started_ = false;
    std::atomic_bool disposed_{false};
};

}
