#pragma once

#include <functional>
#include <mutex>

#if defined(PPP_ENABLE_XTCP)
#include <array>
#include <atomic>
#include <cstdint>
#include <xtcp/ndi.h>
#endif

namespace ppp::app::client::xtcp {

#if defined(PPP_ENABLE_XTCP)
class XtcpNdiBackend final : public ::xtcp::ndi::Backend {
public:
    using OutputHandler = std::function<bool(const void*, int)>;

    explicit XtcpNdiBackend(OutputHandler output) noexcept;

    bool Tx(::xtcp::ndi::Packet&& packet) noexcept override;
    UInt32 TxBatch(::xtcp::ndi::Packet* packets, UInt32 count) noexcept override;
    void SetRxHandler(::xtcp::ndi::RxHandler handler) noexcept override;
    ::xtcp::ndi::BackendCaps Caps() const noexcept override;

    bool Inject(::xtcp::buf::BufRef&& packet) noexcept;
    void Stop() noexcept;

    /** @brief Snapshot of the A2-0 output-path diagnostics (cumulative). */
    struct TxStats final {
        std::uint64_t tx_calls = 0;
        std::uint64_t tx_bytes = 0;
        std::uint64_t attempts = 0;
        std::uint64_t accepted = 0;
        std::uint64_t rejected = 0;
        std::uint64_t batch_calls = 0;
        std::uint64_t batch_packets = 0;
        std::uint32_t batch_max = 0;
        // log2(us) histograms: bucket b counts samples in [2^b, 2^(b+1)).
        std::uint64_t output_us[32] = {};
        std::uint64_t interval_us[32] = {};
    };
    TxStats SnapshotTxStats() const noexcept;

private:
    mutable std::mutex sync_;
    OutputHandler output_;
    ::xtcp::ndi::RxHandler rx_handler_;
    bool stopped_ = false;
    // A2-0 output-path diagnostics. Written on the stack owner thread only;
    // read via SnapshotTxStats().
    std::atomic<std::uint64_t> tx_calls_{0};
    std::atomic<std::uint64_t> tx_bytes_{0};
    std::atomic<std::uint64_t> attempts_{0};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> batch_calls_{0};
    std::atomic<std::uint64_t> batch_packets_{0};
    std::atomic<std::uint32_t> batch_max_{0};
    std::atomic<std::uint64_t> output_us_[32]{};
    std::atomic<std::uint64_t> interval_us_[32]{};
    std::uint64_t last_tx_start_us_ = 0;
};
#endif

} // namespace ppp::app::client::xtcp
