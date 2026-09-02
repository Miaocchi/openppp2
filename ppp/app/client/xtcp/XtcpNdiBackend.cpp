#include <ppp/app/client/xtcp/XtcpNdiBackend.h>

#if defined(PPP_ENABLE_XTCP)
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

namespace ppp::app::client::xtcp {
namespace {
void HistAdd(std::atomic<std::uint64_t>* hist, std::uint64_t us) noexcept {
    std::size_t bucket = 0;
    std::uint64_t value = us;
    while (value > 1 && bucket + 1 < 32) {
        value >>= 1;
        ++bucket;
    }
    hist[bucket].fetch_add(1, std::memory_order_relaxed);
}
} // namespace

XtcpNdiBackend::XtcpNdiBackend(OutputHandler output) noexcept
    : output_(std::move(output)) {}

bool XtcpNdiBackend::Tx(::xtcp::ndi::Packet&& packet) noexcept {
    attempts_.fetch_add(1, std::memory_order_relaxed);
    OutputHandler output;
    {
        std::lock_guard<std::mutex> lock(sync_);
        if (stopped_ || !output_) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        output = output_;
    }
    // A2-0 diagnostics: per-packet Output() wall time plus the interval since
    // the previous Tx entry (the NDI callback cadence).
    const std::uint64_t start_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    bool emitted = false;
    if (packet.data != nullptr && packet.len != 0) {
        if (!packet.owned.IsEmpty()) {
            // XTCP-STRAND-DISPATCH-001 (data-plane tier): hand the stack's
            // BufRef to the consumer through an owning shared_ptr; the buffer
            // returns to the pool when the TAP write completes. No alloc and
            // no copy on this path. Ownership is RESTORED to packet.owned
            // when the consumer rejects: a rejected Tx must leave the packet
            // exactly as it arrived (contract of TestProductionNdiOwnership).
            struct BufRefHolder final {
                ::xtcp::buf::BufRef ref;
            };
            const std::shared_ptr<BufRefHolder> holder =
                std::make_shared<BufRefHolder>();
            holder->ref = std::move(packet.owned);
            std::shared_ptr<Byte> buffer(holder->ref.Data(),
                [holder](Byte*) mutable noexcept {});
            emitted = output(std::move(buffer), static_cast<int>(packet.len));
            if (!emitted) {
                packet.owned = std::move(holder->ref);
            }
        }
        else {
            // Defensive: a Tx packet without a backing BufRef cannot be handed
            // off zero-copy; copy once so the consumer still owns the bytes.
            const std::shared_ptr<std::vector<Byte>> holder =
                std::make_shared<std::vector<Byte>>(packet.data, packet.data + packet.len);
            std::shared_ptr<Byte> buffer(holder, holder->data());
            emitted = output(std::move(buffer), static_cast<int>(packet.len));
        }
    }
    const std::uint64_t end_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    tx_calls_.fetch_add(1, std::memory_order_relaxed);
    if (packet.data != nullptr) {
        tx_bytes_.fetch_add(packet.len, std::memory_order_relaxed);
    }
    HistAdd(output_us_, end_us > start_us ? end_us - start_us : 0);
    if (last_tx_start_us_ != 0) {
        HistAdd(interval_us_, start_us > last_tx_start_us_ ? start_us - last_tx_start_us_ : 0);
    }
    last_tx_start_us_ = start_us;
    if (!emitted) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    accepted_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

UInt32 XtcpNdiBackend::TxBatch(::xtcp::ndi::Packet* packets, UInt32 count) noexcept {
    if (packets == nullptr) {
        return 0;
    }
    batch_calls_.fetch_add(1, std::memory_order_relaxed);
    batch_packets_.fetch_add(count, std::memory_order_relaxed);
    std::uint32_t prev_max = batch_max_.load(std::memory_order_relaxed);
    while (count > prev_max &&
           !batch_max_.compare_exchange_weak(prev_max, count, std::memory_order_relaxed)) {
    }
    UInt32 accepted = 0;
    for (; accepted < count; ++accepted) {
        if (!Tx(std::move(packets[accepted]))) {
            break;
        }
    }
    return accepted;
}

void XtcpNdiBackend::SetRxHandler(::xtcp::ndi::RxHandler handler) noexcept {
    std::lock_guard<std::mutex> lock(sync_);
    if (!stopped_) {
        rx_handler_ = std::move(handler);
    }
}

::xtcp::ndi::BackendCaps XtcpNdiBackend::Caps() const noexcept {
    return ::xtcp::ndi::kCapNone;
}

bool XtcpNdiBackend::Inject(::xtcp::buf::BufRef&& packet) noexcept {
    ::xtcp::ndi::RxHandler handler;
    {
        std::lock_guard<std::mutex> lock(sync_);
        if (stopped_ || !rx_handler_ || packet.IsEmpty()) {
            return false;
        }
        handler = rx_handler_;
    }
    ::xtcp::ndi::Packet input;
    input.data = packet.Data();
    input.len = packet.Len();
    input.eth_type = 0x0800;
    input.owned = std::move(packet);
    handler(std::move(input));
    return true;
}

XtcpNdiBackend::TxStats XtcpNdiBackend::SnapshotTxStats() const noexcept {
    TxStats stats;
    stats.tx_calls = tx_calls_.load(std::memory_order_relaxed);
    stats.tx_bytes = tx_bytes_.load(std::memory_order_relaxed);
    stats.attempts = attempts_.load(std::memory_order_relaxed);
    stats.accepted = accepted_.load(std::memory_order_relaxed);
    stats.rejected = rejected_.load(std::memory_order_relaxed);
    stats.batch_calls = batch_calls_.load(std::memory_order_relaxed);
    stats.batch_packets = batch_packets_.load(std::memory_order_relaxed);
    stats.batch_max = batch_max_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < 32; ++i) {
        stats.output_us[i] = output_us_[i].load(std::memory_order_relaxed);
        stats.interval_us[i] = interval_us_[i].load(std::memory_order_relaxed);
    }
    return stats;
}

void XtcpNdiBackend::Stop() noexcept {
    std::lock_guard<std::mutex> lock(sync_);
    stopped_ = true;
    rx_handler_ = nullptr;
    output_ = nullptr;
}

} // namespace ppp::app::client::xtcp
#endif
