#include <ppp/stdafx.h>
#include <ppp/app/client/xtcp/XtcpRuntime.h>

#include <chrono>

#if defined(PPP_ENABLE_XTCP)
#include <ppp/app/client/xtcp/XtcpNdiBackend.h>
#include <ppp/app/client/xtcp/XtcpPoolLease.h>
#include <ppp/app/client/xtcp/XtcpRuntimePolicy.h>

#include <xtcp/core/ip.h>
#include <xtcp/core/stack.h>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>
#endif

namespace ppp::app::client::xtcp {
namespace {

std::uint16_t ReadBigEndian16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8) | static_cast<std::uint16_t>(bytes[1]));
}

XtcpOutputRejectionPacketShape ParseRejectedPacketShape(const void* data, int length) noexcept {
    XtcpOutputRejectionPacketShape shape;
    shape.captured = true;
    if (length <= 0) {
        return shape;
    }
    shape.supplied_bytes = static_cast<std::uint32_t>(length);
    if (data == nullptr) {
        return shape;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const std::size_t supplied = static_cast<std::size_t>(length);
    if (supplied < 20 || (bytes[0] >> 4) != 4) {
        return shape;
    }
    const std::size_t ip_header_length = static_cast<std::size_t>(bytes[0] & 0x0f) * 4;
    if (ip_header_length < 20 || ip_header_length > supplied || bytes[9] != 6) {
        return shape;
    }
    const std::size_t ipv4_total_length = ReadBigEndian16(bytes + 2);
    if (ipv4_total_length < ip_header_length + 20 || ipv4_total_length > supplied) {
        return shape;
    }

    const std::uint8_t* tcp = bytes + ip_header_length;
    const std::size_t tcp_header_length = static_cast<std::size_t>(tcp[12] >> 4) * 4;
    if (tcp_header_length < 20 || ip_header_length + tcp_header_length > ipv4_total_length) {
        return shape;
    }

    bool timestamps = false;
    bool sack = false;
    bool md5 = false;
    bool unknown = false;
    for (std::size_t option = 20; option < tcp_header_length;) {
        const std::uint8_t kind = tcp[option];
        if (kind == 0) {
            break;
        }
        if (kind == 1) {
            ++option;
            continue;
        }
        if (option + 1 >= tcp_header_length) {
            return shape;
        }
        const std::size_t option_length = tcp[option + 1];
        if (option_length < 2 || option + option_length > tcp_header_length) {
            return shape;
        }
        if (kind == 8) {
            if (option_length != 10) {
                return shape;
            }
            timestamps = true;
        }
        else if (kind == 5) {
            if (option_length < 10 || (option_length - 2) % 8 != 0) {
                return shape;
            }
            sack = true;
        }
        else if (kind == 19) {
            if (option_length != 18) {
                return shape;
            }
            md5 = true;
        }
        else {
            unknown = true;
        }
        option += option_length;
    }

    shape.parsed = true;
    shape.ipv4_total_length = static_cast<std::uint16_t>(ipv4_total_length);
    shape.ipv4_ihl = static_cast<std::uint8_t>(ip_header_length);
    shape.tcp_data_offset = static_cast<std::uint8_t>(tcp_header_length);
    shape.tcp_payload_length = static_cast<std::uint16_t>(
        ipv4_total_length - ip_header_length - tcp_header_length);
    shape.tcp_flags = tcp[13];
    shape.tcp_option_timestamps = timestamps;
    shape.tcp_option_sack = sack;
    shape.tcp_option_md5 = md5;
    shape.tcp_option_unknown = unknown;
    return shape;
}

bool IsOversizeIPv4Packet(const void* data, int length) noexcept {
    if (data == nullptr || length < 20) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const std::size_t supplied = static_cast<std::size_t>(length);
    if ((bytes[0] >> 4) != 4) {
        return false;
    }
    const std::size_t ip_header_length = static_cast<std::size_t>(bytes[0] & 0x0f) * 4;
    return ip_header_length >= 20 && ip_header_length <= supplied &&
        ReadBigEndian16(bytes + 2) > 1500;
}

} // namespace

void XtcpOutputRejectionDiagnostics::Record(
    bool owner_present, bool vethernet_disposed, bool tap_present, bool accepted) noexcept {
    if (!enabled_) return;
    // The production output handler calls owner->Output exactly when owner is
    // present, then passes that returned value here.
    if (owner_present && !accepted) {
        const std::uint64_t now_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        std::uint64_t expected = 0;
        (void)first_actual_output_rejected_monotonic_ns_.compare_exchange_strong(
            expected, now_ns, std::memory_order_relaxed);
    }
    if (!owner_present) {
        weak_owner_expired_.fetch_add(1, std::memory_order_relaxed);
    }
    else if (vethernet_disposed) {
        vethernet_disposed_.fetch_add(1, std::memory_order_relaxed);
    }
    else if (!tap_present) {
        tap_missing_.fetch_add(1, std::memory_order_relaxed);
    }
    else if (!accepted) {
        output_rejected_.fetch_add(1, std::memory_order_relaxed);
    }
    else {
        accepted_.fetch_add(1, std::memory_order_relaxed);
    }
}

void XtcpOutputRejectionDiagnostics::RecordRejectedPacketShape(
    const void* data, int length) noexcept {
    if (!enabled_) {
        return;
    }
    std::lock_guard<std::mutex> lock(packet_shape_sync_);
    if (first_rejected_packet_shape_.captured) {
        return;
    }
    first_rejected_packet_shape_ = ParseRejectedPacketShape(data, length);
}

void XtcpOutputRejectionDiagnostics::RecordOversizeOutputAttempt(
    const void* data, int length) noexcept {
    if (!enabled_ || !IsOversizeIPv4Packet(data, length)) {
        return;
    }
    std::lock_guard<std::mutex> lock(packet_shape_sync_);
    if (first_oversize_output_attempt_.packet_shape.captured) {
        return;
    }
    first_oversize_output_attempt_.packet_shape = ParseRejectedPacketShape(data, length);
    first_oversize_output_attempt_.first_monotonic_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

XtcpOutputRejectionSnapshot XtcpOutputRejectionDiagnostics::Snapshot() const noexcept {
    XtcpOutputRejectionPacketShape packet_shape;
    XtcpOutputPreCallOversizeAttemptSnapshot oversize_output_attempt;
    {
        std::lock_guard<std::mutex> lock(packet_shape_sync_);
        packet_shape = first_rejected_packet_shape_;
        oversize_output_attempt = first_oversize_output_attempt_;
    }
    return {
        weak_owner_expired_.load(std::memory_order_relaxed),
        vethernet_disposed_.load(std::memory_order_relaxed),
        tap_missing_.load(std::memory_order_relaxed),
        output_rejected_.load(std::memory_order_relaxed),
        accepted_.load(std::memory_order_relaxed),
        first_actual_output_rejected_monotonic_ns_.load(std::memory_order_relaxed),
        packet_shape,
        oversize_output_attempt,
    };
}

#if !defined(PPP_ENABLE_XTCP)
class XtcpRuntime::Impl final {};

XtcpRuntime::XtcpRuntime(
    const std::shared_ptr<boost::asio::io_context>&,
    OutputHandler,
    ListenerEndpointHandler,
    ExternalAcceptHandler,
    ExternalCancelHandler,
    std::shared_ptr<XtcpOutputRejectionDiagnostics>) noexcept {}
XtcpRuntime::~XtcpRuntime() noexcept = default;
bool XtcpRuntime::Start() noexcept { return false; }
void XtcpRuntime::MarkReady() noexcept {}
void XtcpRuntime::Stop() noexcept {}
bool XtcpRuntime::SubmitIPv4Tcp(const void*, int) noexcept { return false; }
bool XtcpRuntime::IsReady() const noexcept { return false; }
bool XtcpRuntime::IsRunning() const noexcept { return false; }
std::uint64_t XtcpRuntime::Generation() const noexcept { return 0; }
ppp::app::runtime::RuntimeXtcpStats XtcpRuntime::SnapshotStats() const noexcept { return {}; }
#if defined(PPP_XTCP_RUNTIME_TESTING)
bool XtcpRuntime::EmitOutputForTesting(const void*, int) noexcept { return false; }
#endif
#else
namespace {
constexpr std::size_t kIngressMaxItems = 1024;
constexpr std::size_t kIngressMaxBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxFlows = 4096;
constexpr std::size_t kConnectorReadBytes = 64 * 1024;
// Per-flow bridge queue cap for XTCP -> connector data. Rejecting a segment
// here makes the stack withhold the ACK (peer RTOs), so this threshold trades
// memory for retransmission avoidance; OPENPPP2_XTCP_WRITE_CAP_BYTES overrides
// it for the cap-sweep experiment.
std::size_t ConnectorWriteCap() noexcept {
    const char* env = ::getenv("OPENPPP2_XTCP_WRITE_CAP_BYTES");
    if (env != nullptr) {
        const long long value = ::atoll(env);
        if (value >= static_cast<long long>(kConnectorReadBytes)) {
            return static_cast<std::size_t>(value);
        }
    }
    // 4 MiB: cap-sweep evidence - smaller caps make OnReceive reject during
    // bursts, which makes the stack withhold ACKs (peer RTOs) and collapses
    // upload throughput (64K=17, 256K=87, 1M=249, 4M=387 Mbps).
    return 4 * 1024 * 1024;
}
// Runtime-wide queued-byte budget across all flows. The per-flow cap alone is
// not a resource guard (4MiB x kMaxFlows), so admission also checks this sum.
std::size_t GlobalQueueBudget() noexcept {
    const char* env = ::getenv("OPENPPP2_XTCP_GLOBAL_QUEUE_BYTES");
    if (env != nullptr) {
        const long long value = ::atoll(env);
        if (value > 0) {
            return static_cast<std::size_t>(value);
        }
    }
    return 32ull * 1024 * 1024;
}
// Send-admission retry cadence. Laboratory default: 1ms. LAB-ONLY knob
// (OPENPPP2_XTCP_LAB_SEND_RETRY_US): the A1 experiment proved retry cadence
// does not move throughput (stall is ACK-release bound), so this must never
// be treated as a production tuning parameter. Values below 50us clamp to 50.
std::chrono::microseconds SendRetryDelay() noexcept {
    const char* env = ::getenv("OPENPPP2_XTCP_LAB_SEND_RETRY_US");
    if (env != nullptr) {
        const long value = ::atol(env);
        if (value >= 50) {
            return std::chrono::microseconds(value);
        }
    }
    return std::chrono::milliseconds(1);
}

bool EnvEnabled(const char* name) noexcept {
    const char* value = ::getenv(name);
    return value != nullptr && value[0] != '\0' &&
        !(value[0] == '0' && value[1] == '\0');
}

constexpr std::uint8_t kTcpFin = 0x01;
constexpr std::uint8_t kTcpSyn = 0x02;
constexpr std::uint8_t kTcpRst = 0x04;
constexpr std::uint8_t kTcpAck = 0x10;

struct FlowKey final {
    std::uint32_t remote_address = 0;
    std::uint32_t local_address = 0;
    std::uint16_t remote_port = 0;
    std::uint16_t local_port = 0;

    bool operator==(const FlowKey& other) const noexcept {
        return remote_address == other.remote_address &&
            local_address == other.local_address &&
            remote_port == other.remote_port && local_port == other.local_port;
    }
};

struct FlowKeyHash final {
    std::size_t operator()(const FlowKey& key) const noexcept {
        std::size_t value = static_cast<std::size_t>(key.remote_address);
        value ^= static_cast<std::size_t>(key.local_address) * 0x9e3779b1u;
        value ^= static_cast<std::size_t>(key.remote_port) << 16;
        value ^= static_cast<std::size_t>(key.local_port);
        return value;
    }
};

struct ParsedPacket final {
    FlowKey key;
    ::xtcp::core::Endpoint remote;
    ::xtcp::core::Endpoint local;
    std::uint8_t flags = 0;
};

bool ParsePacket(const void* packet, int packet_length, ParsedPacket& parsed) noexcept {
    if (packet == nullptr || packet_length < 40) {
        return false;
    }
    const Byte* bytes = static_cast<const Byte*>(packet);
    ::xtcp::core::Ip4Hdr ip;
    if (!::xtcp::core::ParseIp4(bytes, static_cast<UInt32>(packet_length), ip) ||
        ip.proto != 6 || IsFragmentedIPv4(ip.frag_off, ip.flags) ||
        ip.payload_off + 20 > static_cast<UInt32>(packet_length)) {
        return false;
    }
    const Byte* tcp = bytes + ip.payload_off;
    parsed.key.remote_address = ip.src;
    parsed.key.local_address = ip.dst;
    parsed.key.remote_port = static_cast<std::uint16_t>((tcp[0] << 8) | tcp[1]);
    parsed.key.local_port = static_cast<std::uint16_t>((tcp[2] << 8) | tcp[3]);
    parsed.flags = tcp[13];
    parsed.remote.family = 4;
    parsed.remote.addr[0] = ip.src;
    parsed.remote.port = parsed.key.remote_port;
    parsed.local.family = 4;
    parsed.local.addr[0] = ip.dst;
    parsed.local.port = parsed.key.local_port;
    return parsed.key.remote_port != 0 && parsed.key.local_port != 0;
}

boost::asio::ip::address_v4 ToAddress(std::uint32_t network_address) noexcept {
    return boost::asio::ip::address_v4(IPv4AddressBytes(network_address));
}
} // namespace

class XtcpRuntime::Impl final :
    public XtcpFirstLegHooks,
    public std::enable_shared_from_this<XtcpRuntime::Impl> {
public:
    using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;

    struct Flow final {
        Flow(
            const std::shared_ptr<boost::asio::io_context>& context,
            const FlowKey& flow_key,
            const ::xtcp::core::Endpoint& remote_endpoint,
            const ::xtcp::core::Endpoint& local_endpoint,
            std::uint64_t generation_value) noexcept
            : key(flow_key), remote(remote_endpoint), local(local_endpoint),
              generation(generation_value), connector(*context), retry_timer(*context) {}

        FlowKey key;
        ::xtcp::core::Endpoint remote;
        ::xtcp::core::Endpoint local;
        std::uint64_t generation = 0;
        UInt64 connection_id = 0;
        std::uint16_t source_port = 0;
        boost::asio::ip::tcp::socket connector;
        boost::asio::steady_timer retry_timer;
        ::xtcp::buf::BufRef deferred_syn;
        std::array<Byte, kConnectorReadBytes> read_buffer{};
        std::vector<Byte> pending_read;
        std::deque<std::shared_ptr<std::vector<Byte>>> write_queue;
        std::size_t write_bytes = 0;
        bool connector_connected = false;
        bool connector_read_eof = false;
        bool connector_send_shutdown = false;
        bool first_leg_ready = false;
        bool first_leg_eof = false;
        bool first_leg_close_started = false;
        // The ppp-side forwarding for this flow is gone: the connector's peer
        // socket is closed or about to be. The read side still drains whatever
        // the kernel already received (EOF then closes the first leg
        // gracefully; a reset aborts it, since unread data was lost).
        bool peer_gone = false;
        // Set when the second leg failed before the first leg was established:
        // the deferred SYN is injected only so OnAccept rejects it with RST,
        // failing the app's pending connect fast instead of timing out.
        bool abort_when_ready = false;
        bool write_active = false;
        bool read_active = false;
        bool closing = false;
        // Perf diagnostic (strand-only): when Send admission rejected this
        // chunk, when the chunk finally went through; feeds send_stall_us.
        std::uint64_t send_stall_start_us = 0;
        // Optional transition-only SendData rejection snapshot. No packet
        // pointer is retained, and a successful admission clears this state.
        bool send_admission_blocked = false;
        bool send_admission_snapshot_valid = false;
        std::uint64_t send_admission_blocked_since_us = 0;
        ::xtcp::core::SendAdmissionSnapshot send_admission_snapshot;
        // Perf diagnostic (strand-only): async_write submit timestamp and the
        // previous completion timestamp, for cycle/gap histograms.
        std::uint64_t write_submit_us = 0;
        std::uint64_t write_last_complete_us = 0;
        // Bytes handed to the in-flight async_write; queued-byte accounting
        // is debited here on completion and for the remainder at teardown.
        std::size_t in_flight_bytes = 0;
    };

    Impl(
        const std::shared_ptr<boost::asio::io_context>& context,
        OutputHandler output,
        ListenerEndpointHandler listener_endpoint,
        ExternalAcceptHandler external_accept,
        ExternalCancelHandler external_cancel,
        std::shared_ptr<XtcpOutputRejectionDiagnostics> output_rejection_diagnostics) noexcept
        : context_(context), strand_(context ? std::make_shared<Strand>(context->get_executor()) : nullptr),
          output_(std::move(output)), listener_endpoint_(std::move(listener_endpoint)),
          external_accept_(std::move(external_accept)), external_cancel_(std::move(external_cancel)),
          output_rejection_diagnostics_(std::move(output_rejection_diagnostics)),
          budget_(kIngressMaxItems, kIngressMaxBytes) {}

    bool Start() noexcept {
        std::lock_guard<std::mutex> lock(state_sync_);
        if (!context_ || !strand_ || running_.load(std::memory_order_acquire) ||
            stopping_.load(std::memory_order_acquire)) {
            return false;
        }
        lease_ = XtcpPoolLease::Acquire();
        if (!lease_) {
            return false;
        }
        const std::weak_ptr<Impl> weak = shared_from_this();
        counted_output_ = [weak, handler = output_](const void* data, int length) noexcept {
            if (!handler) {
                return false;
            }
            if (const std::shared_ptr<Impl> self = weak.lock()) {
                if (self->output_rejection_diagnostics_ &&
                    self->output_rejection_diagnostics_->Enabled()) {
                    self->output_rejection_diagnostics_->RecordOversizeOutputAttempt(data, length);
                }
            }
            if (!handler(data, length)) {
                if (const std::shared_ptr<Impl> self = weak.lock()) {
                    if (self->output_rejection_diagnostics_ &&
                        self->output_rejection_diagnostics_->Enabled()) {
                        self->output_rejection_diagnostics_->RecordRejectedPacketShape(data, length);
                    }
                }
                return false;
            }
            if (const std::shared_ptr<Impl> self = weak.lock()) {
                self->stats_.output_packets.fetch_add(1, std::memory_order_relaxed);
                self->stats_.output_bytes.fetch_add(
                    static_cast<std::uint64_t>(static_cast<std::size_t>(length)),
                    std::memory_order_relaxed);
            }
            return true;
        };
        backend_ = std::unique_ptr<XtcpNdiBackend>(new (std::nothrow) XtcpNdiBackend(counted_output_));
        if (!backend_) {
            lease_.reset();
            return false;
        }
        stack_ = std::unique_ptr<::xtcp::XtcpStack>(new (std::nothrow) ::xtcp::XtcpStack(backend_.get()));
        if (!stack_) {
            backend_->Stop();
            backend_.reset();
            lease_.reset();
            return false;
        }
        // 拥塞控制可选 (XTCP-CC-SELECT-001): 默认 KCC (上游默认, 开发者维护)。
        // env 可切 bbr/cubic/reno。历史: KCC pacing bug (0005 修复前) 把 DL 锁在
        // ~216Mbps, CUBIC 因 pacing_rate=0 绕开 -> 曾临时默认 CUBIC; 0005
        // pacing-burst-quantum 修复后 KCC 追平 CUBIC (P1 DL 398 vs 400, P16 DL
        // 499 vs 493, UL 一致), 恢复 KCC 默认。保留 env 切换以便回归。
        {
            const char* cc_env = ::getenv("OPENPPP2_XTCP_CC");
            const char* cc = (cc_env != nullptr && cc_env[0] != '\0') ? cc_env : "kcc";
            stack_->SetDefaultCongestionControl(cc);
        }
        // KCC snd_buf 实验 (XTCP-KCC-SNDBUF-001): 上游默认 64K, 开发者建议 16K-32K。
        // KCC 是 BBR 风格, pacing_rate = bw_est, FlushPendingSend 的
        // limit = min(snd_buf, cwnd, snd_wnd)。64K snd_buf 与 KCC pacing 耦合
        // 可能低估 bw -> pacing 锁 216Mbps。env 可调, 默认不设 (=上游 64K)。
        {
            const char* sndbuf_env = ::getenv("OPENPPP2_XTCP_SNDBUF_BYTES");
            if (sndbuf_env != nullptr && sndbuf_env[0] != '\0') {
                const unsigned long long sndbuf = ::atoll(sndbuf_env);
                if (sndbuf >= 1024) {
                    stack_->SetSndBuf(static_cast<UInt32>(sndbuf));
                }
            }
        }
        stack_->SetAcceptHandler([weak](UInt64 connection_id,
            const ::xtcp::core::Endpoint& remote,
            const ::xtcp::core::Endpoint& local) noexcept {
            const std::shared_ptr<Impl> self = weak.lock();
            return self && self->OnAccept(connection_id, remote, local);
        });
        stack_->SetRecvHandlerChecked([weak](UInt64 connection_id, const Byte* data, UInt32 length) noexcept {
            const std::shared_ptr<Impl> self = weak.lock();
            return self && self->OnReceive(connection_id, data, length);
        });
        stack_->SetStateHandler([weak](UInt64 connection_id, ::xtcp::core::TcpState state) noexcept {
            if (const std::shared_ptr<Impl> self = weak.lock()) {
                self->OnState(connection_id, state);
            }
        });
        generation_.store(budget_.Start(), std::memory_order_release);
        running_.store(true, std::memory_order_release);
        ready_.store(false, std::memory_order_release);
        StartPerfDump();
        SchedulePoll(generation_.load(std::memory_order_acquire));
        return true;
    }

#if defined(PPP_XTCP_RUNTIME_TESTING)
    bool EmitOutputForTesting(const void* data, int length) noexcept {
        return counted_output_ && counted_output_(data, length);
    }
#endif

    void MarkReady() noexcept {
        const std::uint64_t generation = generation_.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(state_sync_);
            if (!running_.load(std::memory_order_acquire)) {
                return;
            }
            budget_.MarkReady(generation);
            ready_.store(budget_.IsReady(), std::memory_order_release);
        }
    }

    void Stop() noexcept {
        bool expected = false;
        if (!stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        running_.store(false, std::memory_order_release);
        ready_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(state_sync_);
            budget_.Stop();
        }
        if (!strand_) {
            DoStop();
            return;
        }
        const std::shared_ptr<Impl> self = shared_from_this();
        boost::asio::post(*strand_, [self]() noexcept { self->DoStop(); });
    }

    bool Submit(const void* packet, int packet_length) noexcept {
        if (packet == nullptr || packet_length < 1 || !ready_.load(std::memory_order_acquire)) {
            stats_.ingress_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const std::uint64_t generation = generation_.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(state_sync_);
            if (!budget_.Accepts(generation) ||
                !budget_.TryReserve(static_cast<std::size_t>(packet_length))) {
                stats_.ingress_dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        std::shared_ptr<std::vector<Byte>> copy;
        try {
            copy = std::make_shared<std::vector<Byte>>(static_cast<std::size_t>(packet_length));
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(state_sync_);
            budget_.Release(static_cast<std::size_t>(packet_length));
            stats_.ingress_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::memcpy(copy->data(), packet, static_cast<std::size_t>(packet_length));
        // XTCP-UL-ZEROCOPY-001: 上游 BufRef 是 zero-copy 语义 (movable, not
        // copyable)。此前 Submit -> vector 分配+memcpy, 然后 InjectCopy 再
        // BufRef::Acquire+memcpy, 每 UL 包 2 次分配 + 2 次拷贝。这里改为
        // 直接在 Submit 分配一次 BufRef, ProcessIngress 直接注入, StartFlow
        // 把 SYN 的 BufRef 移交给 deferred_syn, 全程 1 次分配 + 1 次拷贝。
        ::xtcp::buf::BufRef owned = ::xtcp::buf::BufRef::Acquire(
            static_cast<UInt32>(packet_length));
        if (owned.IsEmpty()) {
            std::lock_guard<std::mutex> lock(state_sync_);
            budget_.Release(static_cast<std::size_t>(packet_length));
            stats_.ingress_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::memcpy(owned.Data(), packet, static_cast<std::size_t>(packet_length));
        owned.SetLen(static_cast<UInt32>(packet_length));
        const std::shared_ptr<Impl> self = shared_from_this();
        const std::uint64_t enqueue_us = NowUs();
        try {
            boost::asio::post(*strand_, [self, owned = std::move(owned), generation, enqueue_us]() mutable noexcept {
                self->ProcessIngress(std::move(owned), generation, enqueue_us);
            });
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(state_sync_);
            budget_.Release(static_cast<std::size_t>(packet_length));
            stats_.ingress_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        stats_.ingress_enqueued.fetch_add(1, std::memory_order_relaxed);
        stats_.ingress_submitted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool IsReady() const noexcept { return ready_.load(std::memory_order_acquire); }
    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    std::uint64_t Generation() const noexcept { return generation_.load(std::memory_order_acquire); }

    ppp::app::runtime::RuntimeXtcpStats SnapshotStats() const noexcept {
        ppp::app::runtime::RuntimeXtcpStats snapshot;
        snapshot.ingress_submitted = stats_.ingress_submitted.load(std::memory_order_relaxed);
        snapshot.ingress_dropped = stats_.ingress_dropped.load(std::memory_order_relaxed);
        snapshot.ingress_injected = stats_.ingress_injected.load(std::memory_order_relaxed);
        snapshot.flows_opened = stats_.flows_opened.load(std::memory_order_relaxed);
        snapshot.flows_closed = stats_.flows_closed.load(std::memory_order_relaxed);
        snapshot.flows_active = snapshot.flows_opened >= snapshot.flows_closed
            ? snapshot.flows_opened - snapshot.flows_closed : 0;
        snapshot.timer_polls = stats_.timer_polls.load(std::memory_order_relaxed);
        snapshot.timer_events = stats_.timer_events.load(std::memory_order_relaxed);
        snapshot.output_packets = stats_.output_packets.load(std::memory_order_relaxed);
        snapshot.output_bytes = stats_.output_bytes.load(std::memory_order_relaxed);
        snapshot.connector_read_bytes = stats_.connector_read_bytes.load(std::memory_order_relaxed);
        snapshot.connector_written_bytes = stats_.connector_written_bytes.load(std::memory_order_relaxed);
        snapshot.queued_bytes = stats_.queued_bytes_total.load(std::memory_order_relaxed);
        snapshot.queued_bytes_highwater = stats_.queued_bytes_highwater.load(std::memory_order_relaxed);
        return snapshot;
    }

    void OnFirstLegReady(std::uint64_t runtime_generation, std::uint64_t flow_generation) noexcept override {
        const std::shared_ptr<Impl> self = shared_from_this();
        boost::asio::post(*strand_, [self, runtime_generation, flow_generation]() noexcept {
            if (!self->IsCurrent(runtime_generation)) {
                return;
            }
            const std::shared_ptr<Flow> flow = self->FindFlowGeneration(flow_generation);
            if (!flow || flow->closing || flow->first_leg_ready) {
                return;
            }
            flow->first_leg_ready = true;
            if (!flow->deferred_syn.IsEmpty() && self->backend_) {
                if (self->backend_->Inject(std::move(flow->deferred_syn))) {
                    self->stats_.ingress_injected.fetch_add(1, std::memory_order_relaxed);
                }
            }
            self->KickPoll();
        });
    }

    void OnFirstLegClosed(std::uint64_t runtime_generation, std::uint64_t flow_generation) noexcept override {
        const std::shared_ptr<Impl> self = shared_from_this();
        boost::asio::post(*strand_, [self, runtime_generation, flow_generation]() noexcept {
            if (!self->IsCurrent(runtime_generation)) {
                return;
            }
            if (const std::shared_ptr<Flow> flow = self->FindFlowGeneration(flow_generation)) {
                self->HandlePeerGone(flow, runtime_generation);
            }
        });
    }

    // The ppp-side forwarding object for this flow was disposed. Depending on
    // how far the flow got, this is either a refused/failed connect (answer
    // the app's pending SYN with RST) or an established second leg going away
    // (let the connector drain what the kernel already received; its EOF then
    // closes the first leg gracefully, a reset aborts it).
    void HandlePeerGone(const std::shared_ptr<Flow>& flow, std::uint64_t runtime_generation) noexcept {
        if (!flow->first_leg_ready) {
            if (!flow->deferred_syn.IsEmpty() && backend_) {
                flow->abort_when_ready = true;
                if (backend_->Inject(std::move(flow->deferred_syn))) {
                    stats_.ingress_injected.fetch_add(1, std::memory_order_relaxed);
                    KickPoll();
                    return;
                }
                flow->abort_when_ready = false;
            }
            CloseFlow(flow, true);
            return;
        }
        flow->peer_gone = true;
        // Upload data still queued for the connector can never be delivered;
        // drop it. The in-flight chunk is debited by its own completion
        // handler, so only the not-yet-submitted remainder is refunded here.
        const std::size_t unsubmitted =
            flow->write_bytes >= flow->in_flight_bytes
                ? flow->write_bytes - flow->in_flight_bytes : 0;
        stats_.queued_bytes_total.fetch_sub(unsubmitted, std::memory_order_relaxed);
        flow->write_queue.clear();
        flow->write_bytes = flow->in_flight_bytes;
        (void)runtime_generation;
    }

private:
    struct Stats final {
        std::atomic<std::uint64_t> ingress_submitted{0};
        std::atomic<std::uint64_t> ingress_dropped{0};
        std::atomic<std::uint64_t> ingress_injected{0};
        std::atomic<std::uint64_t> flows_opened{0};
        std::atomic<std::uint64_t> flows_closed{0};
        std::atomic<std::uint64_t> timer_polls{0};
        std::atomic<std::uint64_t> timer_events{0};
        std::atomic<std::uint64_t> output_packets{0};
        std::atomic<std::uint64_t> output_bytes{0};
        std::atomic<std::uint64_t> connector_read_bytes{0};
        std::atomic<std::uint64_t> connector_written_bytes{0};
        // Perf diagnostics for the env-gated JSON dump (see Start()); all
        // relaxed, none read on the stable stats path.
        std::atomic<std::uint64_t> timer_armed{0};
        std::atomic<std::uint64_t> connector_read_calls{0};
        std::atomic<std::uint64_t> connector_write_ops{0};
        std::atomic<std::uint64_t> recv_cb_calls{0};
        std::atomic<std::uint64_t> recv_cb_bytes{0};
        std::atomic<std::uint64_t> write_queue_highwater{0};
        std::atomic<std::uint64_t> stack_send_calls{0};
        std::atomic<std::uint64_t> stack_send_rejected{0};
        std::atomic<std::uint64_t> send_retry_armed{0};
        std::atomic<std::uint64_t> send_retry_fired{0};
        std::atomic<std::uint64_t> send_stall_us_sum{0};
        std::atomic<std::uint64_t> send_stall_events{0};
        std::atomic<std::uint64_t> ingress_enqueued{0};
        std::atomic<std::uint64_t> ingress_dispatched{0};
        std::atomic<std::uint64_t> on_receive_rejected{0};
        std::atomic<std::uint64_t> on_receive_rejected_bytes{0};
        std::atomic<std::uint64_t> queued_bytes_total{0};
        std::atomic<std::uint64_t> queued_bytes_highwater{0};
        // log2(us) histograms: bucket b counts samples in [2^b, 2^(b+1)).
        static constexpr std::size_t kHistBuckets = 32;
        using Hist = std::array<std::atomic<std::uint64_t>, kHistBuckets>;
        Hist queue_delay_us{};
        Hist timer_late_us{};
        Hist write_cycle_us{};
        Hist write_gap_us{};
    };
    // Histogram helpers (bucket = floor(log2(us)), clamped).
    static void HistAdd(Stats::Hist& hist, std::uint64_t us) noexcept {
        std::size_t bucket = 0;
        std::uint64_t value = us;
        while (value > 1 && bucket + 1 < Stats::kHistBuckets) {
            value >>= 1;
            ++bucket;
        }
        hist[bucket].fetch_add(1, std::memory_order_relaxed);
    }
    static double HistPercentile(
        const Stats::Hist& hist, std::uint64_t total, double pct) noexcept {
        if (total == 0) {
            return 0.0;
        }
        const std::uint64_t target = static_cast<std::uint64_t>(
            static_cast<double>(total) * pct);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < Stats::kHistBuckets; ++i) {
            seen += hist[i].load(std::memory_order_relaxed);
            if (seen >= target && seen != 0) {
                return static_cast<double>(std::uint64_t{1} << i);
            }
        }
        return 0.0;
    }

    bool IsCurrent(std::uint64_t generation) const noexcept {
        return running_.load(std::memory_order_acquire) && generation != 0 &&
            generation == generation_.load(std::memory_order_acquire);
    }

    void ProcessIngress(::xtcp::buf::BufRef&& packet, std::uint64_t generation, std::uint64_t enqueue_us) noexcept {
        stats_.ingress_dispatched.fetch_add(1, std::memory_order_relaxed);
        HistAdd(stats_.queue_delay_us, NowUs() - enqueue_us);
        {
            std::lock_guard<std::mutex> lock(state_sync_);
            budget_.Release(packet.IsEmpty() ? 0 : packet.Len());
        }
        if (packet.IsEmpty() || !IsCurrent(generation) || !stack_ || !backend_) {
            return;
        }
        ParsedPacket parsed;
        if (!ParsePacket(packet.Data(), static_cast<int>(packet.Len()), parsed)) {
            return;
        }
        const auto existing = flows_.find(parsed.key);
        if (existing != flows_.end()) {
            const std::shared_ptr<Flow>& flow = existing->second;
            if (flow->closing) {
                return;
            }
            if (!flow->first_leg_ready) {
                if ((parsed.flags & kTcpRst) != 0) {
                    CloseFlow(flow, false);
                }
                return;
            }
            if (backend_->Inject(std::move(packet))) {
                stats_.ingress_injected.fetch_add(1, std::memory_order_relaxed);
            }
            KickPoll();
            return;
        }
        if ((parsed.flags & kTcpSyn) == 0 || (parsed.flags & kTcpAck) != 0 ||
            flows_.size() >= kMaxFlows) {
            return;
        }
        StartFlow(parsed, std::move(packet));
    }

    void StartFlow(const ParsedPacket& parsed, ::xtcp::buf::BufRef&& packet) noexcept {
        const std::uint64_t flow_generation = ++next_flow_generation_;
        std::shared_ptr<Flow> flow;
        try {
            flow = std::make_shared<Flow>(context_, parsed.key, parsed.remote, parsed.local, flow_generation);
        }
        catch (...) {
            return;
        }
        flow->deferred_syn = std::move(packet);
        if (flow->deferred_syn.IsEmpty()) {
            return;
        }

        const UInt64 listener_key = ::xtcp::EndpointKey(parsed.local);
        auto listener = listener_refs_.find(listener_key);
        if (listener == listener_refs_.end()) {
            if (!stack_->Listen(parsed.local)) {
                return;
            }
            listener_refs_.emplace(listener_key, std::make_pair(parsed.local, 1u));
        }
        else {
            ++listener->second.second;
        }
        flows_.emplace(parsed.key, flow);
        stats_.flows_opened.fetch_add(1, std::memory_order_relaxed);

        boost::system::error_code ec;
        flow->connector.open(boost::asio::ip::tcp::v4(), ec);
        if (!ec) {
            flow->connector.bind(boost::asio::ip::tcp::endpoint(
                boost::asio::ip::address_v4::loopback(), 0), ec);
        }
        if (ec) {
            CloseFlow(flow, false);
            return;
        }
        flow->source_port = flow->connector.local_endpoint(ec).port();
        if (ec || flow->source_port == 0 || !external_accept_) {
            CloseFlow(flow, false);
            return;
        }
        const boost::asio::ip::tcp::endpoint local_endpoint(
            ToAddress(parsed.key.remote_address), parsed.key.remote_port);
        const boost::asio::ip::tcp::endpoint remote_endpoint(
            ToAddress(parsed.key.local_address), parsed.key.local_port);
        std::weak_ptr<XtcpFirstLegHooks> hooks = shared_from_this();
        const std::uint64_t runtime_generation = Generation();
        if (!external_accept_(local_endpoint, remote_endpoint, flow->source_port,
                runtime_generation, flow_generation, hooks)) {
            CloseFlow(flow, false);
            return;
        }
        const boost::asio::ip::tcp::endpoint listener_endpoint = listener_endpoint_
            ? listener_endpoint_() : boost::asio::ip::tcp::endpoint();
        if (listener_endpoint.port() == 0) {
            CloseFlow(flow, false);
            return;
        }
        const std::shared_ptr<Impl> self = shared_from_this();
        flow->connector.async_connect(listener_endpoint,
            boost::asio::bind_executor(*strand_,
                [self, flow, runtime_generation](const boost::system::error_code& connect_ec) noexcept {
                    if (!self->IsFlowCurrent(flow, runtime_generation)) {
                        return;
                    }
                    if (connect_ec) {
                        self->CloseFlow(flow, true);
                        return;
                    }
                    flow->connector_connected = true;
                    self->StartRead(flow, runtime_generation);
                }));
    }

    bool OnAccept(UInt64 connection_id, const ::xtcp::core::Endpoint& remote,
        const ::xtcp::core::Endpoint& local) noexcept {
        FlowKey key;
        key.remote_address = remote.addr[0];
        key.local_address = local.addr[0];
        key.remote_port = remote.port;
        key.local_port = local.port;
        const auto found = flows_.find(key);
        if (found == flows_.end() || found->second->closing ||
            found->second->connection_id != 0) {
            return false;
        }
        if (found->second->abort_when_ready) {
            // The second leg already refused this flow: reject so the stack
            // answers the app's pending connect with RST instead of SYN+ACK.
            // The flow cleanup runs after the stack finished this segment.
            const std::shared_ptr<Impl> self = shared_from_this();
            const std::shared_ptr<Flow> flow = found->second;
            const std::uint64_t runtime_generation = Generation();
            boost::asio::post(*strand_, [self, flow, runtime_generation]() noexcept {
                if (self->IsFlowCurrent(flow, runtime_generation)) {
                    self->CloseFlow(flow, false);
                }
            });
            return false;
        }
        try {
            if (!connections_.emplace(connection_id, key).second) {
                return false;
            }
        }
        catch (...) {
            return false;
        }
        found->second->connection_id = connection_id;
        return true;
    }

    bool OnReceive(UInt64 connection_id, const Byte* data, UInt32 length) noexcept {
        stats_.recv_cb_calls.fetch_add(1, std::memory_order_relaxed);
        if (data != nullptr) {
            stats_.recv_cb_bytes.fetch_add(length, std::memory_order_relaxed);
        }
        const std::shared_ptr<Flow> flow = FindConnection(connection_id);
        if (!flow || flow->closing || data == nullptr || length == 0) {
            return false;
        }
        if (flow->peer_gone) {
            // The second leg is gone: consume and discard so the first leg's
            // flow control keeps moving until the close lands on the app.
            return true;
        }
        const std::size_t global_budget = GlobalQueueBudget();
        if (length > kConnectorReadBytes + ConnectorWriteCap() - flow->write_bytes ||
            stats_.queued_bytes_total.load(std::memory_order_relaxed) + length > global_budget) {
            stats_.on_receive_rejected.fetch_add(1, std::memory_order_relaxed);
            stats_.on_receive_rejected_bytes.fetch_add(length, std::memory_order_relaxed);
            return false;
        }
        try {
            flow->write_queue.emplace_back(
                std::make_shared<std::vector<Byte>>(data, data + length));
        }
        catch (...) {
            return false;
        }
        flow->write_bytes += length;
        stats_.queued_bytes_total.fetch_add(length, std::memory_order_relaxed);
        {
            std::uint64_t highwater =
                stats_.write_queue_highwater.load(std::memory_order_relaxed);
            while (flow->write_bytes > highwater &&
                   !stats_.write_queue_highwater.compare_exchange_weak(
                       highwater, flow->write_bytes, std::memory_order_relaxed)) {
            }
        }
        {
            std::uint64_t global_high =
                stats_.queued_bytes_highwater.load(std::memory_order_relaxed);
            const std::uint64_t total =
                stats_.queued_bytes_total.load(std::memory_order_relaxed);
            while (total > global_high &&
                   !stats_.queued_bytes_highwater.compare_exchange_weak(
                       global_high, total, std::memory_order_relaxed)) {
            }
        }
        const std::shared_ptr<Impl> self = shared_from_this();
        const std::uint64_t runtime_generation = Generation();
        try {
            boost::asio::post(*strand_, [self, flow, runtime_generation]() noexcept {
                self->StartWrite(flow, runtime_generation);
            });
        }
        catch (...) {
            flow->write_queue.pop_back();
            flow->write_bytes -= length;
            return false;
        }
        return true;
    }

    void OnState(UInt64 connection_id, ::xtcp::core::TcpState state) noexcept {
        if (state != ::xtcp::core::TcpState::kClosed &&
            state != ::xtcp::core::TcpState::kCloseWait &&
            state != ::xtcp::core::TcpState::kTimeWait) {
            return;
        }
        const std::shared_ptr<Impl> self = shared_from_this();
        const std::uint64_t runtime_generation = Generation();
        boost::asio::post(*strand_, [self, connection_id, state, runtime_generation]() noexcept {
            if (!self->IsCurrent(runtime_generation)) {
                return;
            }
            if (const std::shared_ptr<Flow> flow = self->FindConnection(connection_id)) {
                if (state == ::xtcp::core::TcpState::kCloseWait) {
                    flow->first_leg_eof = true;
                    self->MaybeShutdownConnectorSend(flow, runtime_generation);
                }
                else {
                    self->CloseFlow(flow, false);
                }
            }
            self->KickPoll();
        });
    }

    void StartRead(const std::shared_ptr<Flow>& flow, std::uint64_t runtime_generation) noexcept {
        if (!IsFlowCurrent(flow, runtime_generation) || flow->read_active ||
            !flow->connector_connected || flow->connector_read_eof ||
            !flow->pending_read.empty()) {
            return;
        }
        flow->read_active = true;
        const std::shared_ptr<Impl> self = shared_from_this();
        flow->connector.async_read_some(boost::asio::buffer(flow->read_buffer),
            boost::asio::bind_executor(*strand_,
                [self, flow, runtime_generation](const boost::system::error_code& ec,
                    std::size_t length) noexcept {
                    flow->read_active = false;
                    if (!self->IsFlowCurrent(flow, runtime_generation)) {
                        return;
                    }
                    if (ec && ec != boost::asio::error::eof) {
                        self->CloseFlow(flow, true);
                        return;
                    }
                    flow->connector_read_eof = ec == boost::asio::error::eof || length == 0;
                    if (length != 0) {
                        self->stats_.connector_read_calls.fetch_add(1, std::memory_order_relaxed);
                        self->stats_.connector_read_bytes.fetch_add(
                            static_cast<std::uint64_t>(length), std::memory_order_relaxed);
                        flow->pending_read.assign(flow->read_buffer.data(),
                            flow->read_buffer.data() + length);
                        self->TrySendPending(flow, runtime_generation);
                    }
                    else {
                        self->BeginFirstLegClose(flow);
                    }
                }));
    }

    void TrySendPending(const std::shared_ptr<Flow>& flow, std::uint64_t runtime_generation) noexcept {
        if (!IsFlowCurrent(flow, runtime_generation) || flow->pending_read.empty()) {
            return;
        }
        stats_.stack_send_calls.fetch_add(1, std::memory_order_relaxed);
        const bool accepted = flow->connection_id != 0 && stack_ &&
            stack_->Send(flow->connection_id, flow->pending_read.data(),
                static_cast<UInt32>(flow->pending_read.size()));
        if (accepted) {
            if (flow->send_stall_start_us != 0) {
                stats_.send_stall_us_sum.fetch_add(
                    NowUs() - std::exchange(flow->send_stall_start_us, 0),
                    std::memory_order_relaxed);
                stats_.send_stall_events.fetch_add(1, std::memory_order_relaxed);
            }
            if (send_admission_enabled_) {
                flow->send_admission_blocked = false;
                flow->send_admission_snapshot_valid = false;
                flow->send_admission_blocked_since_us = 0;
                flow->send_admission_snapshot = {};
            }
            flow->pending_read.clear();
            KickPoll();
            if (flow->connector_read_eof) {
                BeginFirstLegClose(flow);
            }
            else {
                StartRead(flow, runtime_generation);
            }
            return;
        }
        stats_.stack_send_rejected.fetch_add(1, std::memory_order_relaxed);
        if (flow->send_stall_start_us == 0) {
            flow->send_stall_start_us = NowUs();
        }
        // Read exactly one upstream snapshot on the transition into blocked;
        // subsequent 1ms retries deliberately do not sample again.
        if (send_admission_enabled_ && !flow->send_admission_blocked) {
            flow->send_admission_blocked = true;
            flow->send_admission_blocked_since_us = NowUs();
            flow->send_admission_snapshot = {};
            flow->send_admission_snapshot_valid = flow->connection_id != 0 && stack_ &&
                stack_->ConnLastSendAdmission(flow->connection_id, flow->send_admission_snapshot);
        }
        stats_.send_retry_armed.fetch_add(1, std::memory_order_relaxed);
        flow->retry_timer.expires_after(SendRetryDelay());
        const std::shared_ptr<Impl> self = shared_from_this();
        flow->retry_timer.async_wait(boost::asio::bind_executor(*strand_,
            [self, flow, runtime_generation](const boost::system::error_code& ec) noexcept {
                if (!ec && self->IsFlowCurrent(flow, runtime_generation)) {
                    self->stats_.send_retry_fired.fetch_add(1, std::memory_order_relaxed);
                    self->TrySendPending(flow, runtime_generation);
                }
            }));
    }

    void StartWrite(const std::shared_ptr<Flow>& flow, std::uint64_t runtime_generation) noexcept {
        if (!IsFlowCurrent(flow, runtime_generation) || flow->write_active ||
            flow->peer_gone || !flow->connector_connected) {
            return;
        }
        if (flow->write_queue.empty()) {
            MaybeShutdownConnectorSend(flow, runtime_generation);
            return;
        }
        // 注: XTCP-UL-WRITE-BATCH-001 (合并 write_queue 为一次 async_write) 已回滚——
        // A/B 实测 UL on 崩到 2.6Mbps (256KB 单次写超时), off 366 vs 375 略降。
        // 证明 UL 瓶颈不在 write 合并; 真瓶颈是 strand 串行投递 (owner q_p50=128us)。
        flow->write_active = true;
        const std::shared_ptr<std::vector<Byte>> chunk = flow->write_queue.front();
        flow->in_flight_bytes = chunk->size();
        const std::uint64_t submit_us = NowUs();
        if (flow->write_last_complete_us != 0) {
            HistAdd(stats_.write_gap_us,
                submit_us > flow->write_last_complete_us
                    ? submit_us - flow->write_last_complete_us : 0);
        }
        flow->write_submit_us = submit_us;
        const std::shared_ptr<Impl> self = shared_from_this();
        boost::asio::async_write(flow->connector, boost::asio::buffer(*chunk),
            boost::asio::bind_executor(*strand_,
                [self, flow, chunk, runtime_generation](const boost::system::error_code& ec,
                    std::size_t) noexcept {
                    // Debit the queued-byte ledger for this chunk exactly once,
                    // regardless of the outcome.
                    self->stats_.queued_bytes_total.fetch_sub(
                        std::exchange(flow->in_flight_bytes, 0),
                        std::memory_order_relaxed);
                    flow->write_active = false;
                    if (!self->IsFlowCurrent(flow, runtime_generation)) {
                        return;
                    }
                    if (ec) {
                        if (flow->peer_gone) {
                            // The second leg is gone: the write side is dead,
                            // but the read side may still drain buffered bytes
                            // before the peer's EOF arrives.
                            return;
                        }
                        self->CloseFlow(flow, true);
                        return;
                    }
                    if (flow->write_submit_us != 0) {
                        HistAdd(self->stats_.write_cycle_us,
                            NowUs() - std::exchange(flow->write_submit_us, 0));
                        flow->write_last_complete_us = NowUs();
                    }
                    flow->write_bytes = chunk->size() <= flow->write_bytes
                        ? flow->write_bytes - chunk->size() : 0;
                    self->stats_.connector_write_ops.fetch_add(1, std::memory_order_relaxed);
                    self->stats_.connector_written_bytes.fetch_add(
                        static_cast<std::uint64_t>(chunk->size()), std::memory_order_relaxed);
                    if (!flow->write_queue.empty()) {
                        flow->write_queue.pop_front();
                    }
                    self->StartWrite(flow, runtime_generation);
                }));
    }

    void MaybeShutdownConnectorSend(
        const std::shared_ptr<Flow>& flow,
        std::uint64_t runtime_generation) noexcept {
        if (!IsFlowCurrent(flow, runtime_generation) || !flow->first_leg_eof ||
            flow->connector_send_shutdown || flow->write_active ||
            !flow->write_queue.empty()) {
            return;
        }
        boost::system::error_code ec;
        flow->connector.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
        if (ec) {
            if (flow->peer_gone) {
                // The peer socket is already gone; the read side still drains.
                flow->connector_send_shutdown = true;
                return;
            }
            CloseFlow(flow, true);
            return;
        }
        flow->connector_send_shutdown = true;
    }

    void BeginFirstLegClose(const std::shared_ptr<Flow>& flow) noexcept {
        if (!flow || flow->closing || flow->first_leg_close_started) {
            return;
        }
        if (flow->connection_id == 0 || !stack_) {
            CloseFlow(flow, false);
            return;
        }
        flow->first_leg_close_started = true;
        stack_->Close(flow->connection_id);
        KickPoll();
    }

    bool IsFlowCurrent(const std::shared_ptr<Flow>& flow, std::uint64_t runtime_generation) const noexcept {
        if (!flow || flow->closing || !IsCurrent(runtime_generation)) {
            return false;
        }
        const auto found = flows_.find(flow->key);
        return found != flows_.end() && found->second == flow;
    }

    std::shared_ptr<Flow> FindFlowGeneration(std::uint64_t flow_generation) noexcept {
        for (const auto& entry : flows_) {
            if (entry.second->generation == flow_generation) {
                return entry.second;
            }
        }
        return nullptr;
    }

    std::shared_ptr<Flow> FindConnection(UInt64 connection_id) noexcept {
        const auto key = connections_.find(connection_id);
        if (key == connections_.end()) {
            return nullptr;
        }
        const auto flow = flows_.find(key->second);
        return flow == flows_.end() ? nullptr : flow->second;
    }

    void CloseFlow(const std::shared_ptr<Flow>& flow, bool abort_stack) noexcept {
        if (!flow || flow->closing) {
            return;
        }
        flow->closing = true;
        // Refund only the not-yet-submitted tail; the in-flight chunk is
        // debited by its own completion handler even after cancellation.
        const std::size_t unsubmitted =
            flow->write_bytes >= flow->in_flight_bytes
                ? flow->write_bytes - flow->in_flight_bytes : 0;
        stats_.queued_bytes_total.fetch_sub(unsubmitted, std::memory_order_relaxed);
        if (external_cancel_ && flow->source_port != 0) {
            external_cancel_(flow->source_port, Generation());
        }
        boost::system::error_code ec;
        flow->retry_timer.cancel(ec);
        flow->connector.cancel(ec);
        flow->connector.close(ec);
        if (flow->connection_id != 0) {
            connections_.erase(flow->connection_id);
            if (abort_stack && stack_) {
                stack_->Abort(flow->connection_id);
            }
        }
        const UInt64 listener_key = ::xtcp::EndpointKey(flow->local);
        const auto listener = listener_refs_.find(listener_key);
        if (listener != listener_refs_.end()) {
            if (listener->second.second > 1) {
                --listener->second.second;
            }
            else {
                if (stack_) {
                    stack_->StopListen(listener->second.first);
                }
                listener_refs_.erase(listener);
            }
        }
        flows_.erase(flow->key);
        stats_.flows_closed.fetch_add(1, std::memory_order_relaxed);
        KickPoll();
    }

    static std::uint64_t NowUs() noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // ---------------------------------------------------------------- perf dump
    // Env-gated (OPENPPP2_XTCP_PERF_JSON=<path>): appends one JSON line per
    // second with per-interval deltas of the diagnostic counters. Off by
    // default; when unset there is no timer and no I/O.
    struct PerfPrev final {
        std::uint64_t stack_send_calls = 0;
        std::uint64_t stack_send_rejected = 0;
        std::uint64_t send_retry_armed = 0;
        std::uint64_t send_retry_fired = 0;
        std::uint64_t send_stall_us_sum = 0;
        std::uint64_t send_stall_events = 0;
        std::uint64_t connector_read_calls = 0;
        std::uint64_t connector_read_bytes = 0;
        std::uint64_t connector_write_ops = 0;
        std::uint64_t connector_written_bytes = 0;
        std::uint64_t recv_cb_calls = 0;
        std::uint64_t recv_cb_bytes = 0;
        std::uint64_t ingress_enqueued = 0;
        std::uint64_t ingress_dispatched = 0;
        std::uint64_t ingress_dropped = 0;
        std::uint64_t ingress_injected = 0;
        std::uint64_t on_receive_rejected = 0;
        std::uint64_t on_receive_rejected_bytes = 0;
        std::uint64_t output_packets = 0;
        std::uint64_t output_bytes = 0;
        std::uint64_t timer_armed = 0;
        ::xtcp::core::AckReleaseTelemetrySnapshot ack_release;
        XtcpOutputRejectionSnapshot output_rejections;
        // Plain snapshots of the atomic histograms (copyable).
        std::uint64_t queue_delay_us[Stats::kHistBuckets] = {};
        std::uint64_t timer_late_us[Stats::kHistBuckets] = {};
        std::uint64_t write_cycle_us[Stats::kHistBuckets] = {};
        std::uint64_t write_gap_us[Stats::kHistBuckets] = {};
    };

    void StartPerfDump() noexcept {
        const char* path = ::getenv("OPENPPP2_XTCP_PERF_JSON");
        if (path == nullptr || path[0] == '\0' || !context_) {
            return;
        }
        send_admission_enabled_ = EnvEnabled("OPENPPP2_XTCP_SEND_ADMISSION_JSON");
        ack_release_enabled_ = EnvEnabled("OPENPPP2_XTCP_ACK_RELEASE_JSON");
        output_rejection_enabled_ = EnvEnabled("OPENPPP2_XTCP_OUTPUT_REJECTION_JSON") &&
            output_rejection_diagnostics_ && output_rejection_diagnostics_->Enabled();
        perf_json_path_ = path;
        boost::asio::post(*strand_, [self = shared_from_this()]() noexcept {
            self->perf_out_.open(self->perf_json_path_, std::ios::app);
            if (!self->perf_out_.is_open()) {
                return;
            }
            self->ArmPerfDump();
        });
    }

    void ArmPerfDump() noexcept {
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }
        perf_dump_timer_ = std::make_shared<boost::asio::steady_timer>(*context_);
        perf_dump_timer_->expires_after(std::chrono::milliseconds(1000));
        const std::shared_ptr<Impl> self = shared_from_this();
        perf_dump_timer_->async_wait(boost::asio::bind_executor(*strand_,
            [self](const boost::system::error_code& ec) noexcept {
                if (ec || !self->running_.load(std::memory_order_acquire)) {
                    return;
                }
                self->WritePerfLine();
                self->ArmPerfDump();
            }));
    }

    template <typename T>
    static T DeltaOf(const std::atomic<T>& counter, T& prev) noexcept {
        const T now_value = counter.load(std::memory_order_relaxed);
        const T delta = now_value > prev ? now_value - prev : 0;
        prev = now_value;
        return delta;
    }

    void WritePerfLine() noexcept {
        PerfPrev now_prev;
        // Sample the first live connection's TCP internals (real cwnd /
        // inflight / peer window) so the DL analysis uses stack truth.
        tcp_sample_ = {};
        for (const auto& entry : flows_) {
            const std::shared_ptr<Flow>& flow = entry.second;
            if (!flow->closing && flow->connection_id != 0 && stack_) {
                stack_->ConnStats(flow->connection_id, tcp_sample_.inflight,
                    tcp_sample_.cwnd, tcp_sample_.ssthresh, tcp_sample_.snd_wnd,
                    tcp_sample_.retx, tcp_sample_.rto_deadline, tcp_sample_.dup_acks,
                    tcp_sample_.fast_rec, tcp_sample_.front_seq, tcp_sample_.snd_una,
                    tcp_sample_.local_port, tcp_sample_.remote_port);
                break;
            }
        }
        now_prev.stack_send_calls = stats_.stack_send_calls.load(std::memory_order_relaxed);
        now_prev.stack_send_rejected = stats_.stack_send_rejected.load(std::memory_order_relaxed);
        now_prev.send_retry_armed = stats_.send_retry_armed.load(std::memory_order_relaxed);
        now_prev.send_retry_fired = stats_.send_retry_fired.load(std::memory_order_relaxed);
        now_prev.send_stall_us_sum = stats_.send_stall_us_sum.load(std::memory_order_relaxed);
        now_prev.send_stall_events = stats_.send_stall_events.load(std::memory_order_relaxed);
        now_prev.connector_read_calls = stats_.connector_read_calls.load(std::memory_order_relaxed);
        now_prev.connector_read_bytes = stats_.connector_read_bytes.load(std::memory_order_relaxed);
        now_prev.connector_write_ops = stats_.connector_write_ops.load(std::memory_order_relaxed);
        now_prev.connector_written_bytes = stats_.connector_written_bytes.load(std::memory_order_relaxed);
        now_prev.recv_cb_calls = stats_.recv_cb_calls.load(std::memory_order_relaxed);
        now_prev.recv_cb_bytes = stats_.recv_cb_bytes.load(std::memory_order_relaxed);
        now_prev.ingress_enqueued = stats_.ingress_enqueued.load(std::memory_order_relaxed);
        now_prev.ingress_dispatched = stats_.ingress_dispatched.load(std::memory_order_relaxed);
        now_prev.ingress_dropped = stats_.ingress_dropped.load(std::memory_order_relaxed);
        now_prev.ingress_injected = stats_.ingress_injected.load(std::memory_order_relaxed);
        now_prev.on_receive_rejected = stats_.on_receive_rejected.load(std::memory_order_relaxed);
        now_prev.on_receive_rejected_bytes =
            stats_.on_receive_rejected_bytes.load(std::memory_order_relaxed);
    // A2-0: NDI output-path snapshot and per-flow queue distribution.
        ndi_stats_ = backend_ ? backend_->SnapshotTxStats() : XtcpNdiBackend::TxStats{};
        std::uint32_t above_256k = 0;
        std::uint32_t above_1m = 0;
        std::uint32_t above_2m = 0;
        for (const auto& entry : flows_) {
            const std::size_t bytes = entry.second->write_bytes;
            if (bytes > 256 * 1024) {
                ++above_256k;
            }
            if (bytes > 1024 * 1024) {
                ++above_1m;
            }
            if (bytes > 2 * 1024 * 1024) {
                ++above_2m;
            }
        }
        flows_above_256k_ = above_256k;
        flows_above_1m_ = above_1m;
        flows_above_2m_ = above_2m;
        now_prev.output_packets = stats_.output_packets.load(std::memory_order_relaxed);
        now_prev.output_bytes = stats_.output_bytes.load(std::memory_order_relaxed);
        now_prev.timer_armed = stats_.timer_armed.load(std::memory_order_relaxed);
        if (ack_release_enabled_ && stack_) {
            now_prev.ack_release = stack_->AckReleaseTelemetry();
        }
        if (output_rejection_enabled_) {
            now_prev.output_rejections = output_rejection_diagnostics_->Snapshot();
        }
        for (std::size_t i = 0; i < Stats::kHistBuckets; ++i) {
            now_prev.queue_delay_us[i] = stats_.queue_delay_us[i].load(std::memory_order_relaxed);
            now_prev.timer_late_us[i] = stats_.timer_late_us[i].load(std::memory_order_relaxed);
            now_prev.write_cycle_us[i] = stats_.write_cycle_us[i].load(std::memory_order_relaxed);
            now_prev.write_gap_us[i] = stats_.write_gap_us[i].load(std::memory_order_relaxed);
        }
        const PerfPrev& p = perf_prev_;
        const std::uint64_t send_calls = now_prev.stack_send_calls >= p.stack_send_calls
            ? now_prev.stack_send_calls - p.stack_send_calls : 0;
        const std::uint64_t send_rejected = now_prev.stack_send_rejected >= p.stack_send_rejected
            ? now_prev.stack_send_rejected - p.stack_send_rejected : 0;
        const std::uint64_t stall_us = now_prev.send_stall_us_sum >= p.send_stall_us_sum
            ? now_prev.send_stall_us_sum - p.send_stall_us_sum : 0;
        const std::uint64_t rd_calls = now_prev.connector_read_calls >= p.connector_read_calls
            ? now_prev.connector_read_calls - p.connector_read_calls : 0;
        const std::uint64_t rd_bytes = now_prev.connector_read_bytes >= p.connector_read_bytes
            ? now_prev.connector_read_bytes - p.connector_read_bytes : 0;
        const std::uint64_t wr_ops = now_prev.connector_write_ops >= p.connector_write_ops
            ? now_prev.connector_write_ops - p.connector_write_ops : 0;
        const std::uint64_t wr_bytes = now_prev.connector_written_bytes >= p.connector_written_bytes
            ? now_prev.connector_written_bytes - p.connector_written_bytes : 0;
        const std::uint64_t recv_calls = now_prev.recv_cb_calls >= p.recv_cb_calls
            ? now_prev.recv_cb_calls - p.recv_cb_calls : 0;
        const std::uint64_t recv_bytes = now_prev.recv_cb_bytes >= p.recv_cb_bytes
            ? now_prev.recv_cb_bytes - p.recv_cb_bytes : 0;
        const std::uint64_t out_pkts = now_prev.output_packets >= p.output_packets
            ? now_prev.output_packets - p.output_packets : 0;
        const std::uint64_t out_bytes = now_prev.output_bytes >= p.output_bytes
            ? now_prev.output_bytes - p.output_bytes : 0;

        Stats::Hist queue_hist{};
        std::uint64_t q_total = 0;
        for (std::size_t i = 0; i < Stats::kHistBuckets; ++i) {
            const std::uint64_t delta =
                now_prev.queue_delay_us[i] > p.queue_delay_us[i]
                    ? now_prev.queue_delay_us[i] - p.queue_delay_us[i] : 0;
            queue_hist[i].store(delta, std::memory_order_relaxed);
            q_total += delta;
        }
        Stats::Hist late_hist{};
        std::uint64_t l_total = 0;
        for (std::size_t i = 0; i < Stats::kHistBuckets; ++i) {
            const std::uint64_t delta =
                now_prev.timer_late_us[i] > p.timer_late_us[i]
                    ? now_prev.timer_late_us[i] - p.timer_late_us[i] : 0;
            late_hist[i].store(delta, std::memory_order_relaxed);
            l_total += delta;
        }

        Stats::Hist cycle_hist{};
        std::uint64_t c_total = 0;
        for (std::size_t i = 0; i < Stats::kHistBuckets; ++i) {
            const std::uint64_t delta =
                now_prev.write_cycle_us[i] > p.write_cycle_us[i]
                    ? now_prev.write_cycle_us[i] - p.write_cycle_us[i] : 0;
            cycle_hist[i].store(delta, std::memory_order_relaxed);
            c_total += delta;
        }
        Stats::Hist gap_hist{};
        std::uint64_t g_total = 0;
        for (std::size_t i = 0; i < Stats::kHistBuckets; ++i) {
            const std::uint64_t delta =
                now_prev.write_gap_us[i] > p.write_gap_us[i]
                    ? now_prev.write_gap_us[i] - p.write_gap_us[i] : 0;
            gap_hist[i].store(delta, std::memory_order_relaxed);
            g_total += delta;
        }
        // A2-0 NDI deltas: per-interval packet rate and wall-time percentiles.
        ndi_pps_ = ndi_stats_.tx_calls >= ndi_prev_.tx_calls
            ? ndi_stats_.tx_calls - ndi_prev_.tx_calls : 0;
        const std::uint64_t batch_delta = ndi_stats_.batch_packets >= ndi_prev_.batch_packets
            ? ndi_stats_.batch_packets - ndi_prev_.batch_packets : 0;
        ndi_batch_avg_ = ndi_stats_.batch_calls > ndi_prev_.batch_calls && batch_delta != 0
            ? static_cast<double>(batch_delta) /
                  (ndi_stats_.batch_calls - ndi_prev_.batch_calls)
            : 0.0;
        ndi_batch_max_ = ndi_stats_.batch_max;
        auto ndi_percentile = [](const std::uint64_t (&now_hist)[32],
                                 const std::uint64_t (&prev_hist)[32], double pct) -> double {
            std::uint64_t total = 0;
            std::uint64_t delta[32] = {};
            for (std::size_t i = 0; i < 32; ++i) {
                delta[i] = now_hist[i] > prev_hist[i] ? now_hist[i] - prev_hist[i] : 0;
                total += delta[i];
            }
            if (total == 0) {
                return 0.0;
            }
            const std::uint64_t target =
                static_cast<std::uint64_t>(static_cast<double>(total) * pct);
            std::uint64_t seen = 0;
            for (std::size_t i = 0; i < 32; ++i) {
                seen += delta[i];
                if (seen >= target && seen != 0) {
                    return static_cast<double>(std::uint64_t{1} << i);
                }
            }
            return 0.0;
        };
        ndi_out_p50_us_ = ndi_percentile(ndi_stats_.output_us, ndi_prev_.output_us, 0.50);
        ndi_out_p95_us_ = ndi_percentile(ndi_stats_.output_us, ndi_prev_.output_us, 0.95);
        ndi_out_p99_us_ = ndi_percentile(ndi_stats_.output_us, ndi_prev_.output_us, 0.99);
        ndi_iv_p50_us_ = ndi_percentile(ndi_stats_.interval_us, ndi_prev_.interval_us, 0.50);
        ndi_iv_p95_us_ = ndi_percentile(ndi_stats_.interval_us, ndi_prev_.interval_us, 0.95);
        const auto counter_delta = [](std::uint64_t now_value, std::uint64_t previous) noexcept {
            return now_value >= previous ? now_value - previous : std::uint64_t{0};
        };
        const std::uint64_t ndi_attempts = counter_delta(ndi_stats_.attempts, ndi_prev_.attempts);
        const std::uint64_t ndi_accepted = counter_delta(ndi_stats_.accepted, ndi_prev_.accepted);
        const std::uint64_t ndi_rejected = counter_delta(ndi_stats_.rejected, ndi_prev_.rejected);
        char ack_release_buf[1024] = {};
        if (ack_release_enabled_) {
            const ::xtcp::core::AckReleaseTelemetrySnapshot& ack = now_prev.ack_release;
            const ::xtcp::core::AckReleaseTelemetrySnapshot& ack_prev = p.ack_release;
            std::snprintf(ack_release_buf, sizeof(ack_release_buf),
                "\"ack\":{\"valid\":%llu,\"advance_events\":%llu,\"advance_bytes\":%llu},"
                "\"pending_flush\":{\"attempts\":%llu,\"tx_sink_packets\":%llu,"
                "\"tx_sink_bytes\":%llu,\"pacing\":%llu,\"window_cwnd\":%llu,"
                "\"fast_recovery_pipe\":%llu,\"packet_allocation\":%llu},"
                "\"ndi_acceptance\":{\"attempts\":%llu,\"accepted\":%llu,\"rejected\":%llu},",
                (unsigned long long)counter_delta(ack.valid_acks, ack_prev.valid_acks),
                (unsigned long long)counter_delta(ack.ack_advance_events, ack_prev.ack_advance_events),
                (unsigned long long)counter_delta(ack.ack_advance_bytes, ack_prev.ack_advance_bytes),
                (unsigned long long)counter_delta(ack.pending_flush_attempts, ack_prev.pending_flush_attempts),
                (unsigned long long)counter_delta(ack.tx_sink_packets, ack_prev.tx_sink_packets),
                (unsigned long long)counter_delta(ack.tx_sink_bytes, ack_prev.tx_sink_bytes),
                (unsigned long long)counter_delta(ack.pending_flush_pacing, ack_prev.pending_flush_pacing),
                (unsigned long long)counter_delta(ack.pending_flush_window_cwnd, ack_prev.pending_flush_window_cwnd),
                (unsigned long long)counter_delta(ack.pending_flush_fast_recovery_pipe,
                    ack_prev.pending_flush_fast_recovery_pipe),
                (unsigned long long)counter_delta(ack.pending_flush_packet_allocation,
                    ack_prev.pending_flush_packet_allocation),
                (unsigned long long)ndi_attempts, (unsigned long long)ndi_accepted,
                (unsigned long long)ndi_rejected);
        }
        ndi_prev_ = ndi_stats_;

        char admission_buf[1024] = {};
        if (send_admission_enabled_) {
            std::uint32_t blocked = 0;
            std::uint32_t non_sendable = 0;
            std::uint32_t sndbuf_quota = 0;
            std::uint32_t unknown = 0;
            std::uint64_t blocked_bytes = 0;
            std::uint64_t max_current_us = 0;
            bool have_snapshot = false;
            UInt32 attempt_min = 0, attempt_max = 0;
            UInt32 pending_min = 0, pending_max = 0;
            UInt32 inflight_min = 0, inflight_max = 0;
            UInt32 sndbuf_min = 0, sndbuf_max = 0;
            UInt32 sndwnd_min = 0, sndwnd_max = 0;
            UInt64 cwnd_min = 0, cwnd_max = 0;
            UInt64 pacing_due_min = 0, pacing_due_max = 0;
            const std::uint64_t now_us = NowUs();
            for (const auto& entry : flows_) {
                const std::shared_ptr<Flow>& flow = entry.second;
                if (!flow->send_admission_blocked) {
                    continue;
                }
                ++blocked;
                blocked_bytes += flow->pending_read.size();
                max_current_us = std::max(max_current_us,
                    now_us > flow->send_admission_blocked_since_us
                        ? now_us - flow->send_admission_blocked_since_us : 0);
                if (!flow->send_admission_snapshot_valid) {
                    ++unknown;
                    continue;
                }
                const ::xtcp::core::SendAdmissionSnapshot& snapshot =
                    flow->send_admission_snapshot;
                if (snapshot.reason == ::xtcp::core::SendAdmissionReason::kNonSendableState) {
                    ++non_sendable;
                }
                else if (snapshot.reason == ::xtcp::core::SendAdmissionReason::kSndBufQuota) {
                    ++sndbuf_quota;
                }
                else {
                    ++unknown;
                }
                const UInt64 pacing_due = snapshot.pacing_deadline > now_us
                    ? snapshot.pacing_deadline - now_us : 0;
                if (!have_snapshot) {
                    have_snapshot = true;
                    attempt_min = attempt_max = snapshot.attempted_len;
                    pending_min = pending_max = snapshot.pending_send;
                    inflight_min = inflight_max = snapshot.inflight;
                    sndbuf_min = sndbuf_max = snapshot.snd_buf;
                    sndwnd_min = sndwnd_max = snapshot.snd_wnd;
                    cwnd_min = cwnd_max = snapshot.cwnd_bytes;
                    pacing_due_min = pacing_due_max = pacing_due;
                    continue;
                }
                attempt_min = std::min(attempt_min, snapshot.attempted_len);
                attempt_max = std::max(attempt_max, snapshot.attempted_len);
                pending_min = std::min(pending_min, snapshot.pending_send);
                pending_max = std::max(pending_max, snapshot.pending_send);
                inflight_min = std::min(inflight_min, snapshot.inflight);
                inflight_max = std::max(inflight_max, snapshot.inflight);
                sndbuf_min = std::min(sndbuf_min, snapshot.snd_buf);
                sndbuf_max = std::max(sndbuf_max, snapshot.snd_buf);
                sndwnd_min = std::min(sndwnd_min, snapshot.snd_wnd);
                sndwnd_max = std::max(sndwnd_max, snapshot.snd_wnd);
                cwnd_min = std::min(cwnd_min, snapshot.cwnd_bytes);
                cwnd_max = std::max(cwnd_max, snapshot.cwnd_bytes);
                pacing_due_min = std::min(pacing_due_min, pacing_due);
                pacing_due_max = std::max(pacing_due_max, pacing_due);
            }
            std::snprintf(admission_buf, sizeof(admission_buf),
                "\"admission\":{\"blocked\":%u,\"bytes\":%llu,\"non_sendable\":%u,"
                "\"sndbuf_quota\":%u,\"unknown\":%u,\"max_current_ms\":%.3f,"
                "\"attempt_min\":%u,\"attempt_max\":%u,\"pending_min\":%u,\"pending_max\":%u,"
                "\"inflight_min\":%u,\"inflight_max\":%u,\"sndbuf_min\":%u,\"sndbuf_max\":%u,"
                "\"sndwnd_min\":%u,\"sndwnd_max\":%u,\"cwnd_min\":%llu,\"cwnd_max\":%llu,"
                "\"pacing_due_min_us\":%llu,\"pacing_due_max_us\":%llu},",
                blocked, (unsigned long long)blocked_bytes, non_sendable, sndbuf_quota, unknown,
                static_cast<double>(max_current_us) / 1000.0,
                attempt_min, attempt_max, pending_min, pending_max, inflight_min, inflight_max,
                sndbuf_min, sndbuf_max, sndwnd_min, sndwnd_max,
                (unsigned long long)cwnd_min, (unsigned long long)cwnd_max,
                (unsigned long long)pacing_due_min, (unsigned long long)pacing_due_max);
        }

        char output_rejection_buf[2048] = {};
        if (output_rejection_enabled_) {
            const XtcpOutputRejectionSnapshot& output = now_prev.output_rejections;
            const XtcpOutputRejectionSnapshot& previous = p.output_rejections;
            const XtcpOutputRejectionPacketShape& shape = output.packet_shape;
            const XtcpOutputPreCallOversizeAttemptSnapshot& oversize =
                output.first_oversize_output_attempt;
            const XtcpOutputRejectionPacketShape& oversize_shape = oversize.packet_shape;
            std::snprintf(output_rejection_buf, sizeof(output_rejection_buf),
                "\"output_rejection\":{\"weak_owner_expired\":%llu,\"vethernet_disposed\":%llu,"
                "\"tap_missing\":%llu,\"output_rejected\":%llu,\"accepted\":%llu,"
                "\"first_actual_output_rejected_monotonic_ns\":%llu,\"packet_shape\":{"
                "\"captured\":%s,\"parsed\":%s,\"supplied_bytes\":%u,"
                "\"ipv4_total_length\":%u,\"ipv4_ihl\":%u,\"tcp_data_offset\":%u,"
                "\"tcp_payload_length\":%u,\"tcp_flags\":%u,"
                "\"tcp_option_timestamps\":%s,\"tcp_option_sack\":%s,"
                "\"tcp_option_md5\":%s,\"tcp_option_unknown\":%s},"
                "\"first_oversize_output_attempt\":{\"first_monotonic_ns\":%llu,\"packet_shape\":{"
                "\"captured\":%s,\"parsed\":%s,\"supplied_bytes\":%u,"
                "\"ipv4_total_length\":%u,\"ipv4_ihl\":%u,\"tcp_data_offset\":%u,"
                "\"tcp_payload_length\":%u,\"tcp_flags\":%u,"
                "\"tcp_option_timestamps\":%s,\"tcp_option_sack\":%s,"
                "\"tcp_option_md5\":%s,\"tcp_option_unknown\":%s}}},",
                (unsigned long long)counter_delta(output.weak_owner_expired, previous.weak_owner_expired),
                (unsigned long long)counter_delta(output.vethernet_disposed, previous.vethernet_disposed),
                (unsigned long long)counter_delta(output.tap_missing, previous.tap_missing),
                (unsigned long long)counter_delta(output.output_rejected, previous.output_rejected),
                (unsigned long long)counter_delta(output.accepted, previous.accepted),
                (unsigned long long)output.first_actual_output_rejected_monotonic_ns,
                shape.captured ? "true" : "false", shape.parsed ? "true" : "false",
                (unsigned)shape.supplied_bytes, (unsigned)shape.ipv4_total_length,
                (unsigned)shape.ipv4_ihl, (unsigned)shape.tcp_data_offset,
                (unsigned)shape.tcp_payload_length, (unsigned)shape.tcp_flags,
                shape.tcp_option_timestamps ? "true" : "false",
                shape.tcp_option_sack ? "true" : "false",
                shape.tcp_option_md5 ? "true" : "false",
                shape.tcp_option_unknown ? "true" : "false",
                (unsigned long long)oversize.first_monotonic_ns,
                oversize_shape.captured ? "true" : "false", oversize_shape.parsed ? "true" : "false",
                (unsigned)oversize_shape.supplied_bytes, (unsigned)oversize_shape.ipv4_total_length,
                (unsigned)oversize_shape.ipv4_ihl, (unsigned)oversize_shape.tcp_data_offset,
                (unsigned)oversize_shape.tcp_payload_length, (unsigned)oversize_shape.tcp_flags,
                oversize_shape.tcp_option_timestamps ? "true" : "false",
                oversize_shape.tcp_option_sack ? "true" : "false",
                oversize_shape.tcp_option_md5 ? "true" : "false",
                oversize_shape.tcp_option_unknown ? "true" : "false");
        }

        char line[5120];
        char ndi_buf[3072];
        std::snprintf(ndi_buf, sizeof(ndi_buf),
            "%s%s%s\"ndi\":{\"pps\":%llu,\"out_p50_us\":%.0f,\"out_p95_us\":%.0f,\"out_p99_us\":%.0f,"
            "\"iv_p50_us\":%.0f,\"iv_p95_us\":%.0f,\"batch_avg\":%.1f,\"batch_max\":%u},",
            ack_release_buf, admission_buf, output_rejection_buf, (unsigned long long)ndi_pps_,
            ndi_out_p50_us_, ndi_out_p95_us_, ndi_out_p99_us_,
            ndi_iv_p50_us_, ndi_iv_p95_us_,
            ndi_batch_avg_, (unsigned)ndi_batch_max_);
        std::snprintf(line, sizeof(line),
            "{\"send\":{\"calls\":%llu,\"rejected\":%llu,\"stall_ms\":%.3f,\"events\":%llu},"
            "\"conn\":{\"rd_calls\":%llu,\"rd_bytes\":%llu,\"avg_rd\":%.0f,"
            "\"wr_ops\":%llu,\"wr_bytes\":%llu,\"avg_wr\":%.0f,\"q_high\":%llu,"
            "\"wr_cyc_p50_us\":%.0f,\"wr_cyc_p95_us\":%.0f,"
            "\"wr_gap_p50_us\":%.0f,\"wr_gap_p95_us\":%.0f,"
            "\"rej\":%llu,\"rej_bytes\":%llu},"
            "\"recv\":{\"calls\":%llu,\"bytes\":%llu,\"avg_seg\":%.0f},"
            "\"out\":{\"pkts\":%llu,\"bytes\":%llu,\"avg_pkt\":%.0f},"
            "\"tcp\":{\"cwnd_mss\":%u,\"inflight\":%u,\"snd_wnd\":%u,\"ssthresh\":%u,"
            "\"retx\":%u,\"dup_acks\":%u,\"fast_rec\":%u},"
            "\"owner\":{\"posts\":%llu,\"dispatched\":%llu,\"dropped\":%llu,\"injected\":%llu,"
            "\"q_p50_us\":%.0f,\"q_p95_us\":%.0f},"
            "%s"
            "\"queue\":{\"global\":%llu,\"global_high\":%llu,"
            "\"above_256k\":%u,\"above_1m\":%u,\"above_2m\":%u},"
            "\"timer\":{\"armed\":%llu,\"late_n\":%llu,\"late_p50_us\":%.0f,\"late_p95_us\":%.0f}}",
            (unsigned long long)send_calls,
            (unsigned long long)send_rejected,
            static_cast<double>(stall_us) / 1000.0,
            (unsigned long long)(now_prev.send_stall_events >= p.send_stall_events
                ? now_prev.send_stall_events - p.send_stall_events : 0),
            (unsigned long long)rd_calls, (unsigned long long)rd_bytes,
            rd_calls != 0 ? static_cast<double>(rd_bytes) / rd_calls : 0.0,
            (unsigned long long)wr_ops, (unsigned long long)wr_bytes,
            wr_ops != 0 ? static_cast<double>(wr_bytes) / wr_ops : 0.0,
            (unsigned long long)stats_.write_queue_highwater.load(std::memory_order_relaxed),
            HistPercentile(cycle_hist, c_total, 0.50),
            HistPercentile(cycle_hist, c_total, 0.95),
            HistPercentile(gap_hist, g_total, 0.50),
            HistPercentile(gap_hist, g_total, 0.95),
            (unsigned long long)(now_prev.on_receive_rejected >= p.on_receive_rejected
                ? now_prev.on_receive_rejected - p.on_receive_rejected : 0),
            (unsigned long long)(now_prev.on_receive_rejected_bytes >= p.on_receive_rejected_bytes
                ? now_prev.on_receive_rejected_bytes - p.on_receive_rejected_bytes : 0),
            (unsigned long long)recv_calls, (unsigned long long)recv_bytes,
            recv_calls != 0 ? static_cast<double>(recv_bytes) / recv_calls : 0.0,
            (unsigned long long)out_pkts, (unsigned long long)out_bytes,
            out_pkts != 0 ? static_cast<double>(out_bytes) / out_pkts : 0.0,
            (unsigned long long)(now_prev.ingress_enqueued >= p.ingress_enqueued
                ? now_prev.ingress_enqueued - p.ingress_enqueued : 0),
            (unsigned long long)(now_prev.ingress_dispatched >= p.ingress_dispatched
                ? now_prev.ingress_dispatched - p.ingress_dispatched : 0),
            (unsigned long long)(now_prev.ingress_dropped >= p.ingress_dropped
                ? now_prev.ingress_dropped - p.ingress_dropped : 0),
            (unsigned long long)(now_prev.ingress_injected >= p.ingress_injected
                ? now_prev.ingress_injected - p.ingress_injected : 0),
            tcp_sample_.cwnd, tcp_sample_.inflight, tcp_sample_.snd_wnd,
            tcp_sample_.ssthresh, tcp_sample_.retx, tcp_sample_.dup_acks,
            tcp_sample_.fast_rec,
            HistPercentile(queue_hist, q_total, 0.50),
            HistPercentile(queue_hist, q_total, 0.95),
            ndi_buf,
            (unsigned long long)stats_.queued_bytes_total.load(std::memory_order_relaxed),
            (unsigned long long)stats_.queued_bytes_highwater.load(std::memory_order_relaxed),
            flows_above_256k_, flows_above_1m_, flows_above_2m_,
            (unsigned long long)(now_prev.timer_armed >= p.timer_armed
                ? now_prev.timer_armed - p.timer_armed : 0),
            (unsigned long long)l_total,
            HistPercentile(late_hist, l_total, 0.50),
            HistPercentile(late_hist, l_total, 0.95));
        perf_out_ << line << '\n';
        perf_out_.flush();
        perf_prev_ = now_prev;
    }

#if defined(PPP_XTCP_HAS_TIMER_DEADLINE)
    // Deadline-driven polling: the timer is armed for the stack's next timer
    // deadline instead of a fixed cadence. Stack-mutating paths call
    // KickPoll() so a freshly armed earlier deadline preempts a pending wait;
    // the idle watchdog bounds the wait when nothing is armed.
    static constexpr std::uint64_t kIdlePollIntervalMs = 10;

    std::chrono::steady_clock::duration PollDelay() const noexcept {
        if (!stack_) {
            return std::chrono::milliseconds(1);
        }
        const UInt64 due = stack_->NextTimerDeadlineUs();
        if (due == ::xtcp::XtcpStack::kNoTimerDeadline) {
            return std::chrono::milliseconds(kIdlePollIntervalMs);
        }
        const std::uint64_t now = NowUs();
        return due <= now
            ? std::chrono::microseconds(0)
            : std::chrono::microseconds(due - now);
    }

    void KickPoll() noexcept {
        if (!running_.load(std::memory_order_acquire) || !stack_ || !poll_timer_) {
            return;
        }
        const std::chrono::steady_clock::time_point target =
            std::chrono::steady_clock::now() + PollDelay();
        if (target < poll_timer_->expiry()) {
            boost::system::error_code ec;
            poll_timer_->cancel(ec);
            SchedulePoll(generation_.load(std::memory_order_acquire));
        }
    }
#else
    void KickPoll() noexcept {}
#endif

    void SchedulePoll(std::uint64_t runtime_generation) noexcept {
        if (!IsCurrent(runtime_generation)) {
            return;
        }
        poll_timer_ = std::make_shared<boost::asio::steady_timer>(*context_);
#if defined(PPP_XTCP_HAS_TIMER_DEADLINE)
        poll_timer_->expires_after(PollDelay());
#else
        poll_timer_->expires_after(std::chrono::milliseconds(1));
#endif
        stats_.timer_armed.fetch_add(1, std::memory_order_relaxed);
        const std::shared_ptr<Impl> self = shared_from_this();
        poll_timer_->async_wait(boost::asio::bind_executor(*strand_,
            [self, runtime_generation](const boost::system::error_code& ec) noexcept {
                if (ec || !self->IsCurrent(runtime_generation) || !self->stack_) {
                    return;
                }
                if (self->poll_timer_) {
                    const auto overdue = std::chrono::steady_clock::now() -
                        self->poll_timer_->expiry();
                    if (overdue.count() > 0) {
                        HistAdd(self->stats_.timer_late_us,
                            static_cast<std::uint64_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    overdue).count()));
                    }
                }
                self->stats_.timer_polls.fetch_add(1, std::memory_order_relaxed);
                self->stats_.timer_events.fetch_add(
                    self->stack_->PollAckTimers(), std::memory_order_relaxed);
                self->SchedulePoll(runtime_generation);
            }));
    }

    void DoStop() noexcept {
        if (perf_dump_timer_) {
            boost::system::error_code ec;
            perf_dump_timer_->cancel(ec);
            perf_dump_timer_.reset();
        }
        if (perf_out_.is_open()) {
            perf_out_.flush();
            perf_out_.close();
        }
        if (poll_timer_) {
            boost::system::error_code ec;
            poll_timer_->cancel(ec);
            poll_timer_.reset();
        }
        while (!flows_.empty()) {
            CloseFlow(flows_.begin()->second, true);
        }
        connections_.clear();
        listener_refs_.clear();
        stack_.reset();
        if (backend_) {
            backend_->Stop();
            backend_.reset();
        }
        lease_.reset();
        stopping_.store(false, std::memory_order_release);
    }

private:
    std::shared_ptr<boost::asio::io_context> context_;
    std::shared_ptr<Strand> strand_;
    OutputHandler output_;
    OutputHandler counted_output_;
    ListenerEndpointHandler listener_endpoint_;
    ExternalAcceptHandler external_accept_;
    ExternalCancelHandler external_cancel_;
    std::shared_ptr<XtcpOutputRejectionDiagnostics> output_rejection_diagnostics_;
    mutable std::mutex state_sync_;
    XtcpIngressBudget budget_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> generation_{0};
    std::uint64_t next_flow_generation_ = 0;
    Stats stats_;
    std::unique_ptr<XtcpPoolLease> lease_;
    std::unique_ptr<XtcpNdiBackend> backend_;
    std::unique_ptr<::xtcp::XtcpStack> stack_;
    std::shared_ptr<boost::asio::steady_timer> poll_timer_;
    // Perf diagnostics state (strand-only once Start() armed the dump).
    std::string perf_json_path_;
    bool send_admission_enabled_ = false;
    bool ack_release_enabled_ = false;
    bool output_rejection_enabled_ = false;
    std::shared_ptr<boost::asio::steady_timer> perf_dump_timer_;
    std::ofstream perf_out_;
    PerfPrev perf_prev_;
    struct TcpSample final {
        UInt32 inflight = 0;
        UInt32 cwnd = 0;
        UInt32 ssthresh = 0;
        UInt32 snd_wnd = 0;
        UInt32 retx = 0;
        UInt32 dup_acks = 0;
        UInt32 fast_rec = 0;
        UInt32 front_seq = 0;
        UInt32 snd_una = 0;
        UInt16 local_port = 0;
        UInt16 remote_port = 0;
        UInt64 rto_deadline = 0;
    };
    TcpSample tcp_sample_;
    XtcpNdiBackend::TxStats ndi_stats_;
    XtcpNdiBackend::TxStats ndi_prev_;
    // Per-interval NDI aggregates written into the JSON line.
    std::uint64_t ndi_pps_ = 0;
    double ndi_out_p50_us_ = 0.0;
    double ndi_out_p95_us_ = 0.0;
    double ndi_out_p99_us_ = 0.0;
    double ndi_iv_p50_us_ = 0.0;
    double ndi_iv_p95_us_ = 0.0;
    double ndi_batch_avg_ = 0.0;
    std::uint32_t ndi_batch_max_ = 0;
    std::uint32_t flows_above_256k_ = 0;
    std::uint32_t flows_above_1m_ = 0;
    std::uint32_t flows_above_2m_ = 0;
    std::unordered_map<FlowKey, std::shared_ptr<Flow>, FlowKeyHash> flows_;
    std::unordered_map<UInt64, FlowKey> connections_;
    std::unordered_map<UInt64, std::pair<::xtcp::core::Endpoint, std::uint32_t>> listener_refs_;
};

XtcpRuntime::XtcpRuntime(
    const std::shared_ptr<boost::asio::io_context>& context,
    OutputHandler output,
    ListenerEndpointHandler listener_endpoint,
    ExternalAcceptHandler external_accept,
    ExternalCancelHandler external_cancel,
    std::shared_ptr<XtcpOutputRejectionDiagnostics> output_rejection_diagnostics) noexcept {
    try {
        impl_ = std::make_shared<Impl>(context, std::move(output),
            std::move(listener_endpoint), std::move(external_accept),
            std::move(external_cancel), std::move(output_rejection_diagnostics));
    }
    catch (...) {}
}

XtcpRuntime::~XtcpRuntime() noexcept {
    Stop();
}

bool XtcpRuntime::Start() noexcept { return impl_ && impl_->Start(); }
void XtcpRuntime::MarkReady() noexcept { if (impl_) impl_->MarkReady(); }
void XtcpRuntime::Stop() noexcept { if (impl_) impl_->Stop(); }
bool XtcpRuntime::SubmitIPv4Tcp(const void* packet, int packet_length) noexcept {
    return impl_ && impl_->Submit(packet, packet_length);
}
bool XtcpRuntime::IsReady() const noexcept { return impl_ && impl_->IsReady(); }
bool XtcpRuntime::IsRunning() const noexcept { return impl_ && impl_->IsRunning(); }
std::uint64_t XtcpRuntime::Generation() const noexcept { return impl_ ? impl_->Generation() : 0; }
ppp::app::runtime::RuntimeXtcpStats XtcpRuntime::SnapshotStats() const noexcept {
    return impl_ ? impl_->SnapshotStats() : ppp::app::runtime::RuntimeXtcpStats{};
}
#if defined(PPP_XTCP_RUNTIME_TESTING)
bool XtcpRuntime::EmitOutputForTesting(const void* data, int length) noexcept {
    return impl_ && impl_->EmitOutputForTesting(data, length);
}
#endif
#endif

} // namespace ppp::app::client::xtcp
