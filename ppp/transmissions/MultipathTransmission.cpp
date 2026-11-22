#include <ppp/transmissions/MultipathTransmission.h>
#include <ppp/configurations/AppConfiguration.h>
#include <ppp/diagnostics/Telemetry.h>
#include <ppp/transmissions/ITcpipTransmission.h>
#include <ppp/transmissions/TransmissionFrameLimits.h>

#include <algorithm>
#include <cstring>
#include <openssl/crypto.h>

namespace ppp::transmissions {
namespace {
constexpr std::size_t PrefaceSize = 7;
constexpr std::size_t MaxLogicalRecords = 128;
constexpr std::size_t MaxLogicalBytes = 4 * 1024 * 1024;
std::uint16_t Read16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}
}

class MultipathLogicalTransmission final : public ITransmission {
public:
    MultipathLogicalTransmission(const std::shared_ptr<MultipathTransmission>& owner,
        std::uint8_t record_type) noexcept
        : ITransmission(owner ? owner->GetContext() : ContextPtr(),
            owner ? owner->GetStrand() : StrandPtr(),
            owner ? owner->GetConfiguration() : AppConfigurationPtr())
        , owner_(owner), record_type_(record_type) {}

    void Dispose() noexcept override {
        if (!disposed_.exchange(true, std::memory_order_acq_rel)) {
            if (auto owner = owner_.lock()) owner->CloseLogicalChannel(record_type_);
            ITransmission::Dispose();
        }
    }

    boost::asio::ip::tcp::endpoint GetRemoteEndPoint() noexcept override {
        auto owner = owner_.lock();
        return owner ? owner->GetRemoteEndPoint() : boost::asio::ip::tcp::endpoint();
    }

    bool ShiftToScheduler() noexcept override { return false; }

    AuthenticatedCarrierKind GetAuthenticatedCarrierKind() const noexcept override {
        auto owner = owner_.lock();
        return owner ? owner->GetAuthenticatedCarrierKind() : AuthenticatedCarrierKind::None;
    }

    // The VMUX/scheduler holds this logical channel, so pacing must be
    // forwarded to the one raw carrier that actually writes the bytes.
    bool SupportsPacingRate() const noexcept override {
        auto owner = owner_.lock();
        return owner && owner->SupportsPacingRate();
    }

    std::uint64_t GetPacingRateBytesPerSecond() const noexcept override {
        auto owner = owner_.lock();
        return owner ? owner->GetPacingRateBytesPerSecond() : 0;
    }

    TransmissionPacingResult ApplyPacingRate(std::uint64_t bytes_per_second) noexcept override {
        auto owner = owner_.lock();
        return owner ? owner->ApplyPacingRate(bytes_per_second)
            : TransmissionPacingResult::Unsupported;
    }

    AuthenticatedCarrierMethod GetAuthenticatedCarrierMethod() const noexcept override {
        return AuthenticatedCarrierMethod::MultipathNoiseV1;
    }

    bool IsAuthenticatedCarrierBindingActive() const noexcept override {
        auto owner = owner_.lock();
        return owner && owner->IsAuthenticatedCarrierBindingActive();
    }

    bool HasAuthenticatedSessionExporter() const noexcept override {
        auto owner = owner_.lock();
        return owner && owner->HasAuthenticatedSessionExporter();
    }

    bool ExportAuthenticatedSessionKey(const char* label, const std::uint8_t* context,
        std::size_t context_length, std::uint8_t* output, std::size_t output_length) noexcept override {
        auto owner = owner_.lock();
        if (!owner || !label) return false;
        try {
            const std::string scoped_label = std::string("multipath-vmux-subchannel-v1:") + label;
            return owner->ExportAuthenticatedSessionKey(scoped_label.c_str(), context,
                context_length, output, output_length);
        } catch (...) { return false; }
    }

protected:
    std::shared_ptr<Byte> DoReadBytes(YieldContext& y, int length) noexcept override {
        auto owner = owner_.lock();
        return !disposed_ && owner ? owner->ReadLogicalBytes(y, length, record_type_) : nullptr;
    }

    bool DoWriteBytes(std::shared_ptr<Byte> packet, int offset, int packet_length,
        const AsynchronousWriteBytesCallback& cb) noexcept override {
        auto owner = owner_.lock();
        return !disposed_ && owner && owner->WriteLogicalBytes(
            std::move(packet), offset, packet_length, record_type_, cb);
    }

private:
    std::weak_ptr<MultipathTransmission> owner_;
    const std::uint8_t record_type_;
    std::atomic_bool disposed_{false};
};

MultipathTransmission::MultipathTransmission(const std::shared_ptr<ITransmission>& carrier,
    SecureParameters parameters, Secret32&& psk, SecureRole role,
    const std::shared_ptr<Byte>& first_handshake_frame, int first_handshake_frame_length) noexcept
    : ITransmission(carrier ? carrier->GetContext() : ContextPtr(),
        carrier ? carrier->GetStrand() : StrandPtr(),
        carrier ? carrier->GetConfiguration() : AppConfigurationPtr())
    , carrier_(carrier)
    , parameters_(std::move(parameters))
    , psk_(std::move(psk))
    , role_(role)
    , first_handshake_frame_(first_handshake_frame)
    , first_handshake_frame_length_(first_handshake_frame_length) {
    SetMaxPendingItems(128);
    SetMaxPendingBytes(4 * 1024 * 1024);
    if (!carrier_ || !psk_.IsSet()) {
        disposed_ = true;
        return;
    }
    if (auto tcp = dynamic_cast<ITcpipTransmission*>(carrier_.get())) tcp->LimitMultipathUnsentBytes();
    secure_started_ = false;
}

MultipathTransmission::~MultipathTransmission() noexcept {
    std::lock_guard<std::mutex> lock(secure_mutex_);
    secure_.Clear();
    OPENSSL_cleanse(read_plaintext_.data(), read_plaintext_.size());
    OPENSSL_cleanse(main_channel_.plaintext.data(), main_channel_.plaintext.size());
    OPENSSL_cleanse(mux_channel_.plaintext.data(), mux_channel_.plaintext.size());
}

bool MultipathTransmission::IsPreface(const Byte* data, int length, bool& supported_version) noexcept {
    supported_version = false;
    if (!data || length < static_cast<int>(sizeof(PrefaceMagic)) ||
        std::memcmp(data, PrefaceMagic, sizeof(PrefaceMagic)) != 0) return false;
    if (length >= static_cast<int>(sizeof(PrefaceMagic) + 2)) {
        supported_version = Read16(reinterpret_cast<const std::uint8_t*>(data) + sizeof(PrefaceMagic)) == PrefaceVersion;
    }
    return true;
}

bool MultipathTransmission::ParsePreface(const Byte* data, int length, bool& supported_version,
    ppp::string& key_id, std::size_t& message_offset) noexcept {
    key_id.clear();
    message_offset = 0;
    if (!IsPreface(data, length, supported_version)) return false;
    if (length < static_cast<int>(PrefaceSize)) return true;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
    const std::size_t key_length = bytes[6];
    if (key_length == 0 || key_length > ppp::cryptography::noise::NoiseKeyIdMaxSize ||
        static_cast<std::size_t>(length) < PrefaceSize + key_length) return true;
    try {
        key_id.assign(reinterpret_cast<const char*>(bytes + PrefaceSize), key_length);
    } catch (...) { key_id.clear(); return true; }
    message_offset = PrefaceSize + key_length;
    return true;
}

bool MultipathTransmission::SendHandshakeFrame(YieldContext& y,
    const std::vector<std::uint8_t>& message) noexcept {
    if (!carrier_ || message.empty() || parameters_.key_id.empty() ||
        parameters_.key_id.size() > ppp::cryptography::noise::NoiseKeyIdMaxSize ||
        message.size() > PPP_BUFFER_SIZE - PrefaceSize - parameters_.key_id.size()) return false;
    std::vector<std::uint8_t> frame;
    try {
        frame.reserve(PrefaceSize + parameters_.key_id.size() + message.size());
        frame.insert(frame.end(), std::begin(PrefaceMagic), std::end(PrefaceMagic));
        frame.push_back(static_cast<std::uint8_t>(PrefaceVersion >> 8));
        frame.push_back(static_cast<std::uint8_t>(PrefaceVersion));
        frame.push_back(static_cast<std::uint8_t>(parameters_.key_id.size()));
        frame.insert(frame.end(), parameters_.key_id.begin(), parameters_.key_id.end());
        frame.insert(frame.end(), message.begin(), message.end());
    } catch (...) { return false; }
    return carrier_->Write(y, frame.data(), static_cast<int>(frame.size()));
}

bool MultipathTransmission::ReceiveHandshakeFrame(YieldContext& y,
    std::vector<std::uint8_t>& message) noexcept {
    int length = 0;
    auto packet = carrier_->Read(y, length);
    bool version_ok = false;
    ppp::string key_id;
    std::size_t message_offset = 0;
    if (!packet || !ParsePreface(packet.get(), length, version_ok, key_id, message_offset) ||
        !version_ok || key_id.size() != parameters_.key_id.size() ||
        std::memcmp(key_id.data(), parameters_.key_id.data(), key_id.size()) != 0 ||
        length <= static_cast<int>(message_offset)) return false;
    try {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(packet.get()) + message_offset;
        message.assign(begin, begin + length - message_offset);
        return true;
    } catch (...) { return false; }
}

bool MultipathTransmission::StartClientHandshake(YieldContext& y) noexcept {
    if (!carrier_ || role_ != SecureRole::NetworkClientInitiator || disposed_) return false;
    std::vector<std::uint8_t> first;
    {
        std::lock_guard<std::mutex> lock(secure_mutex_);
        if (secure_.IsReady()) return true;
        if (secure_started_ || !secure_.Start(role_, std::move(psk_), parameters_, first)) return false;
        secure_started_ = true;
    }
    if (!SendHandshakeFrame(y, first)) return false;
    std::vector<std::uint8_t> response, unused;
    if (!ReceiveHandshakeFrame(y, response)) return false;
    std::lock_guard<std::mutex> lock(secure_mutex_);
    return secure_.ProcessHandshakeMessage(response.data(), response.size(), unused) &&
        unused.empty() && secure_.IsReady();
}

bool MultipathTransmission::StartServerHandshake(YieldContext& y) noexcept {
    if (!carrier_ || role_ != SecureRole::NetworkServerResponder || disposed_ ||
        !first_handshake_frame_ || first_handshake_frame_length_ < static_cast<int>(PrefaceSize)) return false;
    bool version_ok = false;
    ppp::string key_id;
    std::size_t message_offset = 0;
    if (!ParsePreface(first_handshake_frame_.get(), first_handshake_frame_length_,
        version_ok, key_id, message_offset) || !version_ok ||
        key_id.size() != parameters_.key_id.size() ||
        std::memcmp(key_id.data(), parameters_.key_id.data(), key_id.size()) != 0) return false;
    const auto* message = reinterpret_cast<const std::uint8_t*>(first_handshake_frame_.get()) + message_offset;
    const std::size_t message_size = static_cast<std::size_t>(first_handshake_frame_length_) - message_offset;
    std::vector<std::uint8_t> response;
    {
        std::lock_guard<std::mutex> lock(secure_mutex_);
        if (secure_started_) return secure_.IsReady();
        std::vector<std::uint8_t> none;
        if (!secure_.Start(role_, std::move(psk_), parameters_, none) || !none.empty()) return false;
        secure_started_ = true;
        if (message_size == 0 || !secure_.ProcessHandshakeMessage(message, message_size, response)) return false;
    }
    if (!SendHandshakeFrame(y, response)) return false;
    first_handshake_frame_.reset();
    first_handshake_frame_length_ = 0;
    return IsSecureReady();
}

bool MultipathTransmission::StartSecureClient(YieldContext& y) noexcept {
    if (IsSecureReady()) return true;
    return StartClientHandshake(y);
}

bool MultipathTransmission::StartSecureServer(YieldContext& y) noexcept {
    if (IsSecureReady()) return true;
    return StartServerHandshake(y);
}

bool MultipathTransmission::IsSecureReady() const noexcept {
    std::lock_guard<std::mutex> lock(secure_mutex_);
    return secure_.IsReady();
}

bool MultipathTransmission::WriteSecureRecord(YieldContext& y, std::uint8_t type,
    const void* data, std::size_t length) noexcept {
    const YieldContext::Anchor anchor = y.Arm();
    if (!anchor) return false;
    std::atomic_bool completed{false};
    std::atomic_bool succeeded{false};
    if (!WriteSecureRecord(type, data, length, [&anchor, &completed, &succeeded](bool ok) noexcept {
        succeeded.store(ok, std::memory_order_release);
        completed.store(true, std::memory_order_release);
        anchor.R();
    })) return false;
    if (!completed.load(std::memory_order_acquire)) y.Suspend(anchor);
    return succeeded.load(std::memory_order_acquire);
}

bool MultipathTransmission::WriteSecureRecord(std::uint8_t type, const void* data,
    std::size_t length, const AsynchronousWriteCallback& callback) noexcept {
    if ((!data && length) || length > ppp::cryptography::noise::MultipathRecordMaxPlaintext ||
        !carrier_ || !callback) return false;
    const bool control = type == ControlRecord;
    const std::size_t accounted_bytes = length + ppp::cryptography::noise::MultipathRecordOverhead;
    if (control) {
        std::lock_guard<std::mutex> lock(control_write_mutex_);
        if (pending_control_writes_ >= 128 || accounted_bytes > 1024 * 1024 - pending_control_write_bytes_) return false;
        ++pending_control_writes_;
        pending_control_write_bytes_ += accounted_bytes;
    }
    std::shared_ptr<MultipathTransmission> self;
    AsynchronousWriteCallback wrapped_callback;
    try {
        self = std::static_pointer_cast<MultipathTransmission>(shared_from_this());
        wrapped_callback = [self, callback, control, accounted_bytes](bool ok) noexcept {
            if (control) {
                std::lock_guard<std::mutex> lock(self->control_write_mutex_);
                --self->pending_control_writes_;
                self->pending_control_write_bytes_ -= accounted_bytes;
            }
            callback(ok);
        };
    } catch (...) {
        if (control) {
            std::lock_guard<std::mutex> lock(control_write_mutex_);
            --pending_control_writes_;
            pending_control_write_bytes_ -= accounted_bytes;
        }
        return false;
    }
    const bool accepted = SubmitSealedRecord(type,
        static_cast<const std::uint8_t*>(data), length, wrapped_callback);
    if (!accepted && control) {
        std::lock_guard<std::mutex> lock(control_write_mutex_);
        --pending_control_writes_;
        pending_control_write_bytes_ -= accounted_bytes;
    }
    return accepted;
}

bool MultipathTransmission::ReadRecord(YieldContext& y, std::uint8_t& type,
    std::vector<std::uint8_t>& data) noexcept {
    if (!carrier_) return false;
    int length = 0;
    auto packet = carrier_->Read(y, length);
    if (!packet || length <= 0 || static_cast<std::size_t>(length) >
        ppp::cryptography::noise::MultipathRecordMaxPlaintext + ppp::cryptography::noise::MultipathRecordOverhead) return false;
    std::lock_guard<std::mutex> lock(secure_mutex_);
    return secure_.IsReady() && secure_.Open(reinterpret_cast<const std::uint8_t*>(packet.get()),
        static_cast<std::size_t>(length), type, data);
}

bool MultipathTransmission::ReadSecureRecord(YieldContext& y, std::uint8_t& type,
    std::vector<std::uint8_t>& data) noexcept {
    if (application_io_started_.load(std::memory_order_acquire) ||
        reader_active_.exchange(true, std::memory_order_acq_rel)) return false;
    struct ReadGuard final { std::atomic_bool& active; ~ReadGuard() { active.store(false, std::memory_order_release); } } guard{reader_active_};
    if (application_io_started_.load(std::memory_order_acquire)) return false;
    return ReadRecord(y, type, data);
}

bool MultipathTransmission::ReadAdmissionResponse(YieldContext& y, std::uint8_t& type,
    std::vector<std::uint8_t>& data) noexcept {
    if (role_ != SecureRole::NetworkClientInitiator || !ReadSecureRecord(y, type, data)) return false;
    const bool first = !std::exchange(admission_response_seen_, true);
    if (type != BinaryFramingOffer) return type != BinaryFramingAck;
    const auto configuration = GetConfiguration();
    if (!first || !carrier_ || !configuration || configuration->key.plaintext ||
        data.size() != 1 || data.front() != BinaryFramingVersion) return false;

    // The matching ACK is the client's last Base94 write. The responder reads
    // it before emitting its first binary record (the actual admission reply).
    return WriteSecureRecord(y, BinaryFramingAck, &BinaryFramingVersion, 1) &&
        carrier_->EnableAuthenticatedMultipathBinaryFraming() &&
        ReadSecureRecord(y, type, data) && type != BinaryFramingOffer && type != BinaryFramingAck;
}

bool MultipathTransmission::NegotiateBinaryFramingServer(YieldContext& y) noexcept {
    const auto configuration = GetConfiguration();
    if (role_ != SecureRole::NetworkServerResponder || !configuration || !carrier_ || disposed_ ||
        application_io_started_.load(std::memory_order_acquire) || !IsSecureReady()) return false;
    if (configuration->key.plaintext || carrier_->IsAuthenticatedMultipathBinaryFramingEnabled()) return true;

    std::uint8_t type = 0;
    std::vector<std::uint8_t> data;
    return WriteSecureRecord(y, BinaryFramingOffer, &BinaryFramingVersion, 1) &&
        ReadSecureRecord(y, type, data) && type == BinaryFramingAck &&
        data.size() == 1 && data.front() == BinaryFramingVersion &&
        carrier_->EnableAuthenticatedMultipathBinaryFraming();
}

bool MultipathTransmission::BeginApplicationIO() noexcept {
    if (reader_active_.load(std::memory_order_acquire)) return false;
    bool expected = false;
    if (!application_io_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return application_pump_started_.load(std::memory_order_acquire);
    if (reader_active_.load(std::memory_order_acquire) || !StartApplicationPump()) {
        application_io_started_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

std::shared_ptr<ITransmission> MultipathTransmission::CreateMuxSubchannel() noexcept {
    if (!application_io_started_.load(std::memory_order_acquire) || disposed_) return nullptr;
    try {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        if (mux_channel_.closed) return nullptr;
        if (auto existing = mux_subchannel_.lock()) return existing;
        if (mux_subchannel_created_) return nullptr;
        auto self = std::static_pointer_cast<MultipathTransmission>(shared_from_this());
        auto channel = make_shared_object<MultipathLogicalTransmission>(self, MuxRecord);
        if (!channel || (carrier_->IsAuthenticatedMultipathBinaryFramingEnabled() &&
            !channel->EnableAuthenticatedMultipathBinaryFraming())) return nullptr;
        mux_subchannel_ = channel;
        mux_subchannel_created_ = true;
        return channel;
    } catch (...) { return nullptr; }
}

bool MultipathTransmission::StartApplicationPump() noexcept {
    bool expected = false;
    if (!application_pump_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return true;
    application_pump_stopped_.store(false, std::memory_order_release);
    auto context = GetContext();
    auto strand = GetStrand();
    std::shared_ptr<MultipathTransmission> self;
    try { self = std::static_pointer_cast<MultipathTransmission>(shared_from_this()); }
    catch (...) { application_pump_started_.store(false, std::memory_order_release); return false; }
    if (!context || !self || !YieldContext::Spawn(BufferAllocator.get(), *context,
        strand.get(), [self](YieldContext& y) noexcept { self->ApplicationPump(y); })) {
        application_pump_started_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void MultipathTransmission::ApplicationPump(YieldContext& y) noexcept {
    while (!disposed_) {
        std::uint8_t type = 0;
        std::vector<std::uint8_t> plaintext;
        if (!ReadRecord(y, type, plaintext) || plaintext.empty()) break;
        if (type == ControlRecord) {
            if (!DispatchControlRecord(type, std::move(plaintext))) break;
            continue;
        }
        LogicalChannel* channel = type == ApplicationRecord ? &main_channel_ :
            (type == MuxRecord ? &mux_channel_ : nullptr);
        if (!channel || !QueueLogicalRecord(y, *channel, std::move(plaintext))) break;
    }
    YieldContext::Anchor main_wake, mux_wake;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        main_channel_.closed = true;
        mux_channel_.closed = true;
        main_wake = main_channel_.waiter;
        mux_wake = mux_channel_.waiter;
        main_channel_.waiter = {};
        mux_channel_.waiter = {};
    }
    main_wake.R();
    mux_wake.R();
    if (!disposed_) Dispose();
    application_pump_stopped_.store(true, std::memory_order_release);
}

bool MultipathTransmission::QueueLogicalRecord(YieldContext& y, LogicalChannel& channel,
    std::vector<std::uint8_t>&& plaintext) noexcept {
    const std::size_t size = plaintext.size();
    for (;;) {
        YieldContext::Anchor wake, pause;
        {
            std::lock_guard<std::mutex> lock(channels_mutex_);
            if (channel.closed || disposed_) return false;
            if (channel.records.size() < MaxLogicalRecords && size <= MaxLogicalBytes - channel.queued_bytes) {
                try { channel.records.push_back(std::move(plaintext)); }
                catch (...) { return false; }
                channel.queued_bytes += size;
                wake = std::exchange(channel.waiter, {});
            } else {
                // Keep just this authenticated record pending. Stopping carrier reads
                // propagates VMUX's receive pause instead of retiring a healthy link.
                pause = y.Arm();
                if (!pause) return false;
                channel.pump_waiter = pause;
            }
        }
        if (!pause) {
            wake.R();
            return true;
        }
        ppp::telemetry::Count("multipath.receive.backpressure", 1);
        if (!y.Suspend(pause)) return false;
    }
}

void MultipathTransmission::CloseLogicalChannel(std::uint8_t type) noexcept {
    if (type != MuxRecord) return;
    YieldContext::Anchor wake, pump_wake;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        mux_channel_.closed = true;
        mux_channel_.records.clear();
        mux_channel_.queued_bytes = 0;
        OPENSSL_cleanse(mux_channel_.plaintext.data(), mux_channel_.plaintext.size());
        mux_channel_.plaintext.clear();
        mux_channel_.plaintext_offset = 0;
        wake = mux_channel_.waiter;
        mux_channel_.waiter = {};
        pump_wake = std::exchange(mux_channel_.pump_waiter, {});
    }
    wake.R();
    pump_wake.R();
}

std::shared_ptr<Byte> MultipathTransmission::ReadLogicalBytes(YieldContext& y,
    int length, std::uint8_t type) noexcept {
    if (length < 1 || length > MaxEncodedTransmissionFrame || disposed_ ||
        (type != ApplicationRecord && type != MuxRecord)) return nullptr;
    LogicalChannel& channel = type == ApplicationRecord ? main_channel_ : mux_channel_;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        if (channel.closed || channel.reading) return nullptr;
        channel.reading = true;
    }
    struct ReadGuard final {
        std::mutex& mutex;
        LogicalChannel& channel;
        ~ReadGuard() {
            std::lock_guard<std::mutex> lock(mutex);
            channel.reading = false;
        }
    } guard{channels_mutex_, channel};
    for (;;) {
        YieldContext::Anchor anchor = y.Arm();
        if (!anchor) return nullptr;
        YieldContext::Anchor pump_wake;
        bool wait = false, failed = false;
        {
            std::lock_guard<std::mutex> lock(channels_mutex_);
            if (channel.closed || disposed_) return nullptr;
            while (!channel.records.empty() &&
                channel.plaintext.size() - channel.plaintext_offset < static_cast<std::size_t>(length)) {
                if (!pump_wake) pump_wake = std::exchange(channel.pump_waiter, {});
                std::vector<std::uint8_t> record = std::move(channel.records.front());
                channel.records.pop_front();
                channel.queued_bytes -= record.size();
                if (channel.plaintext_offset > 0) {
                    channel.plaintext.erase(channel.plaintext.begin(),
                        channel.plaintext.begin() + channel.plaintext_offset);
                    channel.plaintext_offset = 0;
                }
                try { channel.plaintext.insert(channel.plaintext.end(), record.begin(), record.end()); }
                catch (...) { failed = true; break; }
            }
            if (!failed && channel.plaintext.size() - channel.plaintext_offset < static_cast<std::size_t>(length)) {
                channel.waiter = anchor;
                wait = true;
            }
        }
        pump_wake.R();
        if (failed) return nullptr;
        if (!wait) break;
        if (!y.Suspend(anchor)) return nullptr;
    }
    auto output = BufferswapAllocator::MakeByteArray(BufferAllocator, length);
    if (!output) return nullptr;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        if (channel.closed || disposed_) return nullptr;
        std::memcpy(output.get(), channel.plaintext.data() + channel.plaintext_offset,
            static_cast<std::size_t>(length));
        channel.plaintext_offset += static_cast<std::size_t>(length);
        if (channel.plaintext_offset == channel.plaintext.size()) {
            channel.plaintext.clear();
            channel.plaintext_offset = 0;
        }
    }
    return output;
}

bool MultipathTransmission::SetControlRecordHandler(const ControlRecordHandler& handler) noexcept {
    if (!handler) return false;
    std::deque<std::shared_ptr<const std::vector<std::uint8_t>>> pending;
    try {
        std::lock_guard<std::mutex> lock(control_mutex_);
        control_handler_ = handler;
        pending.swap(pending_control_records_);
        pending_control_bytes_ = 0;
    } catch (...) { return false; }
    for (const auto& record : pending) {
        bool accepted = false;
        try { accepted = handler(ControlRecord, record); }
        catch (...) { accepted = false; }
        if (!accepted) return false;
    }
    return true;
}

void MultipathTransmission::ClearControlRecordHandler() noexcept {
    std::lock_guard<std::mutex> lock(control_mutex_);
    control_handler_ = nullptr;
}

bool MultipathTransmission::DispatchControlRecord(std::uint8_t type,
    std::vector<std::uint8_t>&& data) noexcept {
    if (type != ControlRecord || data.empty() || data.size() > 64 * 1024) return false;
    std::shared_ptr<const std::vector<std::uint8_t>> record;
    try { record = std::make_shared<const std::vector<std::uint8_t>>(std::move(data)); }
    catch (...) { return false; }
    ControlRecordHandler handler;
    try {
        std::lock_guard<std::mutex> lock(control_mutex_);
        handler = control_handler_;
        if (!handler) {
            constexpr std::size_t MaxControlRecords = 128;
            constexpr std::size_t MaxControlBytes = 1024 * 1024;
            if (pending_control_records_.size() >= MaxControlRecords ||
                record->size() > MaxControlBytes - pending_control_bytes_) return false;
            pending_control_bytes_ += record->size();
            pending_control_records_.push_back(std::move(record));
            return true;
        }
    } catch (...) { return false; }
    try { return handler(type, record); }
    catch (...) { return false; }
}

Int128 MultipathTransmission::HandshakeClient(YieldContext& y, bool& mux) noexcept {
    if (!StartSecureServer(y)) return 0;
    if (!BeginApplicationIO()) return 0;
    return ITransmission::HandshakeClient(y, mux);
}

bool MultipathTransmission::HandshakeServer(YieldContext& y, const Int128& session_id, bool mux) noexcept {
    if (!StartSecureClient(y)) return false;
    if (!BeginApplicationIO()) return false;
    return ITransmission::HandshakeServer(y, session_id, mux);
}

void MultipathTransmission::Dispose() noexcept {
    if (disposed_.exchange(true, std::memory_order_acq_rel)) return;
    YieldContext::Anchor main_wake, mux_wake, main_pump_wake, mux_pump_wake;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        main_channel_.closed = true;
        mux_channel_.closed = true;
        main_wake = main_channel_.waiter;
        mux_wake = mux_channel_.waiter;
        main_channel_.waiter = {};
        mux_channel_.waiter = {};
        main_pump_wake = std::exchange(main_channel_.pump_waiter, {});
        mux_pump_wake = std::exchange(mux_channel_.pump_waiter, {});
    }
    main_wake.R();
    mux_wake.R();
    main_pump_wake.R();
    mux_pump_wake.R();
    {
        std::lock_guard<std::mutex> lock(secure_mutex_);
        psk_.Clear();
        secure_.Clear();
    }
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        control_handler_ = nullptr;
        pending_control_records_.clear();
        pending_control_bytes_ = 0;
    }
    if (carrier_) carrier_->Dispose();
    ITransmission::Dispose();
}

boost::asio::ip::tcp::endpoint MultipathTransmission::GetRemoteEndPoint() noexcept {
    return carrier_ ? carrier_->GetRemoteEndPoint() : boost::asio::ip::tcp::endpoint();
}

AuthenticatedCarrierKind MultipathTransmission::GetAuthenticatedCarrierKind() const noexcept {
    return carrier_ ? carrier_->GetAuthenticatedCarrierKind() : AuthenticatedCarrierKind::None;
}

bool MultipathTransmission::SupportsPacingRate() const noexcept {
    return carrier_ && carrier_->SupportsPacingRate();
}

std::uint64_t MultipathTransmission::GetPacingRateBytesPerSecond() const noexcept {
    return carrier_ ? carrier_->GetPacingRateBytesPerSecond() : 0;
}

TransmissionPacingResult MultipathTransmission::ApplyPacingRate(
    std::uint64_t bytes_per_second) noexcept {
    return carrier_ ? carrier_->ApplyPacingRate(bytes_per_second)
        : TransmissionPacingResult::Unsupported;
}

bool MultipathTransmission::IsAuthenticatedCarrierBindingActive() const noexcept { return IsSecureReady(); }
bool MultipathTransmission::HasAuthenticatedSessionExporter() const noexcept { return IsSecureReady(); }

bool MultipathTransmission::ExportAuthenticatedSessionKey(const char* label,
    const std::uint8_t* context, std::size_t context_length,
    std::uint8_t* output, std::size_t output_length) noexcept {
    if (!label || (!context && context_length) || !output || output_length == 0) return false;
    std::lock_guard<std::mutex> lock(secure_mutex_);
    return secure_.IsReady() && secure_.Export(reinterpret_cast<const std::uint8_t*>(label),
        std::strlen(label), context, context_length, output, output_length);
}

std::shared_ptr<Byte> MultipathTransmission::DoReadBytes(YieldContext& y, int length) noexcept {
    if (!application_io_started_.load(std::memory_order_acquire) && !BeginApplicationIO()) return nullptr;
    return ReadLogicalBytes(y, length, ApplicationRecord);
}

bool MultipathTransmission::DoWriteBytes(std::shared_ptr<Byte> packet, int offset,
    int packet_length, const AsynchronousWriteBytesCallback& cb) noexcept {
    return WriteLogicalBytes(std::move(packet), offset, packet_length, ApplicationRecord, cb);
}

bool MultipathTransmission::WriteLogicalBytes(std::shared_ptr<Byte> packet, int offset,
    int packet_length, std::uint8_t record_type, const AsynchronousWriteBytesCallback& cb) noexcept {
    if (!packet || offset < 0 || packet_length < 1 || disposed_ || !cb) return false;
    if (record_type != ApplicationRecord && record_type != MuxRecord) return false;
    if (record_type == MuxRecord) {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        if (mux_channel_.closed || !mux_subchannel_created_) return false;
    }
    constexpr std::size_t chunk_size = ppp::cryptography::noise::MultipathRecordMaxPlaintext;
    const std::size_t total = static_cast<std::size_t>(packet_length);
    const auto* data = reinterpret_cast<const std::uint8_t*>(packet.get() + offset);
    if (total <= chunk_size) return SubmitSealedRecord(record_type, data, total, cb);

    // One logical write is a bounded run of secure records (the caller's frame
    // limit bounds the run). Sealing and submitting the whole run before
    // waiting for any completion keeps several records outstanding on the
    // carrier instead of paying one carrier round trip per 16 KiB fragment.
    // Wire order is preserved because SubmitSealedRecord seals and enqueues
    // each record under the same record_write_mutex_ acquisition. The extra
    // countdown token defers the logical completion until every record has
    // been submitted and every accepted record has completed exactly once, so
    // a carrier that completes inline can never re-enter the caller early.
    struct Pending final {
        Pending(std::size_t records, const AsynchronousWriteCallback& completion) noexcept
            : remaining(records + 1), callback(completion) {}

        void Finish(std::size_t count, bool ok) noexcept {
            if (!ok) failed.store(true, std::memory_order_relaxed);
            if (remaining.fetch_sub(count, std::memory_order_acq_rel) == count) {
                callback(!failed.load(std::memory_order_relaxed));
            }
        }

        std::atomic<std::size_t> remaining;
        std::atomic_bool failed{false};
        AsynchronousWriteCallback callback;
    };

    const std::size_t fragments = 1 + (total - 1) / chunk_size;
    std::shared_ptr<Pending> pending;
    AsynchronousWriteCallback on_fragment;
    try {
        pending = std::make_shared<Pending>(fragments, cb);
        on_fragment = [pending](bool ok) noexcept { pending->Finish(1, ok); };
    } catch (...) { return false; }

    std::size_t submitted = 0;
    while (submitted < fragments) {
        const std::size_t pos = submitted * chunk_size;
        const std::size_t count = std::min(chunk_size, total - pos);
        if (!SubmitSealedRecord(record_type, data + pos, count, on_fragment)) break;
        ++submitted;
    }

    // Records never handed to the carrier and the deferred token retire now;
    // records already accepted still complete once each and decrement the rest.
    pending->Finish(fragments - submitted + 1, submitted == fragments);
    return true;
}

bool MultipathTransmission::SubmitSealedRecord(std::uint8_t type, const std::uint8_t* data,
    std::size_t length, const AsynchronousWriteCallback& callback) noexcept {
    if (!carrier_ || !callback || disposed_) return false;
    struct Completion final {
        std::mutex mutex;
        bool submitting = true;
        bool received = false;
        bool succeeded = false;
        std::shared_ptr<MultipathTransmission> owner;
        AsynchronousWriteCallback callback;

        void Complete(bool ok) noexcept {
            if (!ok) owner->Dispose();
            callback(ok);
        }
    };

    std::shared_ptr<Completion> completion;
    std::vector<std::uint8_t> record;
    bool sealed = false;
    bool accepted = false;
    try {
        completion = std::make_shared<Completion>();
        completion->owner = std::static_pointer_cast<MultipathTransmission>(shared_from_this());
        completion->callback = callback;
    } catch (...) { return false; }

    {
        std::lock_guard<std::mutex> write_lock(record_write_mutex_);
        {
            std::lock_guard<std::mutex> lock(secure_mutex_);
            sealed = !disposed_ && secure_.IsReady() && secure_.Seal(type, data, length, record);
            if (sealed) {
                accepted = carrier_->Write(record.data(), static_cast<int>(record.size()),
                    [completion](bool ok) noexcept {
                        bool complete_now = false;
                        {
                            std::lock_guard<std::mutex> lock(completion->mutex);
                            if (completion->received) return;
                            completion->received = true;
                            completion->succeeded = ok;
                            complete_now = !completion->submitting;
                        }
                        if (complete_now) completion->Complete(ok);
                    });
            }
        }
    }
    // A completion can submit the next fragment. Release both transmission
    // locks before allowing it to run, while keeping seal and write ordered.
    bool complete_deferred = false;
    bool deferred_result = false;
    {
        std::lock_guard<std::mutex> lock(completion->mutex);
        complete_deferred = accepted && completion->received;
        deferred_result = completion->succeeded;
        completion->received = completion->received || !accepted;
        completion->submitting = false;
    }
    if (complete_deferred) completion->Complete(deferred_result);
    if (sealed && !accepted) Dispose();
    return accepted;
}

}
