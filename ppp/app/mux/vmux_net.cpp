#include "vmux.h"
#include "vmux_net.h"
#include "vmux_skt.h"
#include <ppp/configurations/AppConfiguration.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <openssl/crypto.h>
#include <ppp/diagnostics/Error.h>
#include <ppp/diagnostics/Telemetry.h>

#include "ppp/collections/Dictionary.h"

/**
 * @file vmux_net.cpp
 * @brief Implements vmux network multiplexing, handshake, and packet forwarding.
 * @license GPL-3.0
 */

namespace vmux {
    namespace {
        /**
         * Unique payload a carrier must have moved before its byte counters
         * describe a usable framing ratio. A just-attached, ACK-only or
         * control-only link would otherwise divide a burst of handshake bytes
         * by a few kilobytes of payload; below this the ratio stays unset and
         * the pacing policy applies no cap.
         */
        constexpr std::uint64_t PathPacingWireRatioMinPayloadBytes = 256 * 1024;
        /** Fixed-point scale for the measured framing ratio (wire per payload). */
        constexpr std::uint64_t PathPacingWireRatioPermille = 1000;
        constexpr Byte IntentPathProbeMarker = 'I';
        constexpr Byte IntentPathProbeRequest = 'P';
        constexpr Byte IntentPathProbeResponse = 'A';

        ppp::app::multipath::PathEstimatorMode configured_path_estimator() noexcept {
            using Mode = ppp::app::multipath::PathEstimatorMode;
            const char* value = std::getenv("OPENPPP2_VMUX_PATH_ESTIMATOR");
            const std::string_view name = value ? value : "";
            if (name.empty() || name == "kcc") return Mode::Kcc;
            if (name == "bbr") return Mode::Bbr;
            return Mode::Ewma;
        }

        struct TraceWriter final {
            bool enabled = false;
            std::mutex mutex;
            std::ofstream output;

            explicit TraceWriter(const char* variable) noexcept {
                try {
                    const char* path = std::getenv(variable);
                    if (path && *path) {
                        output.open(path, std::ios::out | std::ios::app);
                        enabled = static_cast<bool>(output);
                    }
                }
                catch (...) {}
            }
        };

        TraceWriter& scheduler_trace_writer() noexcept {
            static TraceWriter writer("OPENPPP2_MULTIPATH_SCHEDULER_TRACE_JSON");
            return writer;
        }

        TraceWriter& flow_trace_writer() noexcept {
            static TraceWriter writer("OPENPPP2_MULTIPATH_FLOW_TRACE_JSON");
            return writer;
        }

        const char* scheduler_status_name(ppp::app::multipath::PathScheduler::Status status) noexcept {
            using Status = ppp::app::multipath::PathScheduler::Status;
            switch (status) {
            case Status::Selected: return "selected";
            case Status::NoEligible: return "no_eligible";
            case Status::Backpressure: return "backpressure";
            case Status::InvalidInput: return "invalid_input";
            }
            return "unknown";
        }

        const char* tcp_intent_name(ppp::app::mux::TcpIntent intent) noexcept {
            switch (intent) {
            case ppp::app::mux::TcpIntent::Latency: return "latency";
            case ppp::app::mux::TcpIntent::Bandwidth: return "bandwidth";
            default: return "unknown";
            }
        }

        /** @brief Microsecond monotonic clock used only while the scheduler trace is open. */
        std::uint64_t trace_now_us() noexcept {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        void trace_flow_open(const boost::asio::ip::tcp::socket& socket, std::uint32_t cid,
                             int destination_port) noexcept {
            auto& writer = flow_trace_writer();
            if (!writer.enabled) return;
            try {
                boost::system::error_code error;
                const auto peer = socket.remote_endpoint(error);
                if (error) return;
                std::lock_guard<std::mutex> lock(writer.mutex);
                writer.output << "{\"event\":\"open\",\"monotonic_us\":" << trace_now_us()
                              << ",\"cid\":" << cid << ",\"peer_port\":" << peer.port()
                              << ",\"destination_port\":" << destination_port << "}" << std::endl;
            }
            catch (...) {}
        }

        void trace_flow_delivery(std::uint32_t cid, const std::string& path, int bytes) noexcept {
            auto& writer = flow_trace_writer();
            if (!writer.enabled) return;
            try {
                std::lock_guard<std::mutex> lock(writer.mutex);
                writer.output << "{\"event\":\"delivery\",\"monotonic_us\":" << trace_now_us()
                              << ",\"cid\":" << cid << ",\"path_id\":" << Json::valueToQuotedString(path.c_str())
                              << ",\"bytes\":" << bytes << "}" << std::endl;
            }
            catch (...) {}
        }

        void trace_flow_classification(std::uint32_t cid, ppp::app::mux::TcpIntent intent) noexcept {
            auto& writer = flow_trace_writer();
            if (!writer.enabled) return;
            try {
                std::lock_guard<std::mutex> lock(writer.mutex);
                writer.output << "{\"event\":\"intent\",\"cid\":" << cid
                              << ",\"intent\":\"" << tcp_intent_name(intent) << "\"}" << std::endl;
            }
            catch (...) {}
        }

        /** @brief Relaxed atomic max; writers are concurrent carrier strands while tracing. */
        void trace_note_max(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
            std::uint64_t current = target.load(std::memory_order_relaxed);
            while (value > current &&
                !target.compare_exchange_weak(current, value,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {
            }
        }

        /**
         * @brief One per-link latency/backpressure sample drained from TraceWindow.
         */
        struct SchedulerTraceLinkSample final {
            std::uint16_t link_id = 0;
            std::uint64_t generation = 0;
            std::uint64_t tx_write_count = 0;
            std::uint64_t tx_post_wait_us_sum = 0;
            std::uint64_t tx_post_wait_us_max = 0;
            std::uint64_t tx_write_us_sum = 0;
            std::uint64_t tx_write_us_max = 0;
            std::uint64_t rx_dispatch_count = 0;
            std::uint64_t rx_dispatch_us_sum = 0;
            std::uint64_t rx_dispatch_us_max = 0;
            std::uint64_t rx_handler_us_sum = 0;
            std::uint64_t rx_handler_us_max = 0;
            std::uint64_t rx_backpressure_count = 0;
            std::uint64_t queued_bytes = 0;
            std::uint64_t wire_inflight_bytes = 0;
            bool rx_paused = false;
        };

        /**
         * @brief Records one carrier write: queue-to-write post wait plus write latency.
         * @param link Carrier whose TraceWindow receives the sample (kept alive by the
         *             caller's shared_ptr capture; the counters are atomics so a late
         *             completion after carrier retirement is still safe).
         * @param post_us Steady-clock stamp taken before posting the write to the carrier strand.
         * @param write_start_us Stamp taken on the carrier strand immediately before Write().
         * @param done_us Stamp taken in the write completion callback.
         */
        void trace_record_tx_write(vmux_net::vmux_linklayer& link,
            std::uint64_t post_us, std::uint64_t write_start_us, std::uint64_t done_us) noexcept {
            const std::uint64_t post_wait = write_start_us >= post_us ? write_start_us - post_us : 0;
            const std::uint64_t write_us = done_us >= write_start_us ? done_us - write_start_us : 0;
            auto& window = link.trace_window;
            window.tx_write_count.fetch_add(1, std::memory_order_relaxed);
            window.tx_post_wait_us_sum.fetch_add(post_wait, std::memory_order_relaxed);
            window.tx_write_us_sum.fetch_add(write_us, std::memory_order_relaxed);
            trace_note_max(window.tx_post_wait_us_max, post_wait);
            trace_note_max(window.tx_write_us_max, write_us);
        }

        /**
         * @brief Records one inbound frame: carrier-read to vmux-strand entry plus handler time.
         */
        void trace_record_rx_frame(vmux_net::vmux_linklayer& link,
            std::uint64_t read_done_us, std::uint64_t handler_start_us, std::uint64_t handler_end_us) noexcept {
            const std::uint64_t dispatch_us = handler_start_us >= read_done_us ? handler_start_us - read_done_us : 0;
            const std::uint64_t handler_us = handler_end_us >= handler_start_us ? handler_end_us - handler_start_us : 0;
            auto& window = link.trace_window;
            window.rx_dispatch_count.fetch_add(1, std::memory_order_relaxed);
            window.rx_dispatch_us_sum.fetch_add(dispatch_us, std::memory_order_relaxed);
            window.rx_handler_us_sum.fetch_add(handler_us, std::memory_order_relaxed);
            trace_note_max(window.rx_dispatch_us_max, dispatch_us);
            trace_note_max(window.rx_handler_us_max, handler_us);
        }

        /** @brief Counts one receive-backpressure pause on a carrier while tracing. */
        void trace_note_rx_backpressure(vmux_net::vmux_linklayer& link) noexcept {
            link.trace_window.rx_backpressure_count.fetch_add(1, std::memory_order_relaxed);
        }

        /** @brief Drains one carrier's TraceWindow into a sample and resets the window. */
        void trace_sample_link(vmux_net::vmux_linklayer& link, SchedulerTraceLinkSample& sample) noexcept {
            auto& window = link.trace_window;
            sample.link_id = link.id_;
            sample.generation = link.generation_;
            sample.tx_write_count = window.tx_write_count.exchange(0, std::memory_order_relaxed);
            sample.tx_post_wait_us_sum = window.tx_post_wait_us_sum.exchange(0, std::memory_order_relaxed);
            sample.tx_post_wait_us_max = window.tx_post_wait_us_max.exchange(0, std::memory_order_relaxed);
            sample.tx_write_us_sum = window.tx_write_us_sum.exchange(0, std::memory_order_relaxed);
            sample.tx_write_us_max = window.tx_write_us_max.exchange(0, std::memory_order_relaxed);
            sample.rx_dispatch_count = window.rx_dispatch_count.exchange(0, std::memory_order_relaxed);
            sample.rx_dispatch_us_sum = window.rx_dispatch_us_sum.exchange(0, std::memory_order_relaxed);
            sample.rx_dispatch_us_max = window.rx_dispatch_us_max.exchange(0, std::memory_order_relaxed);
            sample.rx_handler_us_sum = window.rx_handler_us_sum.exchange(0, std::memory_order_relaxed);
            sample.rx_handler_us_max = window.rx_handler_us_max.exchange(0, std::memory_order_relaxed);
            sample.rx_backpressure_count = window.rx_backpressure_count.exchange(0, std::memory_order_relaxed);
            sample.queued_bytes = static_cast<std::uint64_t>(link.queued_bytes_);
            sample.rx_paused = link.rx_paused_.load(std::memory_order_relaxed);
        }

        void write_scheduler_trace(std::uint64_t session_id, std::uint64_t sequence,
                                   std::uint64_t now, int length, bool new_payload,
                                   std::uint16_t avoid_link,
                                   ppp::app::mux::TcpIntent intent,
                                   const ppp::app::multipath::PathScheduler::Selection& selected,
                                   const std::vector<ppp::app::multipath::PathScheduler::Candidate>& candidates,
                                   const std::vector<SchedulerTraceLinkSample>& link_samples,
                                   std::uint64_t reorder_buffered_bytes,
                                   std::uint64_t reorder_events,
                                   std::uint64_t rx_paused_links,
                                   std::uint64_t tx_queued_frames,
                                   std::uint64_t tx_queued_bytes,
                                   std::uint64_t tx_ctrl_frames,
                                   const ppp::app::multipath::PathScheduler& scheduler) noexcept {
            auto& writer = scheduler_trace_writer();
            if (!writer.enabled) return;
            const auto probe_runtime = scheduler.GetProbeRuntime();
            try {
                std::lock_guard<std::mutex> lock(writer.mutex);
                writer.output << "{\"type\":\"multipath-scheduler\",\"session_id\":"
                    << session_id << ",\"sequence\":" << sequence
                    << ",\"monotonic_ms\":" << now
                    << ",\"payload_bytes\":" << (length < 0 ? 0 : length)
                    << ",\"new_payload\":" << (new_payload ? "true" : "false")
                    << ",\"avoid_link\":" << avoid_link
                    << ",\"intent\":\"" << tcp_intent_name(intent) << "\""
                    << ",\"status\":\"" << scheduler_status_name(selected.status) << "\""
                    << ",\"selected_link_id\":" << selected.carrier_id
                    << ",\"selected_probe\":" << (selected.probe ? "true" : "false")
                    << ",\"selected_estimated_arrival_ms\":" << selected.estimated_arrival_ms
                    << ",\"selected_send_credit_bytes\":" << selected.selected_send_credit_bytes
                    << ",\"selected_queue_bytes\":" << selected.selected_queue_bytes
                    << ",\"selected_wire_inflight_bytes\":" << selected.selected_wire_inflight_bytes
                    << ",\"selected_probe_budget_bytes\":" << selected.probe_budget_bytes
                    << ",\"selected_probe_remaining_bytes\":" << selected.probe_remaining_bytes
                    << ",\"probe_interval_ms\":" << probe_runtime.interval_ms
                    << ",\"last_probe_ms\":" << probe_runtime.last_probe_ms
                    << ",\"probe_clock_started\":" << (probe_runtime.clock_started ? "true" : "false")
                    << ",\"probe_owner_active\":" << (probe_runtime.owner_active ? "true" : "false")
                    << ",\"probe_owner_id\":" << probe_runtime.owner_id
                    << ",\"candidates\":[";
                for (std::size_t index = 0; index < candidates.size(); ++index) {
                    if (index != 0) writer.output << ",";
                    const auto& candidate = candidates[index];
                    const auto diagnostic = scheduler.GetProbeDiagnostics(candidate);
                    writer.output << "{\"link_id\":" << candidate.id
                        << ",\"path_id\":\"" << candidate.path_id << "\""
                        << ",\"generation\":" << candidate.generation
                        << ",\"eligible\":" << (candidate.eligible ? "true" : "false")
                        << ",\"state\":" << static_cast<unsigned>(candidate.state)
                        << ",\"send_credit_bytes\":" << candidate.send_credit_bytes.value_or(0)
                        << ",\"queue_bytes\":" << candidate.queue_bytes
                        << ",\"wire_inflight_bytes\":" << candidate.unacked_bytes
                        << ",\"sent_payload_bytes\":" << candidate.sent_payload_bytes
                        << ",\"rtt_ms\":" << candidate.rtt_ms.value_or(0)
                        << ",\"throughput_bytes_per_second\":" << candidate.throughput_bytes_per_second.value_or(0)
                        << ",\"scheduling_weight\":" << candidate.scheduling_weight
                        << ",\"probe_eligible\":" << (candidate.probe_eligible ? "true" : "false")
                        << ",\"at_capacity\":" << (candidate.at_capacity ? "true" : "false")
                        << ",\"probe_budget_bytes\":" << candidate.probe_budget_bytes.value_or(0)
                        << ",\"probe_remaining_bytes\":" << scheduler.ProbeRemainingBytes(candidate)
                        << ",\"capacity_known\":" << (candidate.capacity_known ? "true" : "false")
                        << ",\"needs_capacity_probe\":" << (diagnostic.needs_capacity_probe ? "true" : "false")
                        << ",\"probe_can_probe\":" << (diagnostic.can_probe ? "true" : "false")
                        << ",\"probe_gate_mask\":" << diagnostic.gate_mask
                        << ",\"probe_last_selected_ms\":" << diagnostic.last_selected_ms
                        << ",\"probe_diagnostic_remaining_bytes\":" << diagnostic.probe_remaining_bytes
                        << "}";
                }
                writer.output << "]"
                    << ",\"reorder_buffered_bytes\":" << reorder_buffered_bytes
                    << ",\"reorder_events\":" << reorder_events
                    << ",\"rx_paused_links\":" << rx_paused_links
                    << ",\"tx_queued_frames\":" << tx_queued_frames
                    << ",\"tx_queued_bytes\":" << tx_queued_bytes
                    << ",\"tx_ctrl_frames\":" << tx_ctrl_frames
                    << ",\"links\":[";
                for (std::size_t index = 0; index < link_samples.size(); ++index) {
                    if (index != 0) writer.output << ",";
                    const auto& sample = link_samples[index];
                    const auto average = [](std::uint64_t sum, std::uint64_t count) noexcept {
                        return count == 0 ? 0 : sum / count;
                    };
                    writer.output << "{\"link_id\":" << sample.link_id
                        << ",\"generation\":" << sample.generation
                        << ",\"queued_bytes\":" << sample.queued_bytes
                        << ",\"wire_inflight_bytes\":" << sample.wire_inflight_bytes
                        << ",\"rx_paused\":" << (sample.rx_paused ? "true" : "false")
                        << ",\"rx_backpressure\":" << sample.rx_backpressure_count
                        << ",\"tx_write_count\":" << sample.tx_write_count
                        << ",\"tx_post_wait_us\":" << average(sample.tx_post_wait_us_sum, sample.tx_write_count)
                        << ",\"tx_post_wait_us_max\":" << sample.tx_post_wait_us_max
                        << ",\"tx_post_wait_us_sum\":" << sample.tx_post_wait_us_sum
                        << ",\"tx_write_latency_us\":" << average(sample.tx_write_us_sum, sample.tx_write_count)
                        << ",\"tx_write_latency_us_max\":" << sample.tx_write_us_max
                        << ",\"tx_write_latency_us_sum\":" << sample.tx_write_us_sum
                        << ",\"crypto_us\":null"
                        << ",\"rx_dispatch_count\":" << sample.rx_dispatch_count
                        << ",\"rx_dispatch_us\":" << average(sample.rx_dispatch_us_sum, sample.rx_dispatch_count)
                        << ",\"rx_dispatch_us_max\":" << sample.rx_dispatch_us_max
                        << ",\"rx_dispatch_us_sum\":" << sample.rx_dispatch_us_sum
                        << ",\"rx_handler_us\":" << average(sample.rx_handler_us_sum, sample.rx_dispatch_count)
                        << ",\"rx_handler_us_max\":" << sample.rx_handler_us_max
                        << "}";
                }
                writer.output << "]}\n";
                writer.output.flush();
            }
            catch (...) {}
        }
     }
    /**
     * @brief Parses a textual MUX scheduler mode.
     */
    vmux_net::mux_mode vmux_net::parse_mode(const ppp::string& mode) noexcept {
        ppp::string value = ppp::ToLower<ppp::string>(ppp::LTrim(ppp::RTrim(mode)));
        if (value == "flow" || value == "flow-v1" || value == "primary" || value == "primary-link") {
            return mux_mode_flow;
        }

        if (value == "balance" || value == "balanced" || value == "lb" || value == "load-balance") {
            return mux_mode_balance;
        }

        if (value == "stripe" || value == "striped" || value == "striping") {
            return mux_mode_stripe;
        }

        return mux_mode_compat;
    }

    /**
     * @brief Maps a wire mode byte to a valid scheduler mode.
     * @return The mode for known values; mux_mode_compat for anything else.
     */
    vmux_net::mux_mode vmux_net::parse_mode_byte(Byte mode_value) noexcept {
        switch (mode_value) {
        case mux_mode_flow:
            return mux_mode_flow;
        case mux_mode_balance:
            return mux_mode_balance;
        case mux_mode_stripe:
            return mux_mode_stripe;
        default:
            return mux_mode_compat;
        }
    }

    /**
     * @brief Returns the stable text name for a scheduler mode.
     */
    const char* vmux_net::mode_name(mux_mode mode) noexcept {
        switch (mode) {
        case mux_mode_flow:
            return "flow";
        case mux_mode_balance:
            return "balance";
        case mux_mode_stripe:
            return "stripe";
        default:
            return "compat";
        }
    }

    /**
     * @brief Switches the active scheduler mode at runtime.
     * @details Must run on the vmux strand. Resets per-mode scheduling state
     *          (primary link, affinity map, stripe cursor) so the next drain
     *          re-picks links under the new policy.
     */
    void vmux_net::set_mode(mux_mode mode) noexcept {
        mux_mode normalized = mode;
        switch (normalized) {
        case mux_mode_flow:
        case mux_mode_balance:
        case mux_mode_stripe:
            break;
        default:
            normalized = mux_mode_compat;
            break;
        }

        if (mode_ == normalized) {
            return;
        }

        mode_ = normalized;
        {
            std::lock_guard<std::mutex> scope(runtime_state_mutex_);
            runtime_state_.effective_mode = mode_name(mode_);
            ppp::app::mux::FillMuxPresentation(runtime_state_);
            publish_runtime_snapshot_locked();
        }
        stripe_cursor_ = 0;
        ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux", "scheduler mode switched to %s", mode_name(mode_));
    }

    /**
     * @brief Applies the negotiated receiver ordering mode (flow v2).
     * @details Effective only before the session is established (no hot switch
     *          between compat and flow-v2). Also latches the per-connection
     *          reorder bounds from AppConfiguration when entering flow-v2.
     */
    void vmux_net::set_ordering_mode(receiver_ordering_mode m) noexcept {
        if (base_.established_) {
            return; // session-level, immutable after establishment.
        }

        receiver_ordering_mode normalized = (m == ordering_flow_v2) ? ordering_flow_v2 : ordering_compat;
        ordering_mode_ = normalized;
        {
            std::lock_guard<std::mutex> scope(runtime_state_mutex_);
            runtime_state_.receiver_ordering = normalized == ordering_flow_v2 ? "flow_v2" : "compat";
        }

        // Latch send-side backpressure / watchdog bounds from config (applies to
        // all modes; AppConfiguration is set by the exchanger before establishment).
        if (NULLPTR != AppConfiguration) {
            int qmax = AppConfiguration->mux.tx.queue.max;
            int qstall = AppConfiguration->mux.tx.queue.stall;
            tx_queue_high_water_ = (qmax > 0) ? (size_t)qmax : (size_t)PPP_MUX_TX_QUEUE_HIGH_WATER;
            tx_backlog_stall_ms_ = (qstall > 0) ? (uint64_t)qstall : (uint64_t)PPP_MUX_TX_BACKLOG_STALL_TIMEOUT;
            // flow turbo: only meaningful under flow mode; harmless to latch always.
            turbo_ = AppConfiguration->mux.turbo && (mode_ == mux_mode_flow);
            {
                std::lock_guard<std::mutex> scope(runtime_state_mutex_);
                runtime_state_.turbo = turbo_;
                ppp::app::mux::FillMuxPresentation(runtime_state_);
                publish_runtime_snapshot_locked();
            }
        }

        // Latch gap-timeout for both ordering modes (flow_v2 per-flow; compat global).
        {
            int timeout_ms = (NULLPTR != AppConfiguration) ? AppConfiguration->mux.flow.reorder.timeout : 0;
            flow_reorder_timeout_ = (timeout_ms > 0) ? (uint64_t)timeout_ms : (uint64_t)PPP_MUX_FLOW_REORDER_TIMEOUT;
        }
        {
            int session_cap = (NULLPTR != AppConfiguration) ? AppConfiguration->mux.flow.session_reorder.bytes : 0;
            session_reorder_cap_bytes_ = (session_cap > 0) ? (size_t)session_cap : (size_t)PPP_MUX_FLOW_SESSION_REORDER_BYTES;
            int max_open = (NULLPTR != AppConfiguration) ? AppConfiguration->mux.flow.max_open : 0;
            max_open_flows_ = (max_open > 0) ? (size_t)max_open : (size_t)PPP_MUX_FLOW_MAX_OPEN;
            int ctrl_budget = (NULLPTR != AppConfiguration) ? AppConfiguration->mux.tx.ctrl.budget_frames : 0;
            tx_ctrl_budget_frames_ = (ctrl_budget > 0) ? (size_t)ctrl_budget : (size_t)PPP_MUX_TX_CTRL_BUDGET_FRAMES;
        }
        if (normalized == ordering_flow_v2) {
            // Latch bounded-reorder byte cap from config; fall back to safe defaults.
            int cap_bytes = (NULLPTR != AppConfiguration) ? AppConfiguration->mux.flow.reorder.bytes : 0;
            flow_reorder_cap_bytes_ = (cap_bytes > 0) ? (size_t)cap_bytes : (size_t)PPP_MUX_FLOW_REORDER_BYTES;
            flow_context_cap_ = (size_t)PPP_MUX_FLOW_MAX_CONTEXTS;
            flow_aggregate_cap_bytes_ = (size_t)PPP_MUX_FLOW_AGGREGATE_BYTES;
            if (flow_aggregate_cap_bytes_ < flow_reorder_cap_bytes_) {
                flow_aggregate_cap_bytes_ = flow_reorder_cap_bytes_;
            }
            if (session_reorder_cap_bytes_ > 0 && session_reorder_cap_bytes_ < flow_aggregate_cap_bytes_) {
                flow_aggregate_cap_bytes_ = session_reorder_cap_bytes_;
            }
        }

        ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux", "ordering mode=%s",
            normalized == ordering_flow_v2 ? "flow-v2" : "compat");
    }

    void vmux_net::set_carrier_resume_policy(bool enabled, uint32_t grace_ms) noexcept {
        carrier_resume_grace_ms_.store(std::min<std::uint32_t>(grace_ms, 30000U), std::memory_order_release);
        carrier_resume_enabled_.store(enabled && grace_ms > 0, std::memory_order_release);
        if (!enabled || grace_ms == 0) {
            carrier_resume_deadline_ms_.store(0, std::memory_order_release);
        }
    }

    void vmux_net::apply_negotiation(bool local_supports_flow_v2, bool peer_supports_flow_v2, bool local_reliability, bool peer_reliability, bool local_fec, bool peer_fec, bool peer_requests_flow_v2) noexcept {
        ppp::app::mux::MuxRuntimeState state;
        {
            std::lock_guard<std::mutex> scope(runtime_state_mutex_);
            // turbo_ may already be latched from config; include it so flow+turbo
            // negotiates ordering correctly and presentation shows adaptive pool.
            state = ppp::app::mux::NegotiateMuxRuntimeState(
                runtime_state_.requested_mode,
                local_supports_flow_v2,
                peer_supports_flow_v2,
                runtime_state_.active_links,
                turbo_ || runtime_state_.turbo,
                local_reliability,
                peer_reliability,
                local_fec,
                peer_fec,
                peer_requests_flow_v2 || intent_routing_enabled());
            runtime_state_ = state;
            publish_runtime_snapshot_locked();
        }
        set_mode(parse_mode(ppp::string(state.effective_mode.data(), state.effective_mode.size())));
        set_ordering_mode(state.receiver_ordering == "flow_v2" ? ordering_flow_v2 : ordering_compat);
        latch_reliability(state.reliability, state.fec);
        // Keep presentation fields in sync after ordering/turbo latch.
        {
            std::lock_guard<std::mutex> scope(runtime_state_mutex_);
            runtime_state_.turbo = turbo_;
            ppp::app::mux::FillMuxPresentation(runtime_state_);
            publish_runtime_snapshot_locked();
        }
    }

    void vmux_net::apply_agreed_ordering(bool agreed_flow_v2, bool agreed_reliability, bool agreed_fec) noexcept {
        ppp::app::mux::MuxRuntimeState state;
        {
            std::lock_guard<std::mutex> scope(runtime_state_mutex_);
            state = ppp::app::mux::ApplyAgreedMuxRuntimeState(
                runtime_state_.requested_mode,
                agreed_flow_v2,
                runtime_state_.active_links,
                turbo_ || runtime_state_.turbo,
                agreed_reliability,
                agreed_fec);
            runtime_state_ = state;
            publish_runtime_snapshot_locked();
        }

        set_mode(parse_mode(ppp::string(state.effective_mode.data(), state.effective_mode.size())));
        set_ordering_mode(state.receiver_ordering == "flow_v2"
            ? ordering_flow_v2
            : ordering_compat);
        latch_reliability(state.reliability, state.fec);
        {
            std::lock_guard<std::mutex> scope(runtime_state_mutex_);
            runtime_state_.turbo = turbo_;
            ppp::app::mux::FillMuxPresentation(runtime_state_);
            publish_runtime_snapshot_locked();
        }
    }

    ppp::app::mux::MuxRuntimeState vmux_net::get_runtime_state() const noexcept {
        // Prefer lock-free snapshot published on the vmux strand (P2-5).
        if (auto snap = std::atomic_load_explicit(&runtime_snapshot_, std::memory_order_acquire)) {
            return *snap;
        }
        // Cold path: first read before any publish, or mid-construction.
        std::lock_guard<std::mutex> scope(runtime_state_mutex_);
        ppp::app::mux::MuxRuntimeState state = runtime_state_;
        state.turbo = turbo_;
        if (state.effective_mode.empty()) {
            state.effective_mode = mode_name(mode_);
        }
        ppp::app::mux::FillMuxPresentation(state);
        return state;
    }

    vmux_net::MultipathRuntimeStats vmux_net::get_multipath_runtime_stats() const noexcept {
        try {
            auto snapshot = std::atomic_load_explicit(
                &multipath_path_runtime_snapshot_, std::memory_order_acquire);
            return snapshot ? *snapshot : MultipathRuntimeStats();
        }
        catch (...) {
            return {};
        }
    }

    ppp::app::multipath::PathTelemetry vmux_net::get_link_telemetry(
        const vmux_linklayer& link, uint64_t now) const noexcept {
        auto telemetry = link.path_estimator_ ? link.path_estimator_->GetTelemetry(now)
            : ppp::app::multipath::PathTelemetry{};
        telemetry.local_queue_bytes = link.queued_bytes_;
        telemetry.wire_inflight_bytes = reliability_on_ && link.id_ != 0
            ? rtx_.LinkBytes(link.id_) : link.queued_bytes_;
        const bool fresh_rtt = link.has_rtt_sample_ && link.last_rtt_sample_ms_ <= now &&
            now - link.last_rtt_sample_ms_ <= ppp::app::multipath::PathScheduler::MetricsMaxAgeMs;
        const bool fresh_capacity =
            ppp::app::multipath::PathScheduler::CapacityIsFresh(telemetry, now);
        if (!fresh_capacity) {
            // Do not let an expired estimate keep granting carrier credit or
            // scheduling weight as if it were current.
            telemetry.capacity_rate_bytes_per_second = 0;
            telemetry.capacity_updated_ms = 0;
        }
        telemetry.scheduling_weight = fresh_rtt && fresh_capacity
            ? link.adaptive_weight_ : std::min(link.adaptive_weight_, 50u);
        telemetry.app_limited = link.app_limited_;
        if (fresh_rtt) {
            telemetry.srtt_ms = link.srtt_ms_;
            telemetry.jitter_ms = link.rttvar_ms_;
            telemetry.min_rtt_ms = link.min_rtt_ms_;
            // Until a second sample confirms a floor, min_rtt is unknown. Do
            // not publish the whole SRTT as "queue delay" in that window: the
            // first RTT may itself be the queued one, and consumers such as
            // PoolController act on this field.
            telemetry.queue_delay_ms = link.min_rtt_ms_ != 0 &&
                link.srtt_ms_ > link.min_rtt_ms_
                ? link.srtt_ms_ - link.min_rtt_ms_ : 0;
            if (!telemetry.has_sample) {
                telemetry.has_sample = true;
                telemetry.updated_ms = link.last_rtt_sample_ms_;
                telemetry.confidence = 20;
            }
        }
        // A fresh capacity sample is the best BDP estimate. When only the
        // delivery and RTT samples are fresh (the capacity sample aged out),
        // leaving the BDP at zero collapses a measured multi-megabyte pipe to
        // the fixed startup window, i.e. every such path is treated as if it
        // held a few hundred KiB. Use the freshest delivery rate times the
        // minimum RTT instead; an application-limited delivery sample can only
        // understate the BDP, and the scheduler keeps its startup floor, so
        // this fallback never invents flight beyond measured evidence.
        //
        // at_capacity stays a fresh-capacity signal: a stale estimate must not
        // suppress capacity probes (the sender still needs to re-measure a
        // path whose capacity sample expired), even though the fallback BDP is
        // used for scheduling.
        const bool delivery_fresh = !fresh_capacity &&
            telemetry.current_rate_bytes_per_second != 0;
        if (telemetry.min_rtt_ms != 0 && (fresh_capacity || delivery_fresh)) {
            const auto rate = fresh_capacity
                ? telemetry.capacity_rate_bytes_per_second
                : telemetry.current_rate_bytes_per_second;
            const auto maximum = std::numeric_limits<uint64_t>::max();
            telemetry.bdp_bytes = (rate > maximum / telemetry.min_rtt_ms
                ? maximum : rate * telemetry.min_rtt_ms) / 1000;
            telemetry.at_capacity = fresh_capacity && telemetry.bdp_bytes != 0 &&
                telemetry.wire_inflight_bytes >= telemetry.bdp_bytes;
        }
        return telemetry;
    }

    void vmux_net::update_link_pacing(vmux_linklayer& link, uint64_t now) noexcept {
        // Pacing shapes this carrier's egress from the capacity estimate the
        // path estimator actually measured. Both ends apply it: the reverse
        // ACK path can queue behind bulk payload on either side, so gating the
        // cap to the client left one direction unpaced and let the shared FIFO
        // distort the feedback the estimator depends on.
        if (NULLPTR == link.connection) {
            return;
        }
        if (!link.pacing_probed_) {
            link.pacing_probed_ = true;
            ITransmissionPtr transmission = link.connection->GetTransmission();
            link.pacing_supported_ = transmission && transmission->SupportsPacingRate();
        }
        if (!link.pacing_supported_) {
            // TCP-only: WS/WSS (and any carrier without SO_MAX_PACING_RATE)
            // stays untouched, so an unsupported transport cannot affect the
            // connection in any way.
            return;
        }

        const auto telemetry = get_link_telemetry(link, now);
        if (link.path_estimator_ && !link.path_estimator_->FullBandwidthReached() &&
            link.pacing_.AppliedRate() == 0) {
            // Do not cap slow-start delivery: the cap would become the next
            // estimator sample and freeze discovery below the shaped rate.
            return;
        }
        ppp::app::multipath::PathPacingObservation observation;
        observation.capacity_bytes_per_second = telemetry.capacity_rate_bytes_per_second;
        observation.capacity_updated_ms = telemetry.capacity_updated_ms;
        observation.now_ms = now;
        observation.at_capacity = telemetry.at_capacity;
        // The capacity sample counts delivered payload (our upload, confirmed
        // by the peer's ACK), while the kernel caps wire bytes. This carrier's
        // own counters measure exactly that conversion: every frame written on
        // this link (data, ACKs, control, retransmissions) against the unique
        // payload those frames carried. They are cumulative and per
        // attachment, so the ratio cannot oscillate, and it can only
        // over-state the framing (retransmissions and ACK-only frames add wire
        // bytes without payload), which paces looser rather than tighter.
        // Without a payload sample large enough to divide by, the ratio stays
        // unset and the policy applies no cap.
        if (const auto& counters = link.path_runtime_counters_) {
            const auto payload = counters->tx_unique_payload_bytes.load(std::memory_order_relaxed);
            const auto wire = counters->tx_bytes.load(std::memory_order_relaxed);
            if (payload >= PathPacingWireRatioMinPayloadBytes && wire >= payload) {
                const auto maximum = std::numeric_limits<uint64_t>::max();
                const auto scaled = wire > maximum / PathPacingWireRatioPermille
                    ? maximum : wire * PathPacingWireRatioPermille;
                // Round up: a higher ratio can only pace looser.
                observation.payload_to_wire_numerator =
                    scaled / payload + (scaled % payload != 0 ? 1 : 0);
                observation.payload_to_wire_denominator = PathPacingWireRatioPermille;
            }
        }
        const auto decision = link.pacing_.Observe(observation);
        if (decision.action == ppp::app::multipath::PathPacingAction::None) {
            return;
        }

        ITransmissionPtr transmission = link.connection->GetTransmission();
        if (NULLPTR == transmission) {
            return;
        }
        if (decision.action == ppp::app::multipath::PathPacingAction::Clear) {
            (void)transmission->ApplyPacingRate(0);
            link.pacing_.CommitCleared();
            ppp::telemetry::Count("mux.pacing.clear", 1);
            return;
        }

        switch (transmission->ApplyPacingRate(decision.rate_bytes_per_second)) {
        case ppp::transmissions::TransmissionPacingResult::Applied:
            ppp::telemetry::Count("mux.pacing.apply", 1);
            link.pacing_.CommitApplied(decision.rate_bytes_per_second);
            break;
        case ppp::transmissions::TransmissionPacingResult::Unchanged:
            link.pacing_.CommitApplied(decision.rate_bytes_per_second);
            break;
        case ppp::transmissions::TransmissionPacingResult::Failed:
            ppp::telemetry::Count("mux.pacing.apply_failed", 1);
            link.pacing_.CommitFailed(decision.rate_bytes_per_second);
            break;
        case ppp::transmissions::TransmissionPacingResult::Unsupported:
            link.pacing_supported_ = false;
            link.pacing_.Reset();
            break;
        case ppp::transmissions::TransmissionPacingResult::Cleared:
            link.pacing_.CommitCleared();
            break;
        }
    }

    void vmux_net::publish_multipath_path_runtime_stats() noexcept {
        const int scheduler_policy = multipath_scheduler_policy_.load(std::memory_order_acquire);
        if (path_runtime_counters_.empty() && scheduler_policy < 0) return;
        try {
            const uint64_t now = now_tick();
            const auto saturating_add = [](uint64_t left, uint64_t right) noexcept {
                const uint64_t maximum = std::numeric_limits<uint64_t>::max();
                return right > maximum - left ? maximum : left + right;
            };
            MultipathRuntimeStats snapshot;
            snapshot.session_id = multipath_runtime_session_id_.load(std::memory_order_acquire);
            snapshot.reorder_events = reorder_events_;
            snapshot.reorder_buffered_bytes = session_reorder_bytes_;
            snapshot.intent = intent_runtime_stats_;
            snapshot.paths.reserve(path_runtime_counters_.size());
            snapshot.links.reserve(rx_links_.size());
            for (const auto& item : path_runtime_counters_) {
                if (!item.second) continue;
                MultipathPathRuntimeStats path;
                path.path_id = item.first;
                path.tx_bytes = item.second->tx_bytes.load(std::memory_order_relaxed);
                path.rx_bytes = item.second->rx_bytes.load(std::memory_order_relaxed);
                path.retransmit_bytes = item.second->retransmit_bytes.load(std::memory_order_relaxed);
                path.tx_unique_payload_bytes = item.second->tx_unique_payload_bytes.load(std::memory_order_relaxed);
                path.rx_unique_payload_bytes = item.second->rx_unique_payload_bytes.load(std::memory_order_relaxed);
                snapshot.paths.emplace_back(std::move(path));
            }

            const size_t live_carriers = count_live_carriers();
            for (const vmux_linklayer_ptr& link : rx_links_) {
                if (!link || !link->schedulable() || !link->path_runtime_counters_) continue;
                const auto telemetry = get_link_telemetry(*link, now);
                const auto& counters = *link->path_runtime_counters_;
                ppp::app::multipath::RuntimeCarrierState carrier;
                carrier.path_id = link->path_id_;
                carrier.link_id = link->id_;
                carrier.generation = link->generation_;
                carrier.tx_bytes = counters.tx_bytes.load(std::memory_order_relaxed);
                carrier.rx_bytes = counters.rx_bytes.load(std::memory_order_relaxed);
                carrier.tx_unique_payload_bytes = counters.tx_unique_payload_bytes.load(std::memory_order_relaxed);
                carrier.rx_unique_payload_bytes = counters.rx_unique_payload_bytes.load(std::memory_order_relaxed);
                carrier.retransmit_bytes = counters.retransmit_bytes.load(std::memory_order_relaxed);
                carrier.retransmit_events = counters.retransmit_events.load(std::memory_order_relaxed);
                carrier.queued_bytes = link->queued_bytes_;
                carrier.wire_inflight_bytes = telemetry.wire_inflight_bytes;
                carrier.send_credit_bytes = scheduler_policy == 0 && reliability_on_
                    ? adaptive_send_credit(*link, telemetry, live_carriers) : local_send_credit(*link);
                carrier.rtt_ms = telemetry.srtt_ms;
                carrier.jitter_ms = telemetry.jitter_ms;
                carrier.queue_delay_ms = telemetry.queue_delay_ms;
                carrier.delivery_rate_bytes_per_second = telemetry.current_rate_bytes_per_second;
                carrier.capacity_rate_bytes_per_second = telemetry.capacity_rate_bytes_per_second;
                carrier.scheduling_weight = telemetry.scheduling_weight;
                carrier.confidence = telemetry.confidence;
                carrier.metrics_known =
                    telemetry.srtt_ms != 0 && telemetry.capacity_rate_bytes_per_second != 0;
                carrier.app_limited = telemetry.app_limited;
                carrier.scheduler_selection_count = link->scheduler_selection_count_;
                carrier.scheduler_probe_selection_count = link->scheduler_probe_selection_count_;
                carrier.scheduler_probe_bytes = link->scheduler_probe_bytes_;
                carrier.scheduler_probe_remaining_bytes = link->scheduler_probe_remaining_bytes_;
                carrier.scheduler_last_selection_sequence = link->scheduler_last_selection_sequence_;
                carrier.scheduler_last_payload_bytes = link->scheduler_last_payload_bytes_;
                carrier.scheduler_last_send_credit_bytes = link->scheduler_last_send_credit_bytes_;
                carrier.scheduler_last_queue_bytes = link->scheduler_last_queue_bytes_;
                carrier.scheduler_last_wire_inflight_bytes = link->scheduler_last_wire_inflight_bytes_;
                carrier.scheduler_last_probe_budget_bytes = link->scheduler_last_probe_budget_bytes_;
                carrier.scheduler_last_probe = link->scheduler_last_probe_;
                carrier.scheduler_candidate_observation_count = link->scheduler_candidate_observation_count_;
                carrier.scheduler_last_observation_sequence = link->scheduler_last_observation_sequence_;
                carrier.scheduler_last_observed_send_credit_bytes = link->scheduler_last_observed_send_credit_bytes_;
                carrier.scheduler_last_observed_queue_bytes = link->scheduler_last_observed_queue_bytes_;
                carrier.scheduler_last_observed_wire_inflight_bytes = link->scheduler_last_observed_wire_inflight_bytes_;
                carrier.scheduler_last_observed_probe_remaining_bytes = link->scheduler_last_observed_probe_remaining_bytes_;
                snapshot.links.emplace_back(std::move(carrier));

                auto found = std::find_if(snapshot.paths.begin(), snapshot.paths.end(), [&link](const auto& path) {
                    return path.path_id == link->path_id_;
                });
                if (found == snapshot.paths.end()) continue;
                found->inflight_bytes = saturating_add(found->inflight_bytes, telemetry.wire_inflight_bytes);
                found->delivery_rate_bytes_per_second = saturating_add(
                    found->delivery_rate_bytes_per_second,
                    telemetry.current_rate_bytes_per_second);
                found->capacity_rate_bytes_per_second = saturating_add(
                    found->capacity_rate_bytes_per_second,
                    telemetry.capacity_rate_bytes_per_second);
                found->queue_delay_ms = std::max(found->queue_delay_ms,
                    telemetry.queue_delay_ms);
                found->bdp_bytes = saturating_add(found->bdp_bytes, telemetry.bdp_bytes);
                found->confidence = std::max(found->confidence, telemetry.confidence);
                found->has_telemetry = found->has_telemetry || telemetry.has_sample;
                found->at_capacity = found->at_capacity || telemetry.at_capacity;
                if (telemetry.min_rtt_ms != 0 &&
                    (found->min_rtt_ms == 0 || telemetry.min_rtt_ms < found->min_rtt_ms)) {
                    found->min_rtt_ms = telemetry.min_rtt_ms;
                }
                if (link->has_rtt_sample_ &&
                    (!found->has_rtt || link->srtt_ms_ < found->rtt_ms)) {
                    found->rtt_ms = link->srtt_ms_;
                    found->has_rtt = true;
                }
            }
            auto published = std::make_shared<const MultipathRuntimeStats>(std::move(snapshot));
            std::atomic_store_explicit(&multipath_path_runtime_snapshot_,
                std::move(published), std::memory_order_release);
        }
        catch (...) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::MemoryAllocationFailed);
        }
    }

    void vmux_net::refresh_runtime_active_links() noexcept {
        // Read-only container walk: safe without syncobj_ because container
        // mutations join the syncobj_ domain (see remove/reap/attach) and
        // handshake_complete_ is atomic, so the carrier handshake's store is
        // safely visible here.
        std::size_t active_links = 0;
        for (const vmux_linklayer_ptr& linklayer : rx_links_) {
            if (NULLPTR != linklayer && linklayer->schedulable()) {
                ++active_links;
            }
        }
        std::lock_guard<std::mutex> scope(runtime_state_mutex_);
        runtime_state_.active_links = static_cast<std::uint16_t>(
            std::min<std::size_t>(active_links, UINT16_MAX));
        publish_runtime_snapshot_locked();
    }

    /**
     * @brief Raises the carrier-link ceiling for turbo before establishment.
     */
    void vmux_net::set_pool_hard_max(uint16_t hard_max) noexcept {
        if (base_.established_) {
            return; // base/ceiling are fixed once the initial links are built.
        }

        if (hard_max < status_.max_connections) {
            hard_max = status_.max_connections;
        }

        status_.pool_hard_max = hard_max;
    }

    bool vmux_net::add_pool_trial_linklayer(const IMuxTransportPtr& transport, vmux_linklayer_ptr& link,
        const vmux_native_add_linklayer_after_success_before_callback& cb, uint64_t token) noexcept {
        if (!token || token != pool_token_ || pool_trial_link_ || !base_.established_ ||
            !add_linklayer(transport, link, cb)) return false;
        pool_trial_link_ = link;
        pool_dial_in_flight_ = 0;
        return true;
    }

    void vmux_net::pool_dial_failed(uint64_t token) noexcept {
        if (token == pool_dial_in_flight_) pool_dial_in_flight_ = 0;
        if (!token || token != pool_token_ || pool_trial_link_) return;
        pool_token_ = 0;
        pool_controller_.DialFailed(now_tick());
        ppp::telemetry::Count("mux.pool.dial.failed", 1);
    }

    /**
     * @brief Debug-only wire payload for cmd_mux_mode_set.
     *
     * Layout: [mode:1][key_len:1][key:key_len]. The key authorizes the change;
     * the receiver applies the mode only when the key matches its own non-empty
     * mux.debug.key. No new per-frame header field is introduced.
     */
    bool vmux_net::post_mux_mode_set(mux_mode mode) noexcept {
        if (NULLPTR == AppConfiguration) {
            return false;
        }

        const ppp::string& debug_key = AppConfiguration->mux.debug.key;
        if (debug_key.empty()) {
            ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux", "mux-mode-set ignored: no local debug key configured");
            return false;
        }

        size_t key_length = debug_key.size();
        if (key_length > 255) {
            key_length = 255;
        }

        ppp::vector<Byte> payload(2 + key_length);
        payload[0] = static_cast<Byte>(mode);
        payload[1] = static_cast<Byte>(key_length);
        if (key_length > 0) {
            memcpy(payload.data() + 2, debug_key.data(), key_length);
        }

        ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux", "mux-mode-set requesting peer mode=%s", mode_name(mode));
        return post(cmd_mux_mode_set, payload.data(), static_cast<int>(payload.size()), 0);
    }

    using ppp::telemetry::Level;

    /**
     * @brief Constructs a vmux network core with runtime mode/capacity settings.
     */
    vmux_net::vmux_net(const ContextPtr& context, const StrandPtr strand, uint16_t max_connections, bool server_mode, bool acceleration, mux_mode mode) noexcept {
        assert(max_connections > 0 && "The value of max_connections must be greater than 0.");

        vmux_net* const m             = this;
        m->Vlan                       = 0;
   
        m->base_.server_or_client_    = server_mode;
        m->base_.disposed_.store(false, std::memory_order_release);
        m->base_.ftt_                 = false;
        m->base_.established_         = false;
        m->base_.acceleration_        = acceleration;
        
        m->status_.max_connections    = max_connections;
        m->status_.pool_hard_max      = max_connections; // raised by turbo via set_pool_hard_max() before establishment.
        m->status_.opened_connections = 0;

        m->status_.rx_ack_            = 0;
        m->status_.tx_seq_            = 0;

        m->mode_                      = mode;
        switch (m->mode_) {
        case mux_mode_flow:
        case mux_mode_balance:
        case mux_mode_stripe:
            break;
        default:
            m->mode_                  = mux_mode_compat;
            break;
        }
        m->runtime_state_.requested_mode = mode_name(m->mode_);
        m->runtime_state_.effective_mode = mode_name(m->mode_);
        m->runtime_state_.receiver_ordering = "compat";
        m->runtime_state_.turbo = false;
        ppp::app::mux::FillMuxPresentation(m->runtime_state_);
        m->runtime_snapshot_ = std::make_shared<const ppp::app::mux::MuxRuntimeState>(m->runtime_state_);

        ppp::telemetry::Log(Level::kInfo, "mux", "mode=%s", mode_name(m->mode_));
        uint64_t now                  = now_tick();
        m->status_.last_              = now;
        m->status_.last_heartbeat_    = now;
        m->status_.heartbeat_timeout_ = 0;

        m->context_                   = context;
        m->strand_                    = strand;
        if (NULLPTR != m->context_ && NULLPTR == m->strand_) {
            m->strand_ = ppp::make_shared_object<Strand>(m->context_->get_executor());
        }
        if (NULLPTR == m->context_ || NULLPTR == m->strand_) {
            m->base_.disposed_.store(true, std::memory_order_release);
            ppp::diagnostics::SetLastErrorCode(NULLPTR == m->context_
                ? ppp::diagnostics::ErrorCode::RuntimeIoContextMissing
                : ppp::diagnostics::ErrorCode::RuntimeTaskPostFailed);
        }
    }

    void vmux_net::set_intent_config(const ppp::app::mux::TcpIntentConfig& config) noexcept {
        // Configuration is latched before flows are created. Existing flow
        // classifiers intentionally keep their original policy for the
        // lifetime of the logical connection.
        intent_config_ = config;
        intent_routing_enabled_.store(config.enabled, std::memory_order_release);
    }

    void vmux_net::set_intent_rules(
        const std::vector<ppp::app::mux::TcpIntentRule>& rules) noexcept {
        try {
            intent_rules_ = rules;
        }
        catch (...) {
            intent_rules_.clear();
        }
    }

    void vmux_net::seed_flow_intent(uint32_t connection_id,
        const template_string& host, int port) noexcept {
        if (!intent_routing_enabled() || connection_id == 0) return;
        try {
            ppp::app::mux::TcpIntentClassifier classifier(intent_config_);
            classifier.Seed(std::string(host.data(), host.size()),
                static_cast<std::uint16_t>(std::max(0, port)), intent_rules_);
            flow_intents_[connection_id] = classifier.Intent();
            intent_classifiers_.insert_or_assign(connection_id, std::move(classifier));
            if (flow_intents_[connection_id] != ppp::app::mux::TcpIntent::Unknown) {
                account_flow_intent(connection_id, flow_intents_[connection_id]);
            }
        }
        catch (...) {
            flow_intents_[connection_id] = ppp::app::mux::TcpIntent::Unknown;
        }
    }

    void vmux_net::observe_flow_intent(uint32_t connection_id, std::size_t bytes,
        std::size_t chunk_bytes, bool outbound) noexcept {
        if (!intent_routing_enabled() || connection_id == 0) return;
        auto it = intent_classifiers_.find(connection_id);
        if (it == intent_classifiers_.end()) return;
        const auto before = it->second.Intent();
        const auto result = it->second.Observe(now_tick(), bytes, chunk_bytes, outbound);
        flow_intents_[connection_id] = result.intent;
        if (result.changed && result.intent != before) {
            ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
                "intent classified cid=%u intent=%s bytes=%llu duration_ms=%llu",
                static_cast<unsigned>(connection_id), tcp_intent_name(result.intent),
                static_cast<unsigned long long>(result.sample_bytes),
                static_cast<unsigned long long>(result.sample_duration_ms));
            if (before != ppp::app::mux::TcpIntent::Unknown &&
                result.intent != ppp::app::mux::TcpIntent::Unknown && before != result.intent) {
                ++intent_runtime_stats_.intent_flip_count;
                ppp::telemetry::Count("mux.intent.flip", 1);
            }
            if (result.intent != ppp::app::mux::TcpIntent::Unknown) {
                account_flow_intent(connection_id, result.intent);
            }
        }
    }

    void vmux_net::account_flow_intent(uint32_t connection_id,
        ppp::app::mux::TcpIntent intent) noexcept {
        if (connection_id == 0 || intent_accounted_flows_.find(connection_id) !=
            intent_accounted_flows_.end()) return;
        try {
            if (!intent_accounted_flows_.insert(connection_id).second) return;
            const char* metric = "mux.intent.unknown";
            switch (intent) {
            case ppp::app::mux::TcpIntent::Latency:
                ++intent_runtime_stats_.latency;
                metric = "mux.intent.latency";
                break;
            case ppp::app::mux::TcpIntent::Bandwidth:
                ++intent_runtime_stats_.bandwidth;
                metric = "mux.intent.bandwidth";
                break;
            default:
                ++intent_runtime_stats_.unknown;
                break;
            }
            ppp::telemetry::Count(metric, 1);
            trace_flow_classification(connection_id, intent);
        }
        catch (...) {
            intent_accounted_flows_.erase(connection_id);
        }
    }

    ppp::app::mux::TcpIntent vmux_net::flow_intent(uint32_t connection_id) const noexcept {
        auto it = flow_intents_.find(connection_id);
        return it == flow_intents_.end() ? ppp::app::mux::TcpIntent::Unknown : it->second;
    }

    vmux_net::tx_completion_ptr vmux_net::make_tx_completion(const PostInternalAsynchronousCallback& callback) noexcept {
        if (NULLPTR == callback) {
            return NULLPTR;
        }
        try {
            return ppp::make_shared_object<tx_completion>(callback);
        }
        catch (...) {
            return NULLPTR;
        }
    }

    bool vmux_net::track_tx_completion(const tx_completion_ptr& completion) noexcept {
        if (NULLPTR == completion) {
            return true;
        }

        bool accepted = false;
        {
            std::lock_guard<std::mutex> scope(tx_completion_mutex_);
            if (!close_requested_) {
                try {
                    pending_tx_completions_.emplace(completion);
                    accepted = true;
                }
                catch (...) {
                }
            }
        }

        if (!accepted) {
            completion->Finish(false);
        }
        return accepted;
    }

    void vmux_net::finish_tx_completion(const tx_completion_ptr& completion, bool successed) noexcept {
        if (NULLPTR == completion) {
            return;
        }

        {
            std::lock_guard<std::mutex> scope(tx_completion_mutex_);
            pending_tx_completions_.erase(completion);
            if (completion->retained_payload_key != 0) {
                pending_payload_tx_completions_.erase(completion->retained_payload_key);
            }
        }
        completion->Finish(successed);
    }

    void vmux_net::finish_tx_completions(tx_completion_list& completions, bool successed) noexcept {
        while (!completions.empty()) {
            tx_completion_ptr completion = std::move(completions.front());
            completions.pop_front();
            if (NULLPTR != completion) {
                completion->Finish(successed);
            }
        }
    }

    void vmux_net::fail_pending_tx_completions() noexcept {
        tx_completion_list completions;
        {
            std::lock_guard<std::mutex> scope(tx_completion_mutex_);
            completions.assign(pending_tx_completions_.begin(), pending_tx_completions_.end());
            pending_tx_completions_.clear();
            pending_payload_tx_completions_.clear();
        }
        finish_tx_completions(completions, false);
    }

    bool vmux_net::begin_close() noexcept {
        tx_completion_list completions;
        {
            std::lock_guard<std::mutex> scope(tx_completion_mutex_);
            if (close_requested_) {
                return false;
            }
            close_requested_ = true;
            completions.assign(pending_tx_completions_.begin(), pending_tx_completions_.end());
            pending_tx_completions_.clear();
            pending_payload_tx_completions_.clear();
        }
        finish_tx_completions(completions, false);
        return true;
    }

    bool vmux_net::close_requested() const noexcept {
        std::lock_guard<std::mutex> scope(tx_completion_mutex_);
        return close_requested_;
    }

    /**
     * @brief Destroys the vmux network and releases runtime resources.
     */
    vmux_net::~vmux_net() noexcept {
        // A destructor can run on any thread; finalize() detects the missing
        // shared owner / stopped executor and tears down inline. close_exec()
        // remains the preferred early entry.
        finalize();
    }

    /**
     * @brief Finalizes queues, sockets, and linklayers, then marks disposed.
     */
    void vmux_net::finalize() noexcept {
        // Strand-affine teardown, always performed inline. Entries: on the vmux
        // strand (directly, or posted there by close_exec), the close_exec()
        // stopped-executor fallback, and the destructor -- in the latter two no
        // strand handler can run concurrently. Idempotent via disposed_.
        begin_close();
        vmux_linklayer_vector rx_links;
        rx_packet_ssqueue rx_queue;
        vmux_skt_map skts;
        std::shared_ptr<boost::asio::ip::tcp::resolver> tx_resolver;

        for (;;) {
            SynchronizationObjectScope __SCOPE__(syncobj_);
            // Idempotent: all teardown state below is owned by this strand.
            if (base_.disposed_.load(std::memory_order_acquire)) {
                return;
            }
            base_.disposed_.store(true, std::memory_order_release);
            status_.last_ = now_tick();

            rx_links = std::move(rx_links_);
            const size_t residual_frames = tx_data_frames_;
            rx_queue = std::move(rx_queue_);

            // Early-ACK observability: residual data still queued when the session dies.
            if (residual_frames > 0) {
                ppp::telemetry::Count("mux.tx.residual.frames", (int64_t)residual_frames);
                ppp::telemetry::Gauge("mux.tx.residual.depth", (int64_t)residual_frames);
                ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
                    "finalize residual tx data frames=%u (local write may have been early-acked)",
                    (unsigned)residual_frames);
            }

            skts = std::move(skts_);
            skts_.clear();

            clear_flow_tx();
            tx_ctrl_queue_.clear();
            rx_queue_.clear();
            rx_links_.clear();
            refresh_runtime_active_links();
            tx_links_.clear();
            stripe_cursor_ = 0;
            flows_.clear();
            tx_flow_seq_.clear();
            rx_resume_all_links(); // wake paused forwarding coroutines so they observe disposed_
            rx_pressure_flows_.clear();

            // Reliability sub-protocol teardown: stop the maintenance timer and
            // drop all ACK / retransmit / FEC state with the session.
            if (NULLPTR != reliability_timer_) {
                reliability_timer_->cancel();
                reliability_timer_.reset();
            }
            rtx_.Clear();
            rtx_pending_.clear();
            ack_trackers_.clear();
            ack_pending_cids_.clear();
            ack_partial_pending_ = false;
            ack_block_cursor_ = 0;
            ack_pending_count_ = 0;
            ack_first_pending_tick_ = 0;
            srtt_ms_ = 0;
            fec_encoder_.Reset(now_tick());
            fec_groups_.clear();
            fec_frame_cache_.clear();
            fec_frame_cache_order_.clear();
            fec_frame_cache_bytes_ = 0;
            fec_flush_due_ = false;
            break;
        }

        for (const std::pair<uint32_t, vmux_skt_ptr>& kv : skts) {
            const vmux_skt_ptr& skt = kv.second;
            if (NULLPTR != skt) {
                // finalize() already owns the VMUX strand, so avoid another posted
                // close that could remain queued forever after an executor stop.
                skt->finalize();
            }
        }

        for (vmux_linklayer_ptr& linklayer : rx_links) {
            if (NULLPTR == linklayer) {
                continue;
            }
            ppp::telemetry::Log(Level::kInfo, "mux", "link close");
            ppp::telemetry::Count("mux.link.close", 1);

            IMuxTransportPtr& connection = linklayer->connection;
            if (NULLPTR != connection) {
                connection->Dispose();
            }
        }

        if (NULLPTR != tx_resolver) {
            vmux_post_exec(context_, strand_,
                [tx_resolver]() noexcept {
                    ppp::net::Socket::Cancel(*tx_resolver);
                });
        }
    }

    /** @brief Returns the first active linklayer connection, if available. */
    vmux_net::IMuxTransportPtr vmux_net::get_linklayer() noexcept {
        SynchronizationObjectScope __SCOPE__(syncobj_);
        vmux_linklayer_vector::iterator tail = rx_links_.begin();
        vmux_linklayer_vector::iterator endl = rx_links_.end();
        return tail != endl ? (*tail)->connection : NULLPTR;
    }

    /** @brief Removes one link-layer endpoint from the scheduling containers; caller holds syncobj_. */
    void vmux_net::remove_linklayer_locked(const vmux_linklayer_ptr& linklayer) noexcept {
        for (vmux_linklayer_vector::iterator it = rx_links_.begin(); it != rx_links_.end();) {
            if (*it == linklayer) {
                it = rx_links_.erase(it);
                ++pool_topology_;
            }
            else {
                ++it;
            }
        }

        tx_link_withdraw(linklayer);
    }

    /** @brief Removes one link-layer endpoint from receive/transmit scheduling state. */
    void vmux_net::remove_linklayer(const vmux_linklayer_ptr& linklayer) noexcept {
        if (NULLPTR == linklayer) {
            return;
        }

        {
            SynchronizationObjectScope __SCOPE__(syncobj_);
            remove_linklayer_locked(linklayer);
        }
        refresh_runtime_active_links();
    }

    size_t vmux_net::count_live_carriers(const vmux_linklayer_ptr& except) const noexcept {
        // Read-only walk: handshake_complete_ is atomic; container mutations are
        // strand-side and join the syncobj_ domain (see remove/reap/attach).
        size_t live = 0;
        for (const vmux_linklayer_ptr& link : rx_links_) {
            if (NULLPTR == link || link == except) {
                continue;
            }
            if (link->schedulable()) {
                ++live;
            }
        }
        return live;
    }

    uint64_t vmux_net::refresh_link_write_high_water(vmux_linklayer& link, uint64_t now) noexcept {
        using Scheduler = ppp::app::multipath::PathScheduler;
        const auto telemetry = get_link_telemetry(link, now);
        const auto window = Scheduler::ComputeLinkWriteWindow(telemetry, now,
            link.write_high_water_bytes_, link.write_high_water_updated_ms_,
            link.queued_bytes_);
        link.write_high_water_bytes_ = window.high_water_bytes;
        link.write_high_water_updated_ms_ = now;
        return window.high_water_bytes;
    }

    bool vmux_net::link_has_byte_credit(const vmux_linklayer_ptr& linklayer, int packet_length) noexcept {
        if (NULLPTR == linklayer || packet_length < 0) {
            return false;
        }
        const size_t window = static_cast<size_t>(refresh_link_write_high_water(*linklayer, now_tick()));
        const size_t need = static_cast<size_t>(packet_length);
        // The adaptive floor is larger than max_frame_size. Require the whole
        // frame to fit so the dynamic window remains a real residency bound.
        return linklayer->queued_bytes_ <= window && need <= window - linklayer->queued_bytes_;
    }

    void vmux_net::tx_link_offer(const vmux_linklayer_ptr& linklayer) noexcept {
        // tx_links_ lists carriers that may take another frame. A carrier stays
        // listed while it has in-flight byte budget, so several frames can be in
        // flight on it (the carrier write queue then gathers them into one
        // socket write) instead of one frame per completion round trip.
        if (NULLPTR == linklayer || linklayer->in_tx_links_ || linklayer->drain_.retiring()) {
            return;
        }
        if (!link_has_byte_credit(linklayer, (int)sizeof(vmux_hdr))) {
            return;
        }
        linklayer->in_tx_links_ = true;
        tx_links_.emplace_back(linklayer);
    }

    void vmux_net::tx_links_reoffer() noexcept {
        if (base_.disposed_.load(std::memory_order_acquire) || close_requested()) {
            return;
        }
        // O(carriers): the pool is bounded by the negotiated hard maximum. Only
        // carriers that can actually take the next frame join the list, so this
        // never lists an idle-but-full or unestablished carrier.
        for (const vmux_linklayer_ptr& linklayer : rx_links_) {
            if (NULLPTR == linklayer || !is_linklayer_active(linklayer)) {
                continue;
            }
            tx_link_offer(linklayer);
        }
    }

    void vmux_net::tx_link_withdraw(const vmux_linklayer_ptr& linklayer) noexcept {
        if (NULLPTR == linklayer || !linklayer->in_tx_links_) {
            return;
        }
        linklayer->in_tx_links_ = false;
        tx_links_.remove(linklayer);
    }

    uint64_t vmux_net::local_send_credit(vmux_linklayer& link) noexcept {
        if (!link.in_tx_links_) {
            return 0;
        }
        const auto window = static_cast<size_t>(refresh_link_write_high_water(link, now_tick()));
        if (link.queued_bytes_ >= window) {
            return 0;
        }
        return static_cast<uint64_t>(window - link.queued_bytes_);
    }

    uint64_t vmux_net::adaptive_send_credit(vmux_linklayer& link,
        const ppp::app::multipath::PathTelemetry& telemetry, size_t live_carriers) noexcept {
        using Scheduler = ppp::app::multipath::PathScheduler;
        const auto local_credit = local_send_credit(link);
        // A provisional estimate below its measured BDP is discovery data, not
        // a safe hard cap. Let the path fill its estimate before applying the
        // adaptive flight limit; otherwise delivery feedback can self-lock low.
        if (live_carriers <= 1 || local_credit == 0 || !telemetry.at_capacity) {
            return local_credit;
        }
        // Delivery materially below a fresh capacity sample means the BDP
        // target itself may be the ceiling under test. The scheduler's own
        // probe budget bounds the extra flight, and the live probe remainder
        // shrinks it as the burst is spent. The allowance never exceeds one
        // link high-water window per calculation, so discovery fills the path
        // incrementally instead of granting the whole probe burst at once.
        // An outstanding probe remainder is itself a grant: the scheduler has
        // already committed that burst to this path, so capping the flight at
        // the estimate under test would starve the very probe that validates
        // it (a delivery gap just under the probe threshold is still a probe).
        // Mature paths are unaffected: only an at-capacity carrier can reach
        // this point, and one whose delivery supports its capacity model never
        // becomes the probe owner, so it keeps a zero remainder and falls back
        // to the measured target: BDP plus RTT-aware reversal headroom.
        const auto probe_runtime = multipath_scheduler_.GetProbeRuntime();
        const auto discovery_budget_for = [this, &probe_runtime](const vmux_linklayer& carrier,
            const ppp::app::multipath::PathTelemetry& carrier_telemetry) noexcept {
            // The cached remainder is only a grant while this carrier is the
            // scheduler's current burst owner. Otherwise a burst that moved to
            // another carrier (or was cleared by an eligibility/generation
            // change) would keep granting the default budget from a stale
            // per-link cache and silently reduce the window available to the
            // real owner and to borrowers.
            const bool is_probe_owner = probe_runtime.owner_active &&
                probe_runtime.owner_id == carrier.id_;
            return ppp::app::multipath::PathScheduler::DiscoveryAllowanceBytes(
                is_probe_owner, carrier.scheduler_probe_remaining_bytes_,
                ppp::app::multipath::PathScheduler::CapacityProbeNeeded(
                    carrier_telemetry),
                multipath_scheduler_.ProbeBurstBytes(),
                Scheduler::LinkWriteHighWaterCeilingBytes);
        };
        // Build a stateless session borrow plan from the active carriers. Both
        // tx_link_pick and the published runtime statistics call this same
        // function, so their send-credit values share this policy. A plan that
        // cannot be built (too many carriers) falls through to the legacy
        // per-carrier adaptive limit below.
        const auto now = now_tick();
        Scheduler::FlightCreditRequest requests[Scheduler::MaxCandidates] = {};
        Scheduler::FlightCreditAllocation allocations[Scheduler::MaxCandidates] = {};
        std::size_t count = 0;
        bool overflow = false;
        for (const vmux_linklayer_ptr& other : rx_links_) {
            if (!other || other->id_ == 0 || !is_linklayer_active(other)) continue;
            if (count >= Scheduler::MaxCandidates) {
                overflow = true;
                break;
            }
            const auto other_telemetry = other.get() == &link
                ? telemetry : get_link_telemetry(*other, now);
            auto& request = requests[count];
            request = Scheduler::MakeFlightCreditRequest(other->id_,
                other_telemetry, true, now,
                discovery_budget_for(*other, other_telemetry));
            // Only an at-capacity carrier with local byte credit actually
            // applies the planned limit. A carrier still filling its
            // provisional BDP, or one with a full write queue, must not
            // reserve borrowable window slack it would not consume.
            request.borrow_eligible =
                other_telemetry.at_capacity && local_send_credit(*other) != 0;
            ++count;
        }
        if (!overflow && count != 0 &&
            Scheduler::BuildFlightCreditPlan(rtx_cap_bytes_, requests, count,
                PPP_MUX_LINK_BYTE_HIGH_WATER, sizeof(vmux_hdr) + max_buffers_size,
                allocations) == count) {
            for (std::size_t i = 0; i < count; ++i) {
                if (allocations[i].id != link.id_) continue;
                if (allocations[i].eligible) {
                    return std::min(local_credit, allocations[i].credit_bytes);
                }
                break;
            }
        }
        // Legacy fallback: the plan could not be built or had no allocation
        // for this carrier, so keep the old equal-share adaptive limit.
        const auto discovery_budget = discovery_budget_for(link, telemetry);
        const auto min_rtt = telemetry.min_rtt_ms != 0
            ? std::optional<std::uint64_t>(telemetry.min_rtt_ms) : std::nullopt;
        const auto observed_rtt = telemetry.srtt_ms != 0
            ? std::optional<std::uint64_t>(telemetry.srtt_ms) : std::nullopt;
        const auto flight_credit = Scheduler::AdaptiveFlightCredit(
            rtx_cap_bytes_, live_carriers, telemetry.bdp_bytes, telemetry.wire_inflight_bytes,
            telemetry.scheduling_weight, PPP_MUX_LINK_BYTE_HIGH_WATER,
            sizeof(vmux_hdr) + max_buffers_size, discovery_budget, min_rtt, observed_rtt);
        return std::min(local_credit, flight_credit);
    }

    vmux_net::vmux_linklayer_ptr vmux_net::tx_link_pick(
        int length, bool new_payload, uint16_t avoid_link,
        ppp::app::mux::TcpIntent intent) noexcept {
        const int multipath_policy = multipath_scheduler_policy_.load(std::memory_order_acquire);
        if (multipath_policy >= 0) {
            using Scheduler = ppp::app::multipath::PathScheduler;
            std::vector<Scheduler::Candidate> candidates;
            candidates.reserve(rx_links_.size());
            std::uint16_t preferred = std::numeric_limits<std::uint16_t>::max();
            size_t live_carriers = 0;
            for (const auto& link : rx_links_) {
                if (!is_linklayer_active(link)) continue;
                preferred = std::min(preferred, link->id_);
                ++live_carriers;
            }
            const bool adaptive_payload = multipath_policy == 0 && reliability_on_ && new_payload;
            const bool large_latency_payload = intent_routing_enabled() && ordering_mode_ == ordering_flow_v2 &&
                reliability_on_ && intent == ppp::app::mux::TcpIntent::Latency && length > 4096;
            const auto now = now_tick();
            const auto scheduler_sequence = ++scheduler_selection_sequence_;
            const bool trace_enabled = scheduler_trace_writer().enabled;
            std::vector<SchedulerTraceLinkSample> trace_links;
            if (trace_enabled) {
                // Allocated only for the opt-in trace: a failure here must not
                // turn diagnostics into a noexcept termination on the data path.
                try {
                    trace_links.reserve(rx_links_.size());
                    for (const auto& link : rx_links_) {
                        if (!link) continue;
                        SchedulerTraceLinkSample sample;
                        trace_sample_link(*link, sample);
                        sample.wire_inflight_bytes = static_cast<std::uint64_t>(rtx_.LinkBytes(link->id_));
                        trace_links.emplace_back(sample);
                    }
                }
                catch (...) {
                    trace_links.clear();
                }
            }
            for (const auto& link : rx_links_) {
                if (!link || link->id_ == 0) continue;
                ppp::app::multipath::PathControlSnapshot control_snapshot;
                control_snapshot.path_id = link->path_id_;
                control_snapshot.state = is_linklayer_active(link) && link->id_ != avoid_link
                    ? ppp::app::multipath::PathState::Active
                    : ppp::app::multipath::PathState::Draining;
                control_snapshot.telemetry = get_link_telemetry(*link, now);
                control_snapshot.capabilities.active_probe = true;
                const auto& telemetry = control_snapshot.telemetry;
                // The capability declares that this carrier publishes delivery
                // samples, not that its latest capacity estimate is still
                // fresh. The scheduler prefers a fresh capacity estimate and
                // otherwise falls back to the observed delivery rate, so tying
                // the flag to capacity freshness would drop a live sample.
                control_snapshot.capabilities.payload_delivery_rate =
                    link->path_estimator_ != nullptr;
                // Observability only: reports a carrier that accepted the
                // automatic kernel pacing cap; it never changes scheduling.
                control_snapshot.capabilities.pacing = link->pacing_supported_;

                auto send_credit = intent == ppp::app::mux::TcpIntent::Latency
                    ? local_send_credit(*link)
                    : adaptive_payload
                        ? adaptive_send_credit(*link, telemetry, live_carriers)
                        : local_send_credit(*link);
                if (large_latency_payload) {
                    // A connection can switch to bulk payload after its intent
                    // is fixed. Apply a peer-ACK window to those frames so
                    // they cannot queue megabytes on the interactive path.
                    const auto limit = static_cast<std::uint64_t>(PPP_MUX_LINK_BYTE_HIGH_WATER);
                    send_credit = std::min(send_credit, limit - std::min(limit, telemetry.wire_inflight_bytes));
                }
                Scheduler::Candidate candidate = Scheduler::ProjectCandidate(link->id_,
                    control_snapshot, now, send_credit, link->id_ == preferred);
                candidate.path_id = link->path_id_;
                candidate.intent_probe_rtt_ms = link->intent_path_probe_.RecentRttMs(now);
                candidate.intent_probe_timed_out = link->intent_path_probe_.TimedOut(now, link->last_delivery_ack_ms_);
                if (multipath_policy == 0 && reliability_on_) {
                    candidate.bdp_bytes = telemetry.bdp_bytes;
                    candidate.jitter_ms = telemetry.jitter_ms;
                    candidate.scheduling_weight = telemetry.scheduling_weight;
                    candidate.probe_eligible = adaptive_payload;
                }
                candidate.sent_payload_bytes = link->path_runtime_counters_
                    ? link->path_runtime_counters_->tx_unique_payload_bytes.load(std::memory_order_relaxed)
                    : link->total_sent_bytes_;
                candidate.generation = link->generation_;
                candidate.last_active_ms = std::max(link->last_rtt_sample_ms_,
                    link->last_active_.load(std::memory_order_acquire));
                candidates.emplace_back(std::move(candidate));
            }
            // Keep the complete candidate vector so the scheduler's fairness,
            // probe and generation trackers remain valid across intent epochs.
            // Intent routing only narrows eligibility for the current pick.
            // Cross-carrier intent pools require independent per-flow DSNs.
            // Legacy compat ordering has one session-global sequence space and
            // would turn a low-latency/bulk split into session-wide HoL.
            // Keep the first small application payloads on the lowest-RTT
            // group while behavior sampling is still unknown. Restrict this
            // to data frames so ACK/FIN/control traffic keeps the normal
            // credit scheduler and cannot starve a narrow carrier.
            const bool small_unknown_latency = intent_routing_enabled() &&
                ordering_mode_ == ordering_flow_v2 && new_payload &&
                intent == ppp::app::mux::TcpIntent::Unknown &&
                length > 0 && length <= 4096;
            const ppp::app::mux::TcpIntent policy_intent =
                small_unknown_latency ? ppp::app::mux::TcpIntent::Latency : intent;
            const bool intent_filter = intent_routing_enabled() &&
                ordering_mode_ == ordering_flow_v2 &&
                policy_intent != ppp::app::mux::TcpIntent::Unknown;
            if (intent_routing_enabled() && ordering_mode_ == ordering_flow_v2 &&
                std::any_of(candidates.begin(), candidates.end(), [](const Scheduler::Candidate& candidate) {
                    return candidate.eligible && !candidate.intent_probe_timed_out;
                })) {
                for (auto& candidate : candidates) {
                    if (candidate.intent_probe_timed_out) candidate.eligible = false;
                }
            }
            std::vector<bool> base_eligibility;
            std::vector<std::string> latency_groups;
            const std::size_t group_width = std::max<std::size_t>(1, candidates.size() / 3);
            const auto group_name = [&](std::size_t index) {
                if (!candidates[index].path_id.empty()) return candidates[index].path_id;
                return std::string("path") + std::to_string(index / group_width + 1);
            };
            const auto latency_rtt = [](const Scheduler::Candidate& candidate) noexcept {
                if (candidate.intent_probe_rtt_ms != 0) return candidate.intent_probe_rtt_ms;
                return candidate.min_rtt_ms.value_or(candidate.rtt_ms.value_or(500));
            };
            bool bandwidth_preferred_warming = false;
            std::uint16_t alternate_latency_reserve = 0;
            if (intent_filter) {
                const bool latency = policy_intent == ppp::app::mux::TcpIntent::Latency;
                base_eligibility.resize(candidates.size());
                std::vector<std::size_t> eligible;
                eligible.reserve(candidates.size());
                for (std::size_t i = 0; i < candidates.size(); ++i) {
                    base_eligibility[i] = candidates[i].eligible;
                    if (candidates[i].eligible) eligible.emplace_back(i);
                }
                if (latency) {
                    std::stable_sort(eligible.begin(), eligible.end(), [&](std::size_t a, std::size_t b) {
                        const auto left = latency_rtt(candidates[a]);
                        const auto right = latency_rtt(candidates[b]);
                        return left == right ? group_name(a) < group_name(b) : left < right;
                    });
                    for (const auto index : eligible) {
                        const auto group = group_name(index);
                        if (std::find(latency_groups.begin(), latency_groups.end(), group) == latency_groups.end()) {
                            latency_groups.emplace_back(group);
                        }
                    }
                    eligible.erase(std::remove_if(eligible.begin(), eligible.end(), [&](std::size_t index) {
                        return group_name(index) != latency_groups.front();
                    }), eligible.end());
                }
                // At startup the high-capacity group may have no data sample.
                // Keep that group as the bulk default while it is warming.
                else if (candidates.size() >= 3) {
                    const auto rate = [](const Scheduler::Candidate& c) noexcept {
                        if (c.capacity_known && c.throughput_bytes_per_second) {
                            return *c.throughput_bytes_per_second;
                        }
                        return c.delivery_rate_bytes_per_second.value_or(
                            c.throughput_bytes_per_second.value_or(0));
                    };
                    auto group_rate = [&](const std::string& group) noexcept {
                        std::uint64_t best = 0;
                        for (const auto index : eligible) {
                            if (group_name(index) == group) {
                                best = std::max(best, rate(candidates[index]));
                            }
                        }
                        return best;
                    };
                    const auto group_sent_bytes = [&](const std::string& group) noexcept {
                        std::uint64_t total = 0;
                        for (const auto index : eligible) {
                            if (group_name(index) == group) {
                                total = std::max(total, candidates[index].sent_payload_bytes);
                            }
                        }
                        return total;
                    };
                    std::string preferred_group = "path3";
                    const auto preferred_rate = group_rate(preferred_group);
                    const auto alternate_rate = group_rate("path2");
                    constexpr std::uint64_t min_mature_rate = 256 * 1024;
                    const bool preferred_available = std::any_of(eligible.begin(), eligible.end(),
                        [&](std::size_t index) { return group_name(index) == preferred_group; });
                    std::size_t alternate_carriers = 0;
                    for (const auto index : eligible) {
                        if (group_name(index) != "path2") continue;
                        ++alternate_carriers;
                        alternate_latency_reserve = std::max(alternate_latency_reserve, candidates[index].id);
                    }
                    const bool alternate_available = alternate_carriers != 0;
                    // Keep a stable, independent RTT hedge off the bulk TCP
                    // send queue during fallback. A single survivor must
                    // continue servicing both intents.
                    if (alternate_carriers < 2) alternate_latency_reserve = 0;
                    const bool saturated = preferred_available && std::none_of(
                        eligible.begin(), eligible.end(), [&](std::size_t index) {
                            const auto& candidate = candidates[index];
                            return group_name(index) == preferred_group &&
                                (candidate.unacked_bytes == 0 || !candidate.min_rtt_ms || !candidate.rtt_ms ||
                                 *candidate.rtt_ms / 3 < *candidate.min_rtt_ms);
                        });
                    const bool preferred_sampled = group_sent_bytes(preferred_group) >= (4u << 20);
                    const bool rate_collapsed = preferred_rate >= min_mature_rate && alternate_rate / 2 > preferred_rate;
                    // After the hold, give the preferred path a probe interval
                    // to measure its new load. A drained carrier can still have
                    // a high EWMA from its previous flight.
                    constexpr auto probe_interval = ppp::app::mux::MuxIntentPathProbe::IntervalMs;
                    const bool first_demotion = bandwidth_reprobe_after_ms_ == 0;
                    const bool probe_complete = first_demotion || now >= bandwidth_reprobe_after_ms_ + probe_interval;
                    const bool pressured = alternate_available && preferred_sampled && probe_complete &&
                        (saturated || (first_demotion && rate_collapsed));
                    // A single ACK or an initial rate sample can be distorted
                    // by startup or carrier reordering. The first demotion
                    // requires a full interval of pressure. Later recovery
                    // already has its own bounded probe before switching.
                    if (!pressured) {
                        bandwidth_pressure_since_ms_ = 0;
                    }
                    else if (!first_demotion || (bandwidth_pressure_since_ms_ != 0 &&
                            now - bandwidth_pressure_since_ms_ >= probe_interval)) {
                        bandwidth_reprobe_after_ms_ = now + intent_config_.hold_ms;
                        bandwidth_pressure_since_ms_ = 0;
                        ppp::telemetry::Count(saturated ? "mux.intent.bandwidth_queue_hold" :
                            "mux.intent.bandwidth_rate_hold", 1);
                    }
                    else if (bandwidth_pressure_since_ms_ == 0) {
                        bandwidth_pressure_since_ms_ = now;
                    }
                    if (alternate_available && now < bandwidth_reprobe_after_ms_) {
                        preferred_group = "path2";
                    }
                    else if (alternate_available && !probe_complete) {
                        // Sample the recovering path without immediately
                        // refilling its old bulk queue. Remaining traffic can
                        // use the alternate until a fresh probe interval ends.
                        constexpr uint64_t trial_window = PPP_MUX_LINK_BYTE_HIGH_WATER;
                        for (const auto index : eligible) {
                            if (group_name(index) != preferred_group) continue;
                            auto& candidate = candidates[index];
                            const auto room = trial_window - std::min(trial_window, candidate.unacked_bytes);
                            candidate.send_credit_bytes = std::min(candidate.send_credit_bytes.value_or(0), room);
                        }
                    }
                    bandwidth_preferred_warming = preferred_group == "path3" &&
                        preferred_available && preferred_rate < min_mature_rate && probe_complete;
                    eligible.erase(std::remove_if(eligible.begin(), eligible.end(), [&](std::size_t index) {
                        return group_name(index) != preferred_group || candidates[index].id == alternate_latency_reserve;
                    }), eligible.end());
                }
                // Keep latency traffic on its preferred path until every
                // carrier in the group has exhausted its credit.
                const std::size_t keep = latency ? eligible.size() : std::min<std::size_t>(eligible.size(), 2);
                std::sort(eligible.begin(), eligible.end(), [&](std::size_t left, std::size_t right) {
                    const auto& a = candidates[left];
                    const auto& b = candidates[right];
                    if (policy_intent == ppp::app::mux::TcpIntent::Latency) {
                        const auto as = latency_rtt(a);
                        const auto bs = latency_rtt(b);
                        if (as != bs) return as < bs;
                    }
                    else {
                        const auto rate = [](const Scheduler::Candidate& c) noexcept {
                            if (c.capacity_known && c.throughput_bytes_per_second) {
                                return *c.throughput_bytes_per_second;
                            }
                            return c.delivery_rate_bytes_per_second.value_or(
                                c.throughput_bytes_per_second.value_or(0));
                        };
                        const auto ar = rate(a);
                        const auto br = rate(b);
                        if (ar != br) return ar > br;
                        const auto ac = a.send_credit_bytes.value_or(0);
                        const auto bc = b.send_credit_bytes.value_or(0);
                        if (ac != bc) return ac > bc;
                    }
                    // At startup several carriers can have identical unknown
                    // measurements.  Keep latency flows on the first carrier
                    // group, while bandwidth flows start on the later carrier
                    // group.  Using the reverse stable id tie-break for bulk
                    // traffic prevents a narrow first path from filling the
                    // flow-v2 reorder window before its capacity sample is
                    // available.  Once measurements are fresh, the rate/RTT
                    // comparisons above determine the order.
                    return policy_intent == ppp::app::mux::TcpIntent::Bandwidth
                        ? a.id > b.id : a.id < b.id;
                });
                std::vector<bool> keep_link(candidates.size(), false);
                for (std::size_t i = 0; i < std::min(keep, eligible.size()); ++i) {
                    keep_link[eligible[i]] = true;
                }
                for (std::size_t i = 0; i < candidates.size(); ++i) {
                    candidates[i].eligible = candidates[i].eligible && keep_link[i];
                }
            }

            multipath_scheduler_.SetProbeDiagnosticsEnabled(trace_enabled);
            auto selected = multipath_scheduler_.Select(multipath_policy == 1
                ? Scheduler::Policy::Failover : Scheduler::Policy::Adaptive,
                candidates, now, static_cast<std::uint64_t>(std::max(0, length)));
            if (intent_filter && selected.status != Scheduler::Status::Selected) {
                const bool preferred_out_of_credit = selected.status == Scheduler::Status::Backpressure;
                // A stale metric or a concurrent carrier retirement must never
                // strand a classified flow. Try the remaining intent groups
                // in order, and leave the frame queued when every group is
                // out of credit. Opening the complete candidate set here
                // would turn a credit fallback into cross-intent leakage.
                if (policy_intent == ppp::app::mux::TcpIntent::Latency) {
                    for (std::size_t rank = 1; rank < latency_groups.size(); ++rank) {
                        for (std::size_t i = 0; i < candidates.size(); ++i) {
                            candidates[i].eligible = base_eligibility[i] && group_name(i) == latency_groups[rank];
                        }
                        selected = multipath_scheduler_.Select(multipath_policy == 1
                            ? Scheduler::Policy::Failover : Scheduler::Policy::Adaptive,
                            candidates, now, static_cast<std::uint64_t>(std::max(0, length)));
                        if (selected.status == Scheduler::Status::Selected) break;
                    }
                }
                else if (candidates.size() >= 3) {
                    // Keep normal bandwidth fallback on the middle path. The
                    // low-latency path is reserved for latency flows while
                    // any higher-capacity path remains live. Falling back to
                    // path1 here can consume its entire flight credit and
                    // make a subsequent web request migrate to a high-RTT
                    // path merely because the bulk flow arrived first.
                    if (!bandwidth_preferred_warming) {
                        for (std::size_t i = 0; i < candidates.size(); ++i) {
                            candidates[i].eligible = base_eligibility[i] && group_name(i) == "path2" &&
                                candidates[i].id != alternate_latency_reserve;
                        }
                        selected = multipath_scheduler_.Select(multipath_policy == 1
                            ? Scheduler::Policy::Failover : Scheduler::Policy::Adaptive,
                            candidates, now, static_cast<std::uint64_t>(std::max(0, length)));
                    }
                }
                if (selected.status != Scheduler::Status::Selected && candidates.size() < 3) {
                    for (std::size_t i = 0; i < candidates.size(); ++i) {
                        candidates[i].eligible = base_eligibility[i];
                    }
                }
                ppp::telemetry::Count("mux.intent.fallback", 1);
                if (selected.status != Scheduler::Status::Selected && candidates.size() < 3) {
                    selected = multipath_scheduler_.Select(multipath_policy == 1
                        ? Scheduler::Policy::Failover : Scheduler::Policy::Adaptive,
                        candidates, now, static_cast<std::uint64_t>(std::max(0, length)));
                }
                if (intent_runtime_stats_.RecordCreditFallback(preferred_out_of_credit,
                        selected.status == Scheduler::Status::Selected)) {
                    ppp::telemetry::Count("mux.intent.credit_fallback", 1);
                }
            }
            for (const auto& candidate : candidates) {
                for (const auto& link : rx_links_) {
                    if (!link || link->id_ != candidate.id) continue;
                    ++link->scheduler_candidate_observation_count_;
                    link->scheduler_last_observation_sequence_ = scheduler_sequence;
                    link->scheduler_last_observed_send_credit_bytes_ =
                        candidate.send_credit_bytes.value_or(0);
                    link->scheduler_last_observed_queue_bytes_ = candidate.queue_bytes;
                    link->scheduler_last_observed_wire_inflight_bytes_ = candidate.unacked_bytes;
                    link->scheduler_last_observed_probe_remaining_bytes_ =
                        multipath_scheduler_.ProbeRemainingBytes(candidate);
                    break;
                }
            }
            write_scheduler_trace(
                multipath_runtime_session_id_.load(std::memory_order_acquire),
                scheduler_sequence, now, length, new_payload, avoid_link, intent,
                selected, candidates, trace_links,
                static_cast<std::uint64_t>(session_reorder_bytes_),
                reorder_events_,
                static_cast<std::uint64_t>(rx_paused_links_.size()),
                static_cast<std::uint64_t>(tx_data_frames_),
                static_cast<std::uint64_t>(tx_data_bytes_total_),
                static_cast<std::uint64_t>(tx_ctrl_queue_.size()),
                multipath_scheduler_);
            if (selected.status != Scheduler::Status::Selected) return {};
            for (const auto& link : rx_links_) {
                if (!link || link->id_ != selected.carrier_id) continue;
                ++link->scheduler_selection_count_;
                if (selected.probe) {
                    ++link->scheduler_probe_selection_count_;
                    link->scheduler_probe_bytes_ += selected.payload_bytes;
                }
                link->scheduler_probe_remaining_bytes_ = selected.probe_remaining_bytes;
                link->scheduler_last_selection_sequence_ = scheduler_sequence;
                link->scheduler_last_payload_bytes_ = selected.payload_bytes;
                link->scheduler_last_send_credit_bytes_ = selected.selected_send_credit_bytes;
                link->scheduler_last_queue_bytes_ = selected.selected_queue_bytes;
                link->scheduler_last_wire_inflight_bytes_ = selected.selected_wire_inflight_bytes;
                link->scheduler_last_probe_budget_bytes_ = selected.probe_budget_bytes;
                link->scheduler_last_probe_ = selected.probe;
                return link;
            }
            return {};
        }
        // Least bytes in flight first: "free" used to mean "its last local write
        // completed", which a carrier with a bloated socket buffer reports just
        // as quickly as a fast one.
        vmux::vector<ppp::app::mux::MuxLinkLoad> loads;
        vmux::vector<vmux_linklayer_ptr> links;
        loads.reserve(tx_links_.size());
        links.reserve(tx_links_.size());
        for (const vmux_linklayer_ptr& link : tx_links_) {
            if (NULLPTR == link) {
                continue;
            }
            ppp::app::mux::MuxLinkLoad load;
            load.queued_bytes = link->queued_bytes_;
            load.srtt_ms = link->srtt_ms_;
            load.has_rtt = link->has_rtt_sample_;
            load.has_credit = !link->drain_.retiring() && link_has_byte_credit(link, length);
            loads.emplace_back(load);
            links.emplace_back(link);
        }
        const int index = ppp::app::mux::MuxPickLeastLoaded(loads);
        if (index < 0) {
            return vmux_linklayer_ptr();
        }

        // Ties go to the earliest listed carrier, so rotate the pick to the back:
        // equally loaded (e.g. idle) carriers then take turns. Without this every
        // small frame landed on the same carrier, where Nagle (TCP_NODELAY is
        // off unless tcp.turbo) held each one until the previous was TCP-acked,
        // adding about one RTT per request/response exchange.
        vmux_linklayer_ptr chosen = links[(size_t)index];
        tx_links_.remove(chosen);
        tx_links_.emplace_back(chosen);
        return chosen;
    }

    void vmux_net::tx_assign_seq(tx_packet& packet) noexcept {
        // compat: the global sequence is assigned in send order, not enqueue
        // order. DRR serves flows in a different order than they were posted,
        // so enqueue-time numbering reordered frames even on a single carrier.
        if (!packet.assign_seq || NULLPTR == packet.buffer || packet.length < (int)sizeof(vmux_hdr)) {
            return;
        }
        vmux_hdr* h = (vmux_hdr*)packet.buffer.get();
        h->seq = htonl(status_.tx_seq_++);
        packet.assign_seq = false;
    }

    bool vmux_net::tx_send_on(const vmux_linklayer_ptr& linklayer, tx_packet& packet) noexcept {
        tx_assign_seq(packet);
        if (!underlyin_sent(linklayer, packet.buffer, packet.length, packet.completion)) {
            // A carrier refusing a write is going away; its forwarding loop
            // reports the exit. Stop offering it frames meanwhile.
            tx_link_withdraw(linklayer);
            return false;
        }
        if (!link_has_byte_credit(linklayer, (int)sizeof(vmux_hdr))) {
            // The window is full for the next frame: the completion re-offers
            // it, and so does the re-offer pass at drain entry, so a withdrawal
            // here never outlives the residency it was caused by.
            tx_link_withdraw(linklayer);
        }
        return true;
    }

    bool vmux_net::send_intent_path_probe(const vmux_linklayer_ptr& linklayer, Byte kind, uint32_t token) noexcept {
        constexpr int length = sizeof(vmux_hdr) + 2;
        if (!is_linklayer_active(linklayer) || !link_has_byte_credit(linklayer, length)) return false;
        auto buffer = make_byte_array(length);
        if (!buffer) return false;
        auto* header = reinterpret_cast<vmux_hdr*>(buffer.get());
        header->cmd = cmd_keep_alived;
        header->seq = htonl(0);
        header->connection_id = htonl(token);
        auto* payload = reinterpret_cast<Byte*>(header + 1);
        payload[0] = IntentPathProbeMarker;
        payload[1] = kind;
        tx_packet packet{buffer, length, {}};
        return tx_send_on(linklayer, packet);
    }

    void vmux_net::on_link_exit(const vmux_linklayer_ptr& linklayer, const char* reason) noexcept {
        if (base_.disposed_.load(std::memory_order_acquire)) {
            return;
        }

        // One exit per carrier: a failed write and the end of its forwarding loop
        // both report it. Retiring it also keeps late write completions from
        // listing the dead carrier for new frames again.
        const bool planned_retirement = linklayer && linklayer->drain_.retiring();
        if (NULLPTR != linklayer) {
            if (linklayer->exited_) {
                return;
            }
            linklayer->exited_ = true;
            linklayer->drain_.BeginRetire();
        }

        const char* why = (reason != NULLPTR && reason[0] != '\0') ? reason : "unspecified";
        const size_t remaining = count_live_carriers(linklayer);

        remove_linklayer(linklayer);
        rx_forget_paused_link(linklayer);
        if (NULLPTR != linklayer && NULLPTR != linklayer->connection) {
            if (link_exit_callback_) {
                try { link_exit_callback_(linklayer->connection->GetTransmission(), planned_retirement); }
                catch (...) {}
            }
            linklayer->connection->Dispose();
        }

        if (remaining == 0) {
            if (carrier_resume_enabled_.load(std::memory_order_acquire)) {
                const std::uint64_t grace = carrier_resume_grace_ms_.load(std::memory_order_acquire);
                carrier_resume_deadline_ms_.store(now_tick() + grace, std::memory_order_release);
                ppp::telemetry::Count("mux.link.exit.resume_window", 1);
                ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
                    "last carrier exited; retaining session for bounded resume window");
                reinject_link_inflight(linklayer, now_tick());
                process_tx_all_packets();
                return;
            }
            ppp::telemetry::Count("mux.link.exit.session", 1);
            ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
                "last carrier exit reason=%s; closing session", why);
            close_exec();
            return;
        }

        ppp::telemetry::Count("mux.link.exit.isolated", 1);
        ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
            "carrier exit isolated reason=%s remaining=%u", why, (unsigned)remaining);
        reinject_link_inflight(linklayer, now_tick());
        process_tx_all_packets();
    }

    size_t vmux_net::tx_data_depth() const noexcept {
        return tx_data_frames_;
    }

    size_t vmux_net::tx_data_bytes() const noexcept {
        return tx_data_bytes_total_;
    }

    bool vmux_net::enqueue_flow_tx(uint32_t connection_id, tx_packet&& packet) noexcept {
        // connection_id 0 is session control; data frames use non-zero cids.
        // Still route cid=0 through DRR as a single "control-data" bucket if needed.
        flow_tx_context& fx = tx_flows_[connection_id];
        const size_t nbytes = packet.length > 0 ? (size_t)packet.length : 0;
        // Per-flow TX quota: a single slow consumer cannot monopolize the session
        // TX budget. Senders re-couple their read pump at half the cap (see
        // MuxTxShouldCoupleCompletion), so reaching the cap means a stream
        // ignored backpressure. The frame is never dropped silently: under
        // compat its global sequence is already assigned and a silent drop left
        // a hole that only the gap timeout noticed; the caller fails instead.
        if (fx.queue.size() >= (size_t)PPP_MUX_TX_FLOW_MAX_FRAMES ||
            fx.bytes + nbytes > (size_t)PPP_MUX_TX_FLOW_MAX_BYTES) {
            ppp::telemetry::Count("mux.tx.flow_overflow", 1);
            if (ordering_mode_ == ordering_flow_v2) {
                fail_flow(connection_id, "tx_flow_overflow");
            }
            return false;
        }
        fx.queue.emplace_back(std::move(packet));
        fx.bytes += nbytes;
        tx_data_frames_++;
        tx_data_bytes_total_ += nbytes;
        if (!fx.active) {
            fx.active = true;
            fx.deficit = 0;
            fx.quantum_due = true;
            active_tx_flows_.emplace_back(connection_id);
        }
        return true;
    }

    void vmux_net::clear_flow_tx() noexcept {
        tx_flows_.clear();
        active_tx_flows_.clear();
        tx_rtx_parked_flows_.clear();
        tx_data_frames_ = 0;
        tx_data_bytes_total_ = 0;
    }

    void vmux_net::drr_requeue_front(tx_packet&& packet) noexcept {
        const uint32_t cid = peek_connection_id(packet.buffer, packet.length);
        flow_tx_context& fx = tx_flows_[cid];
        const size_t nbytes = packet.length > 0 ? (size_t)packet.length : 0;
        fx.queue.emplace_front(std::move(packet));
        fx.bytes += nbytes;
        tx_data_frames_++;
        tx_data_bytes_total_ += nbytes;
        fx.deficit += static_cast<int64_t>(nbytes);
        fx.quantum_due = false;
        fx.visit_count = 0;
        if (!fx.active) {
            fx.active = true;
            // Do not reset deficit — preserve remaining credit for this round.
            active_tx_flows_.emplace_front(cid);
        }
    }

    void vmux_net::drr_park_front(tx_packet&& packet) noexcept {
        const uint32_t cid = peek_connection_id(packet.buffer, packet.length);
        drr_requeue_front(std::move(packet));

        // drr_pop_next / drr_requeue_front may have put the cid back on the ring;
        // a parked flow is not scheduled again until an ACK resumes it. It stays
        // "active" so enqueue_flow_tx keeps queueing behind it without re-adding.
        active_tx_flows_.remove(cid);
        flow_tx_context& fx = tx_flows_[cid];
        fx.active = true;
        if (!fx.rtx_parked) {
            fx.rtx_parked = true;
            tx_rtx_parked_flows_.emplace_back(cid);
        }
    }

    bool vmux_net::drr_unpark_all() noexcept {
        if (tx_rtx_parked_flows_.empty()) {
            return false;
        }

        for (uint32_t cid : tx_rtx_parked_flows_) {
            auto it = tx_flows_.find(cid);
            if (it == tx_flows_.end() || !it->second.rtx_parked) {
                continue; // Flow failed / re-created since it was parked.
            }

            flow_tx_context& fx = it->second;
            fx.rtx_parked = false;
            if (fx.queue.empty()) {
                fx.active = false;
                fx.deficit = 0;
                fx.quantum_due = true;
            }
            else {
                active_tx_flows_.emplace_back(cid);
            }
        }
        tx_rtx_parked_flows_.clear();
        return true;
    }

    bool vmux_net::tx_flow_parked(uint32_t connection_id) const noexcept {
        auto it = tx_flows_.find(connection_id);
        return it != tx_flows_.end() && it->second.rtx_parked;
    }

    bool vmux_net::drr_pop_next(tx_packet& out, bool interactive_only) noexcept {
        if (active_tx_flows_.empty()) {
            return false;
        }

        const size_t quantum = (size_t)PPP_MUX_TX_FLOW_QUANTUM_BYTES;
        const size_t q = quantum > 0 ? quantum : (size_t)1;
        const size_t n = active_tx_flows_.size();

        // Keep cycling the ring until some flow has enough deficit. A frame can
        // be up to 64 KiB while the quantum is smaller, so one cycle may end with
        // nothing sendable; returning then left the frame queued until an
        // unrelated event (new post, write completion, ACK) called us again, one
        // quantum per event. Bounded: after ceil(max_frame / quantum) + 1 rounds
        // every flow's head frame fits its deficit.
        const size_t max_rounds = ((size_t)UINT16_MAX + q - 1) / q + 1;
        const size_t budget = n * max_rounds;
        for (size_t scanned = 0; scanned < budget && !active_tx_flows_.empty(); ++scanned) {
            uint32_t cid = active_tx_flows_.front();
            active_tx_flows_.pop_front();

            auto it = tx_flows_.find(cid);
            if (it == tx_flows_.end() || it->second.queue.empty()) {
                if (it != tx_flows_.end()) {
                    it->second.active = false;
                    it->second.deficit = 0;
                    it->second.quantum_due = true;
                    if (it->second.queue.empty() && it->second.bytes == 0) {
                        tx_flows_.erase(it);
                    }
                }
                continue;
            }

            flow_tx_context& fx = it->second;
            if (interactive_only) {
                const auto& head = fx.queue.front();
                const bool interactive = head.length >= static_cast<int>(sizeof(vmux_hdr)) &&
                    head.length <= 4096 && head.buffer &&
                    reinterpret_cast<const vmux_hdr*>(head.buffer.get())->cmd == cmd_push &&
                    flow_intent(cid) != ppp::app::mux::TcpIntent::Bandwidth;
                const bool repair_in_this_flow = interactive &&
                    std::any_of(rtx_pending_.begin(), rtx_pending_.end(), [cid](uint64_t key) {
                        return ppp::app::mux::MuxRetransmitBuffer::KeyCid(key) == cid;
                    });
                if (!interactive || repair_in_this_flow) {
                    active_tx_flows_.emplace_back(cid);
                    continue;
                }
            }
            if (fx.quantum_due) {
                fx.deficit += static_cast<int64_t>(q);
                fx.quantum_due = false;
            }

            // Per-visit frame cap: even when deficit allows, yield to the next
            // flow after max_visit frames so a single elephant flow cannot
            // monopolize an entire drain turn at the expense of mice flows.
            if (fx.visit_count >= (uint32_t)PPP_MUX_TX_FLOW_MAX_VISIT_FRAMES) {
                fx.visit_count = 0;
                fx.quantum_due = true;
                active_tx_flows_.emplace_back(cid);
                continue;
            }

            // Skip this flow for this round if the head frame exceeds remaining deficit.
            // (Classic DRR: only send when deficit >= packet size.)
            const int head_len = fx.queue.front().length;
            if (head_len > 0 && fx.deficit < (int64_t)head_len) {
                // Preserve unused credit and add another quantum next round.
                fx.visit_count = 0;
                fx.quantum_due = true;
                active_tx_flows_.emplace_back(cid);
                continue;
            }

            out = std::move(fx.queue.front());
            fx.queue.pop_front();
            fx.visit_count++;
            const size_t nbytes = out.length > 0 ? (size_t)out.length : 0;
            if (nbytes <= fx.bytes) {
                fx.bytes -= nbytes;
            }
            else {
                fx.bytes = 0;
            }
            if (tx_data_frames_ > 0) {
                tx_data_frames_--;
            }
            if (nbytes <= tx_data_bytes_total_) {
                tx_data_bytes_total_ -= nbytes;
            }
            else {
                tx_data_bytes_total_ = 0;
            }
            fx.deficit -= (int64_t)(nbytes > 0 ? nbytes : 0);
            if (fx.deficit < 0) {
                fx.deficit = 0;
            }

            if (fx.queue.empty()) {
                fx.active = false;
                fx.deficit = 0;
                fx.quantum_due = true;
                fx.visit_count = 0;
                // Leave empty context; cleaned on next miss or finalize.
            }
            else if (fx.queue.front().length <= 0 ||
                fx.deficit >= static_cast<int64_t>(fx.queue.front().length)) {
                // Continue this flow's turn until its byte deficit is exhausted.
                active_tx_flows_.emplace_front(cid);
            }
            else {
                fx.quantum_due = true;
                fx.visit_count = 0;
                active_tx_flows_.emplace_back(cid);
            }
            return true;
        }

        return false;
    }

    void vmux_net::publish_runtime_snapshot_locked() noexcept {
        // Caller holds runtime_state_mutex_ OR is the sole strand mutator of runtime_state_.
        auto snap = std::make_shared<const ppp::app::mux::MuxRuntimeState>(runtime_state_);
        std::atomic_store_explicit(&runtime_snapshot_, std::move(snap), std::memory_order_release);
    }

    /**
     * @brief Performs first-time-transfer sequence initialization/validation.
     */
    bool vmux_net::ftt(uint32_t seq, uint32_t ack) noexcept {
        SynchronizationObjectScope __SCOPE__(syncobj_);
        if (base_.disposed_.load(std::memory_order_acquire)) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionDisposed);
            return false;
        }
        
        if (!base_.ftt_) {
            base_.ftt_ = true;
            status_.tx_seq_ = seq;
            status_.rx_ack_ = ack;
            // Server TX is client RX; both peers expose the same session identifier.
            const uint32_t client_tx = base_.server_or_client_ ? ack : seq;
            const uint32_t client_rx = base_.server_or_client_ ? seq : ack;
            multipath_runtime_session_id_.store(
                (static_cast<uint64_t>(client_tx) << 32) | client_rx, std::memory_order_release);
        }

        return (status_.tx_seq_ == seq) && (status_.rx_ack_ == ack);
    }

    /** @brief Produces a randomized signed identifier encoded as uint32_t. */
    uint32_t vmux_net::ftt_random_aid(int min, int max) noexcept {
        int a = ppp::RandomNext();
        int b = a & 1;
        if (b != 0) {
            return (uint32_t)-ppp::RandomNext(min, max);
        }
        else {
            return (uint32_t)ppp::RandomNext(min, max);
        }
    }

    /** @brief Begins terminal close and finalizes on the VMUX strand (inline when the executor is stopped). */
    void vmux_net::close_exec() noexcept {
        std::shared_ptr<vmux_net> self = weak_from_this().lock();
        if (NULLPTR == self || !self->begin_close()) {
            return;
        }

        if (NULLPTR != self->strand_ && self->strand_->running_in_this_thread()) {
            self->finalize();
            return;
        }

        const ContextPtr& context = self->context_;
        if (NULLPTR == context || context->stopped() ||
            !vmux_post_exec(context, self->strand_,
                [self]() noexcept {
                    self->finalize();
                })) {
            // Stopped executor: a posted finalizer would never run and the session
            // would leak. finalize() detects this and tears down inline.
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeTaskPostFailed);
            self->finalize();
        }
    }

    /**
     * @brief Writes a packet via transmission and dispatches completion on vmux strand.
     */
    static bool transmission_write(
        std::shared_ptr<vmux_net>                                           self,
        const vmux_net::ITransmissionPtr&                                   transmission,
        const std::shared_ptr<Byte>&                                        packet,
        int                                                                 packet_length,
        const vmux_net::vmux_linklayer_ptr&                                   linklayer,
        std::uint64_t                                                       trace_post_us,
        bool                                                                trace_enabled,
        const ppp::transmissions::ITransmission::AsynchronousWriteCallback& ac,
        const ppp::function<void()>&                                        accounting_fallback) noexcept {

        ContextPtr context = transmission->GetContext();
        StrandPtr strand = transmission->GetStrand();
        ContextPtr vmux_context = self->get_context();
        if (NULLPTR == context || NULLPTR == vmux_context || context->stopped() || vmux_context->stopped()) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeTaskPostFailed);
            return false;
        }

        // Optional write-latency sample: post_us is taken on the vmux strand before
        // the carrier post, write_start_us on the carrier strand just before Write().
        // A zero write_start_us marks a write that never reached Write() (the frame
        // was not forwarded), so it is not sampled.
        const ppp::function<void(bool, std::uint64_t)> on_completely =
            [self, ac, accounting_fallback, linklayer, trace_post_us, trace_enabled](bool successed, std::uint64_t write_start_us) noexcept {
                if (trace_enabled && write_start_us != 0 && linklayer) {
                    trace_record_tx_write(*linklayer, trace_post_us, write_start_us, trace_now_us());
                }
                const ContextPtr callback_context = self->get_context();
                const bool posted = NULLPTR != callback_context && !callback_context->stopped() &&
                    vmux_post_exec(callback_context, self->get_strand(),
                        [self, successed, ac]() noexcept {
                            ac(successed);
                        });
                if (!posted && NULLPTR != accounting_fallback) {
                    accounting_fallback();
                }
            };

        bool posted = vmux_post_exec(context, strand,
            [self, transmission, context, strand, packet, packet_length, on_completely, trace_enabled]() noexcept {
                const std::uint64_t write_start_us = trace_enabled ? trace_now_us() : 0;
                bool forwarding =
                    transmission->Write(packet.get(), packet_length,
                        [self, context, strand, on_completely, write_start_us](bool ok) noexcept {
                            on_completely(ok, write_start_us);
                        });

                if (!forwarding) {
                    on_completely(false, 0);
                }
            });

        if (!posted) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeTaskPostFailed);
        }

        return posted;
    }
    
    /**
     * @brief Sends one packet through the specified underlying linklayer.
     */
    bool vmux_net::underlyin_sent(const vmux_linklayer_ptr& linklayer, const std::shared_ptr<Byte>& packet, int packet_length, const tx_completion_ptr& completion, bool retransmission) noexcept {
        if (NULLPTR == linklayer || NULLPTR == packet || packet_length < sizeof(vmux_hdr)) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolFrameInvalid);
            return false;
        }
        
        if (base_.disposed_.load(std::memory_order_acquire) || close_requested()) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionDisposed);
            return false;
        }

        IMuxTransportPtr& connection = linklayer->connection;
        if (NULLPTR == connection || !connection->IsLinked()) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionTransportMissing);
            return false;
        }

        ITransmissionPtr transmission = connection->GetTransmission();
        if (NULLPTR == transmission) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionTransportMissing);
            return false;
        }

        if (!link_has_byte_credit(linklayer, packet_length)) {
            ppp::telemetry::Count("mux.link.byte_credit.deny", 1);
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolMuxFailed);
            return false;
        }

        std::shared_ptr<vmux_net> self = weak_from_this().lock();
        if (NULLPTR == self) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionDisposed);
            return false;
        }
        ppp::telemetry::Count("mux.link.send", 1);
        // Track in-flight writes per link (strand-affine) so runtime link removal
        // (turbo dynamic pool) can retire a link only after its last write
        // completes — a late completion must never touch a freed/retired link's
        // scheduling state.
        const ppp::app::mux::MuxLinkDrainState::WriteTicket write =
            linklayer->drain_.BeginWrite();
        if (!write) {
            return false;
        }

        linklayer->queued_bytes_ += (size_t)packet_length;
        linklayer->total_sent_bytes_ += (uint64_t)packet_length;
        const int accounted_length = packet_length;

        const bool trace_enabled = scheduler_trace_writer().enabled;
        const std::uint64_t trace_post_us = trace_enabled ? trace_now_us() : 0;
        bool queued = transmission_write(self, transmission, packet, packet_length,
            linklayer, trace_post_us, trace_enabled,
            [self, this, linklayer, completion, write, accounted_length](bool ok) noexcept {
                // Decrement in-flight first; this completion is accounted regardless
                // of what follows. Runtime removal checks inflight_ == 0 to retire.
                if (!linklayer->drain_.CompleteWrite(write)) {
                    self->finish_tx_completion(completion, ok);
                    return;
                }

                if ((size_t)accounted_length <= linklayer->queued_bytes_) {
                    linklayer->queued_bytes_ -= (size_t)accounted_length;
                }
                else {
                    linklayer->queued_bytes_ = 0;
                }

                if (!ok && completion && completion->retained_payload_key != 0) {
                    if (!base_.disposed_.load(std::memory_order_acquire) && !close_requested()) {
                        on_link_exit(linklayer, "write_failed");
                    }
                    // The bounded retransmit window already owns this PUSH.
                    // Closing the application stream here would discard later
                    // bytes even if another carrier delivers the original DSN.
                    const auto socket = get_connection(ppp::app::mux::MuxRetransmitBuffer::KeyCid(
                        completion->retained_payload_key));
                    const bool accepted = !close_requested() && socket && !socket->status_.fin_;
                    if (accepted) {
                        ppp::telemetry::Count("mux.rtx.write_recovered", 1);
                    }
                    self->finish_tx_completion(completion, accepted);
                    return;
                }

                self->finish_tx_completion(completion, ok);

                // Teardown guard: a send may complete after the session has been
                // finalized (link flap, idle timeout, or peer close). finalize()
                // clears tx_links_/tx_flows_ under syncobj_; touching them again
                // from this strand callback (emplace_back / erase / re-drain) would
                // race the teardown and operate on freed list nodes. Once closing or
                // disposed, no scheduler state may be touched again.
                if (base_.disposed_.load(std::memory_order_acquire) || self->close_requested()) {
                    return;
                }

                // A link being retired for runtime removal stops taking new frames;
                // do not re-credit it. Once its in-flight drains to 0 the periodic
                // maintenance path removes it. Re-drive the scheduler so queued
                // frames continue on the remaining links.
                if (linklayer->drain_.retiring()) {
                    process_tx_all_packets();
                    return;
                }

                if (!ok) {
                    // Local write failed: isolate this carrier when others remain.
                    on_link_exit(linklayer, "write_failed");
                    return;
                }

                // The write freed in-flight budget on this carrier: list it again
                // (if it has room) and let the scheduler place the next frames by
                // policy, on whichever carrier is least loaded.
                tx_link_offer(linklayer);
                if (!process_tx_all_packets()) {
                    on_link_exit(linklayer, "tx_drain_failed");
                }
            },
            [self, linklayer, completion, write]() noexcept {
                // This fallback may run on the carrier executor. MuxLinkDrainState
                // is thread-safe; queued_bytes_ and scheduler containers are not.
                (void)linklayer->drain_.AbortWrite(write);
                self->finish_tx_completion(completion, false);
            });
        if (!queued) {
            if ((size_t)accounted_length <= linklayer->queued_bytes_) {
                linklayer->queued_bytes_ -= (size_t)accounted_length;
            }
            else {
                linklayer->queued_bytes_ = 0;
            }
            (void)linklayer->drain_.AbortWrite(write);
        }

        // Reliability: retain a copy for retransmission and fold data frames
        // into the running FEC parity group once the frame is on the wire.
        if (queued) {
            const Byte cmd = reinterpret_cast<const vmux_hdr*>(packet.get())->cmd;
            if (const auto& counters = linklayer->path_runtime_counters_) {
                using Counters = vmux_linklayer::PathRuntimeCounters;
                counters->Add(&Counters::tx_bytes, static_cast<uint64_t>(packet_length));
                if (!retransmission && cmd == cmd_push) {
                    counters->Add(&Counters::tx_unique_payload_bytes,
                        static_cast<uint64_t>(packet_length) - sizeof(vmux_hdr));
                }
            }
            const uint64_t sent_now = now_tick();
            if (track_sent_frame(packet, packet_length, sent_now, linklayer->id_, linklayer.get())) {
                if (completion && ordering_mode_ == ordering_flow_v2 && cmd == cmd_push) {
                    const auto* header = reinterpret_cast<const vmux_hdr*>(packet.get());
                    const auto key = ppp::app::mux::MuxRetransmitBuffer::Key(
                        ntohl(header->connection_id), ntohl(header->seq));
                    std::lock_guard<std::mutex> scope(tx_completion_mutex_);
                    completion->retained_payload_key = key;
                    if (pending_tx_completions_.find(completion) != pending_tx_completions_.end()) {
                        pending_payload_tx_completions_.emplace(key, completion);
                    }
                }
                fec_note_sent(packet, packet_length, sent_now);
            }
        }
        return queued;
    }

    /**
     * @brief Periodically updates timeout state and closes stale sockets.
     */
    bool vmux_net::update() noexcept {
        if (base_.disposed_.load(std::memory_order_acquire)) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionDisposed);
            return false;
        }

        std::shared_ptr<vmux_net> self = shared_from_this();
        bool posted = vmux_post_exec(context_, strand_,
            [self, this]() noexcept {
                list<vmux_skt_ptr> release_skts;

                uint64_t max_tcp_inactive_timeout = ((uint64_t)AppConfiguration->tcp.inactive.timeout) * 1000ULL;
                uint64_t max_tcp_connect_timeout = ((uint64_t)AppConfiguration->tcp.connect.timeout) * 1000ULL;

                uint64_t now = now_tick();
                const uint64_t carrier_resume_deadline = carrier_resume_deadline_ms_.load(std::memory_order_acquire);
                if (carrier_resume_deadline != 0 && now >= carrier_resume_deadline) {
                    carrier_resume_deadline_ms_.store(0, std::memory_order_release);
                    close_exec();
                    return;
                }
                if (base_.established_) {
                    for (const std::pair<uint32_t, vmux_skt_ptr>& kv : skts_) {
                        bool is_port_aging = false;
                        const vmux_skt_ptr& skt = kv.second;

                        uint64_t delta_time = now - skt->last_;
                        if (skt->status_.connected_) {
                            is_port_aging = delta_time >= max_tcp_inactive_timeout;
                        }
                        else {
                            is_port_aging = delta_time >= max_tcp_connect_timeout;
                        }

                        if (is_port_aging) {
                            release_skts.emplace_back(skt);
                        }
                    }
                }

                /**
                 * @brief Complex maintenance step:
                 * - close per-socket idle/connect-timeout entries,
                 * - enforce global mux inactivity timeout,
                 * - schedule heartbeat keepalive when established.
                 */
                uint64_t max_mux_inactive_timeout = ((uint64_t)AppConfiguration->mux.inactive.timeout) * 1000ULL;
                uint64_t max_mux_connect_timeout = ((uint64_t)AppConfiguration->mux.connect.timeout) * 1000ULL;

                const bool carrier_resume_pending = carrier_resume_deadline_ms_.load(std::memory_order_acquire) != 0;
                if (!carrier_resume_pending &&
                    (now - status_.last_) >= (base_.established_ ? max_mux_inactive_timeout : max_mux_connect_timeout)) {
                    close_exec();
                }
                elif(base_.established_ && (now - status_.last_heartbeat_) >= status_.heartbeat_timeout_) {
                    if (post(cmd_keep_alived, NULLPTR, 0, ftt_random_aid(1, INT32_MAX))) {
                        status_.last_heartbeat_ = now;
                        switch_to_next_heartbeat_timeout();
                    }
                }

                for (vmux_skt_ptr& skt : release_skts) {
                    skt->close();
                }

                /**
                 * @brief Debug-only one-shot mux-mode push.
                 *
                 * Once the session is established, if a transient
                 * `--mux-mode-set` request and a non-empty `--debug-key` are
                 * configured locally, push the requested scheduler mode to the
                 * peer exactly once. The peer applies it only when its own
                 * debug key matches (see packet_input_mux_mode_set).
                 */
                if (base_.established_ && !mux_mode_set_pushed_ && NULLPTR != AppConfiguration) {
                    const ppp::string& set_mode = AppConfiguration->mux.debug.set_mode;
                    if (!set_mode.empty() && !AppConfiguration->mux.debug.key.empty()) {
                        if (post_mux_mode_set(parse_mode(set_mode))) {
                            mux_mode_set_pushed_ = true;
                        }
                    }
                    else {
                        mux_mode_set_pushed_ = true; // Nothing to push; do not re-check every tick.
                    }
                }

                /**
                 * @brief Scheduler observability (Phase 2 telemetry):
                 * publish the active scheduler mode together with the transmit
                 * queue depth, the out-of-order reorder queue depth, and the
                 * number of attached link-layers. These run on the vmux strand,
                 * so reading the queues/link containers here is race-free.
                 */
                if (!base_.disposed_.load(std::memory_order_acquire)) {
                    // flow-v2: advance any per-connection gap whose wait timed out so a
                    // permanently lost frame cannot stall that connection's delivery.
                    flow_evict_expired(now);
                    compat_evict_expired(now);

                    // turbo dynamic pool: dispose any carrier link that finished
                    // retiring (its in-flight writes drained to 0) since last tick.
                    reap_retired_linklayers();

                    // turbo dynamic pool controller: move the pool one step toward
                    // the quality-derived target (grow request / shrink retire).
                    turbo_controller_tick(now);
                    // Automatic per-carrier TCP pacing: also runs while idle so
                    // an expired capacity sample clears the kernel cap.
                    for (const vmux_linklayer_ptr& link : rx_links_) {
                        if (!link) continue;
                        update_link_pacing(*link, now);
                        if (!reliability_on_ && base_.established_ && ordering_mode_ == ordering_flow_v2 &&
                            intent_routing_enabled() && link->intent_path_probe_.Due(now) &&
                            send_intent_path_probe(link, IntentPathProbeRequest, static_cast<uint32_t>(now))) {
                            link->intent_path_probe_.Sent(now);
                        }
                    }
                    publish_multipath_path_runtime_stats();

                    // D11 stall watchdog: if the data tx queue stays at/over the
                    // high-water mark for longer than the stall timeout, the send
                    // side is wedged (carrier not draining; new connections starve)
                    // and cannot self-heal. Tear the session down so it is rebuilt,
                    // rather than hang forever serving nothing. Control frames have
                    // their own priority queue, so this only triggers on a genuine
                    // data-path wedge.
                    size_t tx_depth = tx_data_frames_;
                    if (tx_depth >= tx_queue_high_water_) {
                        if (tx_backlog_since_ == 0) {
                            tx_backlog_since_ = now;
                        }
                        elif((now - tx_backlog_since_) >= tx_backlog_stall_ms_) {
                            ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
                                "tx backlog stall watchdog: depth=%d held >= %dms, rebuilding session",
                                (int)tx_depth, (int)tx_backlog_stall_ms_);
                            ppp::telemetry::Count("mux.tx.backlog.stall", 1);
                            close_exec();
                        }
                    }
                    else {
                        tx_backlog_since_ = 0;
                    }

                    ppp::telemetry::Gauge("mux.sched.mode", static_cast<int64_t>(mode_));
                    ppp::telemetry::Gauge("mux.tx.queue.depth", static_cast<int64_t>(tx_data_frames_));
                    ppp::telemetry::Gauge("mux.rx.reorder.depth", static_cast<int64_t>(rx_queue_.size()));
                    ppp::telemetry::Gauge("mux.link.count", static_cast<int64_t>(rx_links_.size()));
                    // Scheduler liveness: a non-empty data queue with zero free
                    // links and zero in-flight writes means a link was dropped
                    // from tx_links_ without being returned.
                    size_t link_inflight = 0;
                    for (const vmux_linklayer_ptr& link : rx_links_) {
                        if (NULLPTR != link) {
                            link_inflight += link->drain_.inflight();
                        }
                    }
                    ppp::telemetry::Gauge("mux.tx.free_links", static_cast<int64_t>(tx_links_.size()));
                    ppp::telemetry::Gauge("mux.link.inflight", static_cast<int64_t>(link_inflight));
                    ppp::telemetry::Gauge("mux.tx.ctrl.depth", static_cast<int64_t>(tx_ctrl_queue_.size()));
                    ppp::telemetry::Gauge("mux.tx.rtx_parked", static_cast<int64_t>(tx_rtx_parked_flows_.size()));
                    ppp::telemetry::Gauge("mux.rtx.bytes", static_cast<int64_t>(rtx_.bytes()));
                }
            });

        if (!posted) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeTaskPostFailed);
        }

        return posted;
    }

    /** @brief Selects the next randomized heartbeat timeout window. */
    void vmux_net::switch_to_next_heartbeat_timeout() noexcept {
        int min = std::max<int>(0, AppConfiguration->mux.keep_alived[0]);
        int max = std::max<int>(0, AppConfiguration->mux.keep_alived[1]);
        if (min > max) {
            std::swap(min, max);
        }

        if (max == 0) {
            max = AppConfiguration->mux.connect.timeout;
        }

        min = std::max<int>(1, min) * 1000;
        max = std::max<int>(1, max) * 1000;
        status_.heartbeat_timeout_ = ppp::RandomNext(min, max + 1);
    }

    /**
     * @brief Processes in-order/out-of-order packets and advances ACK state.
     */
    bool vmux_net::packet_input_unorder(const vmux_linklayer_ptr& linklayer, vmux_hdr* h, int length, uint64_t now, const std::shared_ptr<Byte>& owner, bool fec_recovered) noexcept {
        // Prepare the ack frames.
        if (base_.disposed_.load(std::memory_order_acquire)) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionDisposed);
            return false;
        }

        ppp::telemetry::Count("mux.link.recv", 1);

        // Reliability control frames are unordered in both modes: handle them
        // inline before any sequence-space reasoning (they carry seq=0).
        if (is_reliability_control(h->cmd)) {
            if (!packet_input(h->cmd, (Byte*)h, length, now, owner)) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolPacketActionInvalid);
                return false;
            }

            active(now);
            linklayer_update(linklayer);
            return true;
        }

        const auto source = linklayer && !fec_recovered ? linklayer->path_runtime_counters_ : nullptr;
        uint32_t seq = ntohl(h->seq);
        if (status_.rx_ack_ == seq) {
            if (packet_input(h->cmd, (Byte*)h, length, now, owner, source)) {
                status_.rx_ack_++;
            }
            else {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolPacketActionInvalid);
                return false;
            }

            for (;;) {
                rx_packet_ssqueue::iterator packet_tail = rx_queue_.begin();
                rx_packet_ssqueue::iterator packet_endl = rx_queue_.end();
                if (packet_tail != packet_endl && status_.rx_ack_ == packet_tail->first) {
                    rx_ordered_packet i = std::move(packet_tail->second);
                    vmux_hdr* p = (vmux_hdr*)i.buffer.get();
                    rx_queue_.erase(packet_tail);

                    note_flow_unbuffered(static_cast<size_t>(i.length));

                    if (packet_input(p->cmd, (Byte*)p, i.length, now, i.buffer, i.source)) {
                        status_.rx_ack_++;
                    }
                    else {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolPacketActionInvalid);
                        return false;
                    }
                }
                else {
                    break;
                }
            }

            // In-order progress closed the head-of-line gap: restart the gap clock
            // for whatever is still buffered instead of keeping the first gap's
            // tick, which under sustained multi-carrier load would time out the
            // session even though every individual gap closed quickly.
            rx_gap_oldest_tick_ = ppp::app::mux::MuxGapClockAfterAdvance(now, !rx_queue_.empty());
            rx_maybe_resume();

            active(now);
            linklayer_update(linklayer);
            return true;
        }
        elif(packet_less<uint32_t>::after(seq, status_.rx_ack_)) {
            /**
             * @brief Complex reorder path:
             * buffers future packets by sequence and replays contiguous packets once
             * the missing sequence is received.
             */
            // Protect against absurd packet sizes and allocate within limit.
            // 'length' here includes the vmux_hdr; ensure it's at least a header
            // and does not exceed header + max_buffers_size.
            if (length < sizeof(vmux_hdr) || length > (sizeof(vmux_hdr) + max_buffers_size)) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolFrameInvalid);
                return false;
            }

            // The session cap is soft: above it keep buffering but pause the
            // carrier that delivered this frame, so TCP flow control throttles
            // the sender until the lagging frame arrives. Only the hard bound
            // (cap * PPP_MUX_REORDER_HARD_FACTOR) still rebuilds the session.
            const ppp::app::mux::MuxReorderAdmit admit = ppp::app::mux::AdmitReorderBytes(
                session_reorder_bytes_, static_cast<size_t>(length),
                session_reorder_cap_bytes_, (size_t)PPP_MUX_REORDER_HARD_FACTOR);
            if (admit == ppp::app::mux::MuxReorderAdmit::Reject) {
                ppp::telemetry::Count("mux.rx.compat.reorder_cap", 1);
                ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
                    "compat reorder hard cap exceeded: buffered=%u incoming=%u cap=%u hard=%u; rebuilding session",
                    (unsigned)session_reorder_bytes_, (unsigned)length,
                    (unsigned)session_reorder_cap_bytes_,
                    (unsigned)ppp::app::mux::MuxReorderHardCap(session_reorder_cap_bytes_, (size_t)PPP_MUX_REORDER_HARD_FACTOR));
                close_exec();
                active(now);
                linklayer_update(linklayer);
                return true;
            }

            std::shared_ptr<Byte> buf;
            if (NULLPTR != owner) {
                buf = ppp::wrap_shared_pointer(reinterpret_cast<ppp::Byte*>(h), owner);
            } else {
                buf = make_byte_array(length);
                if (NULLPTR != buf) {
                    memcpy(buf.get(), h, length);
                }
            }
            if (NULLPTR == buf) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetReorderPacketBufferAllocFailed);
                return false;
            }

            rx_ordered_packet packet = {{std::move(buf), length}, source};

            bool inserted = rx_queue_.emplace(std::make_pair(seq, packet)).second;
            if (!inserted) {
                // Duplicate of an already-buffered out-of-order frame (a
                // retransmit): with reliability negotiated this is expected.
                if (reliability_on_) {
                    active(now);
                    linklayer_update(linklayer);
                    return true;
                }
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::MappingEntryConflict);
            }
            else {
                note_flow_buffered(static_cast<size_t>(length));
                rx_gap_oldest_tick_ = ppp::app::mux::MuxGapClockOnBuffered(rx_gap_oldest_tick_, now);
                if (admit == ppp::app::mux::MuxReorderAdmit::AcceptAndPause) {
                    rx_pause_link(linklayer);
                }
            }

            // Any valid framed traffic (including OOO) proves peer liveness.
            active(now);
            linklayer_update(linklayer);
            return inserted;
        }
        else {
            // Stale/duplicate (already delivered): with reliability negotiated
            // this is an expected retransmit duplicate — drop it silently.
            if (reliability_on_) {
                active(now);
                linklayer_update(linklayer);
                return true;
            }

            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolFrameInvalid);
            return false;
        }
    }

    /** @brief Delivers payload data to one logical vmux socket. */
    void vmux_net::packet_input_read(uint32_t connection_id, Byte* buffer, int buffer_size, uint64_t now, const std::shared_ptr<Byte>& owner, const std::shared_ptr<vmux_linklayer::PathRuntimeCounters>& source) noexcept {
        vmux_skt_ptr skt = get_connection(connection_id);
        if (NULLPTR != skt) {
            if (skt->input(owner, buffer, buffer_size)) {
                if (get_pool_path_floor() > 0 &&
                    flow_intent(connection_id) == ppp::app::mux::TcpIntent::Bandwidth) {
                    pool_received_bulk_bytes_ += static_cast<uint64_t>(buffer_size);
                }
                if (source) {
                    using Counters = vmux_linklayer::PathRuntimeCounters;
                    source->Add(&Counters::rx_unique_payload_bytes, static_cast<uint64_t>(buffer_size));
                    if (flow_trace_writer().enabled) {
                        const auto path = std::find_if(path_runtime_counters_.begin(),
                            path_runtime_counters_.end(), [&](const auto& entry) {
                                return source->path_totals == entry.second;
                            });
                        trace_flow_delivery(connection_id, path == path_runtime_counters_.end()
                            ? std::string{} : path->first, buffer_size);
                    }
                }
                skt->active(now);
            }
            else {
                skt->close();
            }
        }
    }

    /**
     * @brief Dispatches an incoming vmux command frame to its handler.
     */
    bool vmux_net::packet_input(Byte cmd, Byte* buffer, int buffer_size, uint64_t now, const std::shared_ptr<Byte>& owner, const std::shared_ptr<vmux_linklayer::PathRuntimeCounters>& source) noexcept {
        buffer_size -= sizeof(vmux_hdr);
        if (buffer_size < 0) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolFrameInvalid);
            return false;
        }

        vmux_hdr* h = (vmux_hdr*)buffer;
        buffer = (Byte*)(h + 1);

        uint32_t connection_id = ntohl(h->connection_id);
        if (cmd == cmd_push) {
            // An empty PUSH is a no-op; only cmd_fin may end a stream.
            if (!ppp::app::mux::MuxIsNoopPush(buffer_size)) {
                packet_input_read(connection_id, buffer, buffer_size, now, owner, source);
            }
        }
        elif(cmd == cmd_fin) {
            if (auto skt = get_connection(connection_id)) {
                if (skt->receive_fin(buffer_size == 1 && buffer[0] == fin_write_only)) {
                    skt->active(now);
                } else {
                    skt->close();
                }
            }
        }
        elif(cmd == cmd_syn) {
            std::shared_ptr<vmux_skt> sk;
            bool successed = process_rx_connecting(sk, connection_id, (char*)buffer, buffer_size);

            if (NULLPTR != sk) {
                if (successed) {
                    sk->active(now);
                }
                else {
                    sk->close();
                }
            }
        }
        elif(cmd == cmd_syn_ok) {
            intent_opening_frames_.erase(connection_id);
            vmux_skt_ptr skt = get_connection(connection_id);
            if (NULLPTR != skt) {
                if (intent_routing_enabled() && ordering_mode_ == ordering_flow_v2 &&
                    skt->is_connected() && buffer_size > 0 && buffer[0] == 'A') {
                    return true; // The same connection opening was retried on a surviving carrier.
                }
                bool successed = false;
                if (buffer_size > 0) {
                    const Byte err = static_cast<Byte>(*buffer);
                    successed = skt->connect_ok(err == 'A');
                }

                if (successed) {
                    skt->active(now);
                }
                else {
                    skt->close();
                }
            }
        }
        elif(cmd == cmd_acceleration) {
            vmux_skt_ptr skt = get_connection(connection_id);
            if (NULLPTR != skt) {
                bool acceleration = true;
                if (buffer_size > 0) {
                    acceleration = static_cast<Byte>(*buffer) != FALSE;
                }
                // Versioned peers append a 4-byte flow-control version so a stale
                // XON/XOFF that overtook a newer one is ignored.
                bool has_version = false;
                uint32_t version = 0;
                if (buffer_size >= 1 + (int)sizeof(uint32_t)) {
                    memcpy(&version, buffer + 1, sizeof(version));
                    version = ntohl(version);
                    has_version = true;
                }

                if (skt->tx_acceleration(acceleration, has_version, version)) {
                    skt->active(now);
                }
                else {
                    skt->close();
                }
            }
        }
        elif(cmd == cmd_keep_alived) {
            active(now);
        }
        elif(cmd == cmd_mux_mode_set) {
            packet_input_mux_mode_set(buffer, buffer_size);
            active(now);
        }
        elif(cmd == cmd_ack) {
            packet_input_ack(buffer, buffer_size, now);
            active(now);
        }
        elif(cmd == cmd_fec) {
            packet_input_fec(NULLPTR, buffer, buffer_size, now);
            active(now);
        }
        else {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolPacketActionInvalid);
            return false;
        }

        return true;
    }

    /**
     * @brief Handles a debug-only cmd_mux_mode_set control frame.
     *
     * Applies the requested scheduler mode only when remote control is enabled
     * locally (non-empty mux.debug.key) and the key carried in the frame matches
     * exactly. Mismatches are logged and ignored; the session is never closed,
     * so a malformed/forged frame cannot disrupt traffic.
     */
    void vmux_net::packet_input_mux_mode_set(const Byte* buffer, int buffer_size) noexcept {
        if (NULLPTR == AppConfiguration) {
            return;
        }

        const ppp::string& debug_key = AppConfiguration->mux.debug.key;
        if (debug_key.empty()) {
            // Default-off: no debug key means remote scheduler control is disabled.
            if ((++mux_mode_set_reject_streak_ % 32) == 1) {
                ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
                    "mux-mode-set rejected: remote control disabled (no debug key)");
            }
            ppp::telemetry::Count("mux.debug.mode_set.reject", 1);
            return;
        }

        if (NULLPTR == buffer || buffer_size < 2) {
            ppp::telemetry::Count("mux.debug.mode_set.reject", 1);
            ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux", "mux-mode-set rejected: malformed control frame");
            return;
        }

        Byte requested = buffer[0];
        int key_length = static_cast<int>(buffer[1]);
        if (key_length <= 0 || (2 + key_length) > buffer_size) {
            ppp::telemetry::Count("mux.debug.mode_set.reject", 1);
            ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux", "mux-mode-set rejected: invalid key length");
            return;
        }

        bool key_matched =
            key_length == static_cast<int>(debug_key.size()) &&
            CRYPTO_memcmp(buffer + 2, debug_key.data(), key_length) == 0;
        if (!key_matched) {
            ppp::telemetry::Count("mux.debug.mode_set.reject", 1);
            if ((++mux_mode_set_reject_streak_ % 16) == 1) {
                ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux", "mux-mode-set rejected: debug key mismatch");
            }
            return;
        }

        // Rate limit accepted changes (and successful-auth attempts) to reduce abuse.
        const uint64_t now = now_tick();
        const uint64_t min_interval_ms = 1000;
        if (mux_mode_set_last_accept_ != 0 && (now - mux_mode_set_last_accept_) < min_interval_ms) {
            ppp::telemetry::Count("mux.debug.mode_set.rate_limited", 1);
            return;
        }

        mux_mode mode = parse_mode_byte(requested);
        // set_mode only switches the transmit scheduler (compat/flow/balance/stripe).
        // Receiver ordering remains session-immutable after establishment (see set_ordering_mode).
        ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
            "mux-mode-set accepted from peer: scheduler=%s (ordering unchanged=%s)",
            mode_name(mode),
            ordering_mode_ == ordering_flow_v2 ? "flow_v2" : "compat");

        // Apply to the live session and record a lock-free runtime override on the
        // shared runtime config so the change survives mux session rebuilds (link
        // flap, idle/heartbeat timeout). The exchanger reconstructs a vmux_net via
        // AppConfiguration->GetEffectiveMuxMode() on reconnect; without this the
        // pushed mode would be lost and silently revert to the configured value.
        AppConfiguration->SetMuxModeRuntimeOverride(static_cast<int>(mode));
        set_mode(mode);
        mux_mode_set_last_accept_ = now;
        mux_mode_set_reject_streak_ = 0;
        ppp::telemetry::Count("mux.debug.mode_set.accept", 1);
    }

    void vmux_net::rx_pause_link(const vmux_linklayer_ptr& linklayer) noexcept {
        // A retiring carrier may still hold the missing frame and must keep
        // draining its receive side; only pause carriers that stay scheduled.
        if (NULLPTR == linklayer || !linklayer->schedulable() ||
            linklayer->rx_paused_.load(std::memory_order_acquire)) {
            return;
        }

        linklayer->rx_paused_since_.store(now_tick(), std::memory_order_relaxed);
        linklayer->rx_paused_.store(true, std::memory_order_release);
        // A lease-expired pause clears only the flag; keep one entry per link.
        if (std::find(rx_paused_links_.begin(), rx_paused_links_.end(), linklayer) == rx_paused_links_.end()) {
            rx_paused_links_.emplace_back(linklayer);
        }
        ppp::telemetry::Count("mux.rx.backpressure.pause", 1);
        if (scheduler_trace_writer().enabled) {
            trace_note_rx_backpressure(*linklayer);
        }
        rx_release_if_stalled();
    }

    void vmux_net::rx_forget_paused_link(const vmux_linklayer_ptr& linklayer) noexcept {
        if (NULLPTR == linklayer) {
            return;
        }

        linklayer->rx_paused_.store(false, std::memory_order_release);
        rx_paused_links_.erase(
            std::remove(rx_paused_links_.begin(), rx_paused_links_.end(), linklayer),
            rx_paused_links_.end());
        rx_release_if_stalled();
    }

    void vmux_net::rx_release_if_stalled() noexcept {
        if (rx_paused_links_.empty()) {
            return;
        }

        // The missing frame must still arrive on some carrier: never leave every
        // live carrier paused. Releasing all of them lets the buffer grow toward
        // the hard bound instead of deadlocking the session.
        size_t paused_live = 0;
        for (const vmux_linklayer_ptr& link : rx_paused_links_) {
            if (NULLPTR != link && link->schedulable() && link->rx_paused_.load(std::memory_order_acquire)) {
                ++paused_live;
            }
        }
        if (ppp::app::mux::MuxPauseWouldStallAll(paused_live, count_live_carriers())) {
            ppp::telemetry::Count("mux.rx.backpressure.release_all", 1);
            rx_resume_all_links();
        }
    }

    void vmux_net::rx_resume_all_links() noexcept {
        if (rx_paused_links_.empty()) {
            return;
        }

        for (const vmux_linklayer_ptr& link : rx_paused_links_) {
            if (NULLPTR != link) {
                link->rx_paused_.store(false, std::memory_order_release);
            }
        }
        rx_paused_links_.clear();
        ppp::telemetry::Count("mux.rx.backpressure.resume", 1);
    }

    void vmux_net::rx_maybe_resume() noexcept {
        if (rx_paused_links_.empty()) {
            rx_pressure_flows_.clear();
            return;
        }

        for (auto it = rx_pressure_flows_.begin(); it != rx_pressure_flows_.end();) {
            auto fit = flows_.find(*it);
            if (fit == flows_.end() ||
                ppp::app::mux::MuxReorderShouldResume(fit->second.flow_reorder_.buffered_bytes(), flow_reorder_cap_bytes_)) {
                it = rx_pressure_flows_.erase(it);
            }
            else {
                ++it;
            }
        }

        if (rx_pressure_flows_.empty() &&
            ppp::app::mux::MuxReorderShouldResume(session_reorder_bytes_, session_reorder_cap_bytes_)) {
            rx_resume_all_links();
        }
    }

    /**
     * @brief Fails one logical flow without delivering past an unrecovered gap.
     * @details Erases receive/send flow state and closes the logical socket if
     *          present. Buffered out-of-order payloads for this cid are dropped
     *          with the flow context (no force-advance delivery).
     */

    void vmux_net::note_flow_buffered(size_t bytes) noexcept {
        flow_aggregate_bytes_ += bytes;
        session_reorder_bytes_ += bytes;
        ++reorder_events_;
    }

    void vmux_net::note_flow_unbuffered(size_t bytes) noexcept {
        if (bytes >= flow_aggregate_bytes_) {
            flow_aggregate_bytes_ = 0;
        }
        else {
            flow_aggregate_bytes_ -= bytes;
        }
        if (bytes >= session_reorder_bytes_) {
            session_reorder_bytes_ = 0;
        }
        else {
            session_reorder_bytes_ -= bytes;
        }
    }

    vmux_net::flow_rx_context* vmux_net::try_get_or_create_flow(uint32_t connection_id) noexcept {
        const bool already_tracked = flows_.find(connection_id) != flows_.end();
        const bool socket_exists = NULLPTR != get_connection(connection_id);
        const size_t cap = flow_context_cap_ > 0
            ? flow_context_cap_
            : (size_t)PPP_MUX_FLOW_MAX_CONTEXTS;
        const ppp::app::mux::FlowContextAdmission decision =
            ppp::app::mux::AdmitFlowContext(
                connection_id,
                already_tracked,
                socket_exists,
                flows_.size(),
                cap);

        switch (decision) {
        case ppp::app::mux::FlowContextAdmission::AllowExisting:
            return &flows_[connection_id];
        case ppp::app::mux::FlowContextAdmission::AllowCreate:
            return &flows_[connection_id];
        case ppp::app::mux::FlowContextAdmission::RejectUnknown:
            ppp::telemetry::Count("mux.rx.flow.unknown_cid", 1);
            ppp::telemetry::Count("mux.rx.unknown_cid", 1);
            return NULLPTR;
        case ppp::app::mux::FlowContextAdmission::RejectCap:
            ppp::telemetry::Count("mux.rx.flow.context_cap", 1);
            return NULLPTR;
        case ppp::app::mux::FlowContextAdmission::RejectZero:
        default:
            return NULLPTR;
        }
    }

    void vmux_net::fail_flow(uint32_t connection_id, const char* reason) noexcept {
        const char* why = (reason != NULLPTR && reason[0] != '\0') ? reason : "unspecified";
        ppp::telemetry::Count("mux.rx.flow.reset", 1);
        ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
            "flow reset cid=%u reason=%s", (unsigned)connection_id, why);

        auto fit = flows_.find(connection_id);
        if (fit != flows_.end()) {
            note_flow_unbuffered(fit->second.flow_reorder_.buffered_bytes());
            flows_.erase(fit);
        }
        tx_flow_seq_.erase(connection_id);
        release_flow_reliability_state(connection_id);

        // Drop any remaining TX frames for this cid so a failed flow does not
        // keep consuming DRR send budget or retain memory in the TX queue.
        auto txit = tx_flows_.find(connection_id);
        if (txit != tx_flows_.end()) {
            if (tx_data_frames_ >= txit->second.queue.size()) {
                tx_data_frames_ -= txit->second.queue.size();
            }
            else {
                tx_data_frames_ = 0;
            }
            if (txit->second.bytes <= tx_data_bytes_total_) {
                tx_data_bytes_total_ -= txit->second.bytes;
            }
            else {
                tx_data_bytes_total_ = 0;
            }
            if (txit->second.active) {
                active_tx_flows_.remove(connection_id);
            }
            if (txit->second.rtx_parked) {
                tx_rtx_parked_flows_.remove(connection_id);
            }
            tx_flows_.erase(txit);
        }

        // A reset flow no longer holds reorder bytes; release backpressure if
        // that brought the budgets back under their resume mark.
        rx_pressure_flows_.erase(connection_id);
        rx_maybe_resume();

        vmux_skt_ptr skt = get_connection(connection_id);
        if (NULLPTR != skt) {
            // Suppress outbound FIN: post(cmd_fin) would fail without TX credit and
            // post()'s failure path calls close_exec(), killing the whole session
            // for a single-flow reset. Local close is enough for gap/overflow fail.
            skt->status_.fin_ = true;
            skt->close();
        }
    }

    /**
     * @brief Per-flow (flow v2) receive path: independent per-connection DSN delivery.
     * @details Control frames bypass the DSN gate entirely. Per-flow data frames
     *          (push/fin) are delivered in per-connection DSN order, buffering
     *          future frames in a bounded reorder buffer. One slow link cannot
     *          head-of-line block other connections because each connection_id
     *          has its own flow_rx_next_ and reorder buffer.
     */
    bool vmux_net::packet_input_flow(const vmux_linklayer_ptr& linklayer, vmux_hdr* h, int length, uint64_t now, const std::shared_ptr<Byte>& owner, bool fec_recovered) noexcept {
        if (base_.disposed_.load(std::memory_order_acquire)) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionDisposed);
            return false;
        }

        ppp::telemetry::Count("mux.link.recv", 1);

        Byte cmd = h->cmd;

        // Control frames are not gated by any per-flow DSN; handle them inline.
        if (is_session_control(cmd) || is_connection_control(cmd) || is_reliability_control(cmd)) {
            if (linklayer && cmd == cmd_keep_alived && length == static_cast<int>(sizeof(vmux_hdr)) + 2) {
                const auto* payload = reinterpret_cast<const Byte*>(h + 1);
                if (payload[0] == IntentPathProbeMarker) {
                    const uint32_t token = ntohl(h->connection_id);
                    if (payload[1] == IntentPathProbeRequest) {
                        send_intent_path_probe(linklayer, IntentPathProbeResponse, token);
                    }
                    else if (payload[1] == IntentPathProbeResponse) {
                        linklayer->intent_path_probe_.Acknowledge(token, now);
                    }
                }
            }
            if (!packet_input(cmd, (Byte*)h, length, now, owner)) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolPacketActionInvalid);
                return false;
            }

            active(now);
            linklayer_update(linklayer);
            return true;
        }

        // Any other non per-flow-data command is invalid on the flow path.
        if (!is_per_flow_data(cmd)) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolPacketActionInvalid);
            return false;
        }

        uint32_t cid = ntohl(h->connection_id);
        uint32_t seq = ntohl(h->seq);
        auto opening = intent_opening_frames_.find(cid);
        if (opening != intent_opening_frames_.end() &&
            reinterpret_cast<const vmux_hdr*>(opening->second.packet.buffer.get())->cmd == cmd_syn_ok) {
            intent_opening_frames_.erase(opening);
        }

        flow_rx_context* fxp = try_get_or_create_flow(cid);
        if (NULLPTR == fxp) {
            active(now);
            linklayer_update(linklayer);
            return true;
        }
        flow_rx_context& fx = *fxp;
        if (!fx.primed_) {
            fx.primed_ = true;
            fx.flow_rx_next_ = 1; // Sender DSNs are session-local and start at one.
        }

        const auto source = linklayer && !fec_recovered ? linklayer->path_runtime_counters_ : nullptr;
        if (seq == fx.flow_rx_next_) {
            // In-order: deliver immediately, then replay any contiguous buffered frames.
            if (!packet_input(cmd, (Byte*)h, length, now, owner, source)) {
                return false;
            }
            fx.flow_rx_next_++;

            for (;;) {
                rx_ordered_packet pk;
                if (fx.flow_reorder_.Take(fx.flow_rx_next_, pk)) {
                    note_flow_unbuffered((size_t)pk.length); // keeps the session and aggregate counters in step
                    vmux_hdr* ph = (vmux_hdr*)pk.buffer.get();
                    Byte pcmd = ph->cmd;
                    if (!packet_input(pcmd, (Byte*)ph, pk.length, now, pk.buffer, pk.source)) {
                        return false;
                    }
                    fx.flow_rx_next_++;
                }
                else {
                    break;
                }
            }

            // In-order progress: restart this flow's gap clock (see compat path).
            fx.oldest_buffered_tick_ = ppp::app::mux::MuxGapClockAfterAdvance(now, !fx.flow_reorder_.empty());

            // Retain both DSN spaces until socket release. An ordered write-FIN
            // does not end reverse PUSH, RTX, or flow control.
            rx_maybe_resume();
            active(now);
            linklayer_update(linklayer);
            return true;
        }
        elif(packet_less<uint32_t>::after(seq, fx.flow_rx_next_)) {
            // Future frame: buffer it (bounded by bytes), unless it is itself too large.
            if (length < (int)sizeof(vmux_hdr) || length > (int)(sizeof(vmux_hdr) + max_buffers_size)) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolFrameInvalid);
                return false;
            }

            // A single frame larger than the whole per-connection cap can never be
            // buffered; fail the flow rather than skip-and-deliver past a hole.
            if ((size_t)length > flow_reorder_cap_bytes_) {
                fail_flow(cid, "frame_oversize");
                active(now);
                linklayer_update(linklayer);
                return true;
            }

            // Per-flow and session caps are soft: above them keep buffering but
            // pause the delivering carrier (backpressure). Only past the hard
            // bound fail the flow instead of force-advancing past a gap.
            const size_t flow_hard_cap = ppp::app::mux::MuxReorderHardCap(
                flow_reorder_cap_bytes_, (size_t)PPP_MUX_REORDER_HARD_FACTOR);
            const ppp::app::mux::MuxReorderAdmit flow_admit = ppp::app::mux::AdmitReorderBytes(
                fx.flow_reorder_.buffered_bytes(), (size_t)length,
                flow_reorder_cap_bytes_, (size_t)PPP_MUX_REORDER_HARD_FACTOR);
            if (flow_admit == ppp::app::mux::MuxReorderAdmit::Reject && !fx.flow_reorder_.empty()) {
                fail_flow(cid, "reorder_overflow");
                active(now);
                linklayer_update(linklayer);
                return true;
            }
            const size_t session_hard_cap = ppp::app::mux::MuxReorderHardCap(
                session_reorder_cap_bytes_, (size_t)PPP_MUX_REORDER_HARD_FACTOR);
            const ppp::app::mux::MuxReorderAdmit session_admit = ppp::app::mux::AdmitReorderBytes(
                session_reorder_bytes_, (size_t)length,
                session_reorder_cap_bytes_, (size_t)PPP_MUX_REORDER_HARD_FACTOR);
            if (session_admit == ppp::app::mux::MuxReorderAdmit::Reject) {
                if (!fx.flow_reorder_.empty()) {
                    fail_flow(cid, "session_reorder_overflow");
                    active(now);
                    linklayer_update(linklayer);
                    return true;
                }
                uint32_t victim = 0;
                uint64_t oldest = 0;
                for (auto it = flows_.begin(); it != flows_.end(); ++it) {
                    if (it->second.flow_reorder_.empty() || it->second.oldest_buffered_tick_ == 0) {
                        continue;
                    }
                    if (oldest == 0 || it->second.oldest_buffered_tick_ < oldest) {
                        oldest = it->second.oldest_buffered_tick_;
                        victim = it->first;
                    }
                }
                if (victim != 0) {
                    fail_flow(victim, "session_reorder_overflow");
                }
                if (session_reorder_bytes_ + (size_t)length > session_hard_cap) {
                    ppp::telemetry::Count("mux.rx.session_reorder.drop", 1);
                    active(now);
                    linklayer_update(linklayer);
                    return true;
                }
            }

            std::shared_ptr<Byte> buf;
            if (NULLPTR != owner) {
                buf = ppp::wrap_shared_pointer(reinterpret_cast<ppp::Byte*>(h), owner);
            } else {
                buf = make_byte_array(length);
                if (NULLPTR != buf) {
                    memcpy(buf.get(), h, length);
                }
            }
            if (NULLPTR == buf) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetReorderPacketBufferAllocFailed);
                return false;
            }

            rx_ordered_packet packet = {{std::move(buf), length}, source};

            const size_t entry_cap = std::max<size_t>(
                1, flow_hard_cap / sizeof(vmux_hdr));
            if (fx.flow_reorder_.size() >= entry_cap) {
                fail_flow(cid, "reorder_overflow");
                active(now);
                linklayer_update(linklayer);
                return true;
            }

            bool inserted = fx.flow_reorder_.TryInsert(
                seq,
                fx.flow_rx_next_,
                packet,
                (size_t)length,
                flow_hard_cap,
                entry_cap);
            if (inserted) {
                note_flow_buffered((size_t)length);
                fx.oldest_buffered_tick_ = ppp::app::mux::MuxGapClockOnBuffered(fx.oldest_buffered_tick_, now);
                if (flow_admit == ppp::app::mux::MuxReorderAdmit::AcceptAndPause) {
                    rx_pressure_flows_.emplace(cid);
                    rx_pause_link(linklayer);
                }
                else if (session_admit == ppp::app::mux::MuxReorderAdmit::AcceptAndPause) {
                    rx_pause_link(linklayer);
                }
            }
            // Duplicate future DSN (or other non-overflow reject): keep original, drop duplicate.

            active(now);
            linklayer_update(linklayer);
            return true;
        }
        else {
            // Stale/duplicate (before flow_rx_next_): drop, not an error.
            active(now);
            return true;
        }
    }

    /**
     * @brief Periodically fails per-flow contexts whose gap has timed out.
     * @details Runs only under flow-v2. An unrecovered gap past the timeout
     *          resets that logical flow; it must not skip and deliver past a hole.
     */
    void vmux_net::flow_evict_expired(uint64_t now) noexcept {
        if (ordering_mode_ != ordering_flow_v2) {
            return;
        }

        // With reliability negotiated, retransmission fills gaps first; use the
        // wider reliability gap timeout as the final backstop.
        const uint64_t gap_timeout = (reliability_on_ && reliability_gap_timeout_ms_ > 0)
            ? reliability_gap_timeout_ms_
            : flow_reorder_timeout_;

        // Collect first: fail_flow erases from flows_ while we iterate.
        ppp::vector<uint32_t> expired;
        for (vmux_flow_map::iterator it = flows_.begin(); it != flows_.end(); ++it) {
            flow_rx_context& fx = it->second;
            if (!fx.flow_reorder_.empty() && fx.oldest_buffered_tick_ != 0 &&
                (now - fx.oldest_buffered_tick_) > gap_timeout) {
                expired.emplace_back(it->first);
            }
        }

        for (uint32_t cid : expired) {
            fail_flow(cid, "gap_timeout");
        }
    }

    /**
     * @brief Fails the session when a compat global reorder gap has timed out.
     * @details Under ordering_compat the single rx_queue_ can hold future frames
     *          forever if a missing seq never arrives. That is an unrecovered gap:
     *          rebuild the session instead of stalling silently.
     */
    void vmux_net::compat_evict_expired(uint64_t now) noexcept {
        if (ordering_mode_ != ordering_compat) {
            return;
        }
        if (rx_queue_.empty() || rx_gap_oldest_tick_ == 0) {
            return;
        }

        uint64_t timeout = flow_reorder_timeout_;
        if (timeout == 0) {
            timeout = (uint64_t)PPP_MUX_FLOW_REORDER_TIMEOUT;
        }
        if (reliability_on_ && reliability_gap_timeout_ms_ > 0) {
            timeout = reliability_gap_timeout_ms_;
        }
        if ((now - rx_gap_oldest_tick_) <= timeout) {
            return;
        }

        ppp::telemetry::Count("mux.rx.compat.gap_timeout", 1);
        ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
            "compat gap timeout: buffered=%d age_ms=%llu, rebuilding session",
            (int)rx_queue_.size(),
            (unsigned long long)(now - rx_gap_oldest_tick_));
        close_exec();
    }
    
    // ------------------------------------------------------------------------
    // Reliability sub-protocol (negotiated): ACK feedback, retransmission, FEC.
    // All state below is strand-affine unless noted otherwise.
    // ------------------------------------------------------------------------

    void vmux_net::latch_reliability(bool agreed_reliability, bool agreed_fec) noexcept {
        reliability_on_ = agreed_reliability;
        fec_on_ = agreed_fec && agreed_reliability;

        if (NULLPTR != AppConfiguration) {
            const int rtx_bytes = AppConfiguration->mux.reliability.rtx.bytes;
            const int rtx_attempts = AppConfiguration->mux.reliability.rtx.max_attempts;
            const int ack_delay = AppConfiguration->mux.reliability.ack.delay;
            const int gap_timeout = AppConfiguration->mux.reliability.gap.timeout;
            rtx_cap_bytes_ = (rtx_bytes > 0) ? (size_t)rtx_bytes : (size_t)PPP_MUX_RELIABILITY_RTX_BYTES;
            const int rtx_flow_bytes = AppConfiguration->mux.reliability.rtx.flow_bytes;
            rtx_flow_window_bytes_ = (rtx_flow_bytes > 0) ? (size_t)rtx_flow_bytes : (size_t)PPP_MUX_RTX_FLOW_MAX_BYTES;
            if (rtx_flow_window_bytes_ > rtx_cap_bytes_) {
                rtx_flow_window_bytes_ = rtx_cap_bytes_;
            }
            rtx_max_attempts_ = (rtx_attempts > 0) ? (uint32_t)rtx_attempts : (uint32_t)PPP_MUX_RELIABILITY_RTX_MAX_ATTEMPTS;
            ack_delay_ms_ = (ack_delay > 0) ? (uint64_t)ack_delay : (uint64_t)PPP_MUX_RELIABILITY_ACK_DELAY;
            // Intent routing uses the latency path for feedback as well as
            // request payloads. Avoid adding the normal delayed-ACK interval
            // to every short interactive exchange when that policy is active.
            if (intent_routing_enabled()) {
                ack_delay_ms_ = std::min<std::uint64_t>(ack_delay_ms_, 2);
            }
            reliability_gap_timeout_ms_ = (gap_timeout > 0) ? (uint64_t)gap_timeout : (uint64_t)PPP_MUX_RELIABILITY_GAP_TIMEOUT;

            // Carriers are reliable byte streams: a frame is only lost when its
            // carrier dies, which reinject_link_inflight() covers. PTO and
            // dup-ACK fast retransmit mostly re-send frames that are merely
            // queued behind a slow carrier, so both are opt-in.
            rtx_pto_on_ = AppConfiguration->mux.reliability.rtx.pto;
            rtx_fast_on_ = AppConfiguration->mux.reliability.rtx.fast;

            const int fec_group = AppConfiguration->mux.fec.group;
            const int fec_flush = AppConfiguration->mux.fec.flush;
            fec_group_k_ = (fec_group > 0) ? fec_group : PPP_MUX_FEC_GROUP;
            fec_flush_ms_ = (fec_flush > 0) ? (uint64_t)fec_flush : (uint64_t)PPP_MUX_FEC_FLUSH;
        }

        {
            std::lock_guard<std::mutex> scope(runtime_state_mutex_);
            runtime_state_.reliability = reliability_on_;
            runtime_state_.fec = fec_on_;
            publish_runtime_snapshot_locked();
        }

        ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux", "reliability=%d fec=%d pto=%d fast=%d",
            reliability_on_ ? 1 : 0,
            fec_on_ ? 1 : 0,
            (reliability_on_ && rtx_pto_on_) ? 1 : 0,
            (reliability_on_ && rtx_fast_on_) ? 1 : 0);
        start_reliability_timer();
    }

    void vmux_net::note_inbound_reliability_frame(const vmux_linklayer_ptr& linklayer, const std::shared_ptr<Byte>& frame, vmux_hdr* h, int length, uint64_t now) noexcept {
        if (!reliability_on_ || NULLPTR == h) {
            return;
        }

        const Byte cmd = h->cmd;
        if (is_reliability_control(cmd)) {
            return; // ACK/FEC frames are never themselves ACKed or protected.
        }

        const uint32_t cid = ntohl(h->connection_id);
        const uint32_t seq = ntohl(h->seq);
        if (ordering_mode_ == ordering_flow_v2) {
            // flow_v2: only per-flow data frames participate in the DSN spaces.
            if (!is_per_flow_data(cmd)) {
                return;
            }
            note_ack_pending(cid, seq, now);
            fec_note_received(linklayer, cid, seq, frame, length, now);
        }
        else {
            // compat: every sequenced frame shares the global space (cid 0).
            note_ack_pending(0, seq, now);
            if (is_per_flow_data(cmd)) {
                fec_note_received(linklayer, cid, seq, frame, length, now);
            }
        }
    }

    void vmux_net::note_ack_pending(uint32_t connection_id, uint32_t seq, uint64_t now) noexcept {
        if (!reliability_on_) {
            return;
        }

        const uint32_t key_cid = (ordering_mode_ == ordering_compat) ? 0u : connection_id;
        if (ack_trackers_.size() >= (size_t)PPP_MUX_FLOW_MAX_CONTEXTS && ack_trackers_.find(key_cid) == ack_trackers_.end()) {
            ppp::telemetry::Count("mux.ack.tracker.cap", 1);
            return; // Bound tracker growth from forged connection ids.
        }
        ppp::app::mux::MuxAckTracker& tracker = ack_trackers_[key_cid];
        tracker.Add(seq, (size_t)PPP_MUX_ACK_MAX_RANGES);
        if (ack_pending_count_ == 0) {
            ack_first_pending_tick_ = now;
        }
        ack_pending_count_++;
        ack_pending_cids_.emplace(key_cid);
        maybe_send_ack(now, false);
    }

    void vmux_net::maybe_send_ack(uint64_t now, bool force) noexcept {
        const bool has_pending_blocks = !ack_pending_cids_.empty();
        if (!reliability_on_ || !base_.established_ ||
            (ack_pending_count_ == 0 && !has_pending_blocks)) {
            return;
        }
        if (!force && !ack_partial_pending_ && ack_pending_count_ < 2 &&
            (now - ack_first_pending_tick_) < ack_delay_ms_) {
            return;
        }

        // ACK coalescing is bypassed while a previous frame could only carry a
        // subset of the pending flow sequence spaces. Otherwise an unordered-map
        // iteration would repeatedly ACK the same first block set and starve the
        // remaining flows until the retransmit window filled.
        if (!force && !ack_partial_pending_ && ack_last_sent_tick_ > 0 &&
            (now - ack_last_sent_tick_) < std::max<uint64_t>(ack_delay_ms_ / 2, 1)) {
            return;
        }

        std::vector<uint32_t> pending_ids;
        pending_ids.reserve(ack_pending_cids_.size());
        for (const uint32_t cid : ack_pending_cids_) {
            const auto found = ack_trackers_.find(cid);
            if (found != ack_trackers_.end() && !found->second.empty()) {
                pending_ids.emplace_back(cid);
            }
        }
        if (pending_ids.empty()) {
            ack_pending_cids_.clear();
            ack_pending_count_ = 0;
            ack_partial_pending_ = false;
            ack_first_pending_tick_ = 0;
            return;
        }
        std::sort(pending_ids.begin(), pending_ids.end());

        std::vector<uint32_t> selected_ids;
        ppp::app::mux::SelectMuxAckBlockIds(pending_ids, ack_block_cursor_,
            (size_t)PPP_MUX_ACK_MAX_BLOCKS, selected_ids);
        ppp::app::mux::MuxAckBlock blocks[PPP_MUX_ACK_MAX_BLOCKS];
        size_t block_count = 0;
        for (const uint32_t cid : selected_ids) {
            const auto found = ack_trackers_.find(cid);
            if (found == ack_trackers_.end() || found->second.empty()) continue;
            ppp::app::mux::MuxAckBlock& block = blocks[block_count++];
            block.connection_id = cid;
            block.largest = found->second.largest();
            block.ranges = found->second.ranges();
        }
        if (block_count == 0) {
            ack_pending_cids_.clear();
            ack_pending_count_ = 0;
            ack_partial_pending_ = false;
            ack_first_pending_tick_ = 0;
            return;
        }

        // Return the ACK through the normal control-queue scheduler. ACK affinity
        // (pinning feedback to the carrier that delivered the newest covered
        // frame) was tried and removed: on a full-duplex carrier the reverse
        // direction may itself be the busy one, so a pinned ACK can wait behind
        // bulk data and delay window release. Feedback is a control signal and
        // must stay free to use any live carrier with credit.
        const size_t payload_cap = ppp::app::mux::MuxAckFrameMaxSize(
            (size_t)PPP_MUX_ACK_MAX_BLOCKS, (size_t)PPP_MUX_ACK_MAX_RANGES);
        std::shared_ptr<Byte> packet = make_byte_array((int)(sizeof(vmux_hdr) + payload_cap));
        if (NULLPTR == packet) return;

        const size_t payload_len = ppp::app::mux::EncodeMuxAckFrame(
            blocks, block_count, packet.get() + sizeof(vmux_hdr), payload_cap,
            (size_t)PPP_MUX_ACK_MAX_RANGES);
        if (payload_len == 0) return;

        vmux_hdr* h = (vmux_hdr*)packet.get();
        h->cmd = cmd_ack;
        h->connection_id = htonl(0);

        // Best-effort: ACK frames are periodic and cumulative, so a queueing
        // failure must not tear the session down (post() would close_exec).
        PostInternalAsynchronousCallback null_ac;
        ack_last_sent_tick_ = now;
        if (!post_internal(packet, (int)(sizeof(vmux_hdr) + payload_len), false, null_ac)) return;

        for (const uint32_t cid : selected_ids) ack_pending_cids_.erase(cid);
        ack_block_cursor_ = selected_ids.back();
        ack_partial_pending_ = !ack_pending_cids_.empty();
        ack_pending_count_ = 0;
        ack_first_pending_tick_ = 0;
        ppp::telemetry::Count("mux.ack.send", 1);
    }

    void vmux_net::note_link_rtt(vmux_linklayer& link, uint64_t sample, uint64_t now) noexcept {
        if (sample == 0) return;
        link.last_rtt_sample_ms_ = now;
        if (!link.has_rtt_sample_) {
            // The first ACK RTT is not evidence of the path's minimum: it is
            // commonly inflated by startup and by reverse-path ACK queueing
            // (upload path1 has been observed at ~374 ms against a 90 ms
            // carrier). Seed SRTT/RTTVAR only and leave min_rtt unknown (0) so
            // the BDP/write-window consumers keep their conservative startup
            // floor until a later sample confirms an upper bound. This never
            // changes the SRTT/ACK protocol; it only refuses to let one
            // unreplicated sample define the floor.
            link.srtt_ms_ = sample;
            link.rttvar_ms_ = sample / 2;
            link.has_rtt_sample_ = true;
            link.last_pressure_sample_ms_ = now;
            return;
        }

        // While min_rtt is still unknown exactly one sample has been seen and
        // it is still the whole SRTT; keep it as the candidate floor.
        const auto previous_srtt = link.srtt_ms_;
        const auto variation = link.srtt_ms_ > sample ? link.srtt_ms_ - sample : sample - link.srtt_ms_;
        link.rttvar_ms_ = (link.rttvar_ms_ * 3 + variation) / 4;
        link.srtt_ms_ = (link.srtt_ms_ * 7 + sample) / 8;
        if (link.min_rtt_ms_ == 0) {
            // Second sample: the candidate minimum of the two readings seen so
            // far. Taking the min means the first, possibly queue-inflated,
            // sample cannot set a high floor, while the newer reading cannot
            // raise it either. This is not the first sample "as baseline": the
            // baseline is the smaller of two independent samples.
            link.min_rtt_ms_ = std::min(sample, previous_srtt);
        } else if (sample < link.min_rtt_ms_) {
            // Controlled fast convergence toward a lower candidate minimum:
            // move at most a quarter of the current floor per sample. A single
            // transiently low sample (scheduling artifact, timer coalescing)
            // can no longer pin the floor near zero, while a genuine lower
            // RTT is reached within a few confirming samples. BDP/credit
            // therefore track the confirmed minimum instead of staying pinned
            // to the first, possibly queue-inflated, reading.
            const auto step = std::max<uint64_t>(1, link.min_rtt_ms_ / 4);
            link.min_rtt_ms_ = std::max(sample, link.min_rtt_ms_ - step);
        }

        // One vote per time interval keeps ACK bursts from changing the weight.
        const auto baseline = link.min_rtt_ms_;
        if (baseline == 0) return; // floor still unconfirmed: leave weight alone
        const auto interval = std::max<uint64_t>(20, baseline / 2);
        if (now - link.last_pressure_sample_ms_ < interval) return;
        link.last_pressure_sample_ms_ = now;
        const auto queue_delay = link.srtt_ms_ - std::min(link.srtt_ms_, baseline);
        const bool recent_retry = link.last_retransmit_ms_ != 0 &&
            now - link.last_retransmit_ms_ <= std::max<uint64_t>(250, baseline * 2);
        if (recent_retry || queue_delay > std::max<uint64_t>(15, baseline / 3) ||
            link.rttvar_ms_ > std::max<uint64_t>(10, baseline / 4)) {
            link.pressure_streak_ = std::min(0, link.pressure_streak_) - 1;
            if (link.pressure_streak_ <= -2) {
                link.adaptive_weight_ = std::max(25u, link.adaptive_weight_ - 15);
                link.pressure_streak_ = 0;
            }
        } else if (queue_delay <= std::max<uint64_t>(5, baseline / 6) &&
            link.rttvar_ms_ <= std::max<uint64_t>(5, baseline / 8)) {
            link.pressure_streak_ = std::max(0, link.pressure_streak_) + 1;
            if (link.pressure_streak_ >= 3) {
                link.adaptive_weight_ = std::min(100u, link.adaptive_weight_ + 15);
                link.pressure_streak_ = 0;
            }
        } else {
            link.pressure_streak_ = 0;
        }
    }


    void vmux_net::packet_input_ack(Byte* buffer, int buffer_size, uint64_t now) noexcept {
        if (!reliability_on_) {
            ppp::telemetry::Count("mux.ack.unexpected", 1);
            return;
        }

        std::vector<ppp::app::mux::MuxAckBlock> blocks;
        if (!ppp::app::mux::DecodeMuxAckFrame((const uint8_t*)buffer, (size_t)std::max(0, buffer_size),
            (size_t)PPP_MUX_ACK_MAX_BLOCKS, (size_t)PPP_MUX_ACK_MAX_RANGES, blocks)) {
            // Malformed feedback: drop the frame, never kill the session.
            ppp::telemetry::Count("mux.ack.malformed", 1);
            return;
        }

        ppp::telemetry::Count("mux.ack.recv", 1);
        const size_t rtx_bytes_before = rtx_.bytes();
        std::unordered_map<uint16_t, ppp::app::mux::MuxAckDelivery> ack_deliveries;
        std::unordered_map<uint16_t, ppp::app::mux::MuxAckDelivery> block_deliveries;
        tx_completion_list acknowledged_completions;
        for (const ppp::app::mux::MuxAckBlock& block : blocks) {
            const uint32_t key_cid = (ordering_mode_ == ordering_compat) ? 0u : block.connection_id;
            std::vector<uint64_t> candidates;
            uint16_t ack_link_id = 0;
            // Heterogeneous RTT can create DSN gaps without loss.
            const uint64_t fast_time_threshold = srtt_ms_ != 0
                ? std::max<uint64_t>(srtt_ms_ / 4, 1) : 0;
            std::vector<uint64_t> acknowledged;
            const uint64_t sample = rtx_.Ack(key_cid, block.largest, block.ranges, now,
                (uint32_t)PPP_MUX_FAST_RETX_THRESHOLD, fast_time_threshold, candidates,
                &ack_link_id, nullptr, &block_deliveries, &pool_acked_payload_bytes_,
                ordering_mode_ == ordering_flow_v2 ? &acknowledged : nullptr);
            if (!acknowledged.empty()) {
                std::lock_guard<std::mutex> scope(tx_completion_mutex_);
                for (const auto key : acknowledged) {
                    auto retained = pending_payload_tx_completions_.extract(key);
                    if (!retained.empty()) {
                        pending_tx_completions_.erase(retained.mapped());
                        acknowledged_completions.emplace_back(std::move(retained.mapped()));
                    }
                }
            }
            if (auto skt = get_connection(key_cid); skt && skt->local_fin_sent_) {
                skt->maybe_finish_stream();
            }
            for (const auto& [link_id, delivery] : block_deliveries) {
                ack_deliveries[link_id].Merge(delivery);
            }
            if (sample > 0) {
                // Session-level SRTT (fallback for single-link / no link_id).
                srtt_ms_ = (srtt_ms_ == 0) ? sample : (srtt_ms_ * 7 + sample) / 8;

                // Per-link SRTT (QUIC-style EWMA) when the ACK can be
                // attributed to a specific carrier link.
                if (ack_link_id != 0 && block_deliveries.find(ack_link_id) == block_deliveries.end()) {
                    for (const auto& link : rx_links_) {
                        if (link && link->id_ == ack_link_id) {
                            note_link_rtt(*link, sample, now);
                            break;
                        }
                    }
                }
            }
            // The ACK still releases frames and samples RTT; dup-ACK holes are
            // only re-sent when fast retransmit is enabled (cross-carrier
            // reordering is not loss on reliable carriers).
            if (rtx_fast_on_) {
                for (uint64_t key : candidates) {
                    rtx_pending_.emplace_back(key);
                }
                if (!candidates.empty()) {
                    ppp::telemetry::Count("mux.rtx.fast", (int64_t)candidates.size());
                }
            }
        }

        for (const auto& link : rx_links_) {
            if (!link || !link->path_estimator_) continue;
            const auto found = ack_deliveries.find(link->id_);
            if (found == ack_deliveries.end()) continue;
            const auto& delivery = found->second;
            const auto& stamp = delivery.stamp;
            // One ACK block can release payload sent by several carriers.
            if (delivery.sent_tick <= now) note_link_rtt(*link, now - delivery.sent_tick, now);
            link->delivered_payload_bytes_ += delivery.bytes;
            link->delivered_tick_ = now;
            link->last_delivery_ack_ms_ = now;
            if (!stamp.valid || stamp.delivered_tick > now || delivery.sent_tick < stamp.first_sent_tick ||
                stamp.prior_delivered >= link->delivered_payload_bytes_) continue;
            link->first_delivery_send_tick_ = std::max(link->first_delivery_send_tick_, delivery.sent_tick);
            const auto interval_ms = std::max(delivery.sent_tick - stamp.first_sent_tick,
                now - stamp.delivered_tick);
            if (interval_ms == 0 || interval_ms > std::numeric_limits<uint64_t>::max() / 1000) continue;
            link->app_limited_ = stamp.is_app_limited;
            link->path_estimator_->OnSample({link->delivered_payload_bytes_ - stamp.prior_delivered,
                interval_ms * 1000, stamp.prior_delivered, link->delivered_payload_bytes_,
                stamp.is_app_limited}, now);
        }

        // The capacity estimate just moved: re-derive each carrier's pacing cap.
        // The policy is a no-op when the target did not change, so a steady
        // path does not repeat the syscall.
        for (const vmux_linklayer_ptr& link : rx_links_) {
            if (link) update_link_pacing(*link, now);
        }

        if (!rtx_pending_.empty()) {
            retransmit_pending(now);
        }

        // An ACK returns both session-window and adaptive carrier credit.
        if (rtx_.bytes() < rtx_bytes_before) {
            const bool unparked = drr_unpark_all();
            if (unparked) ppp::telemetry::Count("mux.tx.rtx_window.resume", 1);
            if (unparked || (tx_data_frames_ != 0 &&
                multipath_scheduler_policy_.load(std::memory_order_acquire) == 0)) {
                process_tx_all_packets();
            }
        }
        if (app_tx_idle_since_ms_ == 0 && tx_data_frames_ == 0 && rtx_.bytes() == 0) {
            app_tx_idle_since_ms_ = now;
        }
        if (!acknowledged_completions.empty()) {
            // Once the peer acknowledges the DSN, a pending local write on
            // the old carrier no longer needs to hold the origin read pump.
            ppp::telemetry::Count("mux.rtx.ack_write_recovered", acknowledged_completions.size());
            finish_tx_completions(acknowledged_completions, true);
        }
    }

    size_t vmux_net::rtx_charge_bytes(const std::shared_ptr<Byte>& packet, int packet_length) noexcept {
        if (!reliability_on_ || NULLPTR == packet || packet_length < (int)sizeof(vmux_hdr)) {
            return 0;
        }

        const vmux_hdr* h = (const vmux_hdr*)packet.get();
        const Byte cmd = h->cmd;
        if (is_reliability_control(cmd)) {
            return 0;
        }

        uint32_t key_cid = 0; // compat: one global sequence space.
        if (ordering_mode_ == ordering_flow_v2) {
            // flow_v2: control frames carry no DSN and are not tracked; their
            // loss is tolerated (connect timeout / periodic retry covers them).
            if (!is_per_flow_data(cmd)) {
                return 0;
            }
            key_cid = ntohl(h->connection_id);
        }

        const uint64_t key = ppp::app::mux::MuxRetransmitBuffer::Key(key_cid, ntohl(h->seq));
        if (NULLPTR != rtx_.Find(key)) {
            return 0; // A retransmission, not a first send.
        }
        return (size_t)packet_length;
    }

    bool vmux_net::tx_rtx_window_hold(tx_packet& packet) noexcept {
        // A frame whose compat sequence is still deferred is by definition a
        // first send (its header seq is a placeholder, so no lookup).
        const size_t charge = packet.assign_seq
            ? (reliability_on_ ? (size_t)packet.length : 0)
            : rtx_charge_bytes(packet.buffer, packet.length);
        if (charge == 0) {
            return false;
        }

        // The retransmit caps act as a send window before the frame leaves the
        // DRR queue: unacked bytes ~= BDP + carrier send buffers, which on fast,
        // high-RTT paths exceeds the cap without any loss. Holding the frame
        // keeps the session alive; the ACK that frees bytes resumes the flow.
        const bool per_flow = (ordering_mode_ == ordering_flow_v2);
        const uint32_t cid = peek_connection_id(packet.buffer, packet.length);
        const ppp::app::mux::MuxTxAdmit admit = ppp::app::mux::AdmitTxFrame(
            rtx_.bytes(),
            per_flow ? rtx_.CidBytes(cid) : 0,
            charge,
            rtx_cap_bytes_,
            per_flow ? rtx_flow_window_bytes_ : 0);
        if (admit == ppp::app::mux::MuxTxAdmit::Send) {
            return false;
        }

        ppp::telemetry::Count(admit == ppp::app::mux::MuxTxAdmit::HoldFlow
            ? "mux.tx.rtx_window.hold_flow" : "mux.tx.rtx_window.hold", 1);
        drr_park_front(std::move(packet));
        return true;
    }

    bool vmux_net::track_sent_frame(const std::shared_ptr<Byte>& packet, int packet_length, uint64_t now, uint16_t link_id, vmux_linklayer* link) noexcept {
        // Same predicate the send window charges against (0 = untracked or a
        // retransmission, not a first send).
        if (rtx_charge_bytes(packet, packet_length) == 0) {
            return false;
        }

        const vmux_hdr* h = (const vmux_hdr*)packet.get();
        const uint32_t cid = ntohl(h->connection_id);
        const uint32_t seq = ntohl(h->seq);
        const uint32_t key_cid = (ordering_mode_ == ordering_flow_v2) ? cid : 0u;

        // The caps below are enforced as a send window before frames leave the
        // DRR queue (tx_rtx_window_hold); only frames that bypass it can get
        // here past the window, so only the hard bound (cap * factor) is
        // terminal. Per-flow quota: one flow cannot exhaust the session budget,
        // so crossing it fails only this flow.
        if (ordering_mode_ == ordering_flow_v2 &&
            ppp::app::mux::MuxTxOverHardCap(rtx_.CidBytes(cid), (size_t)packet_length,
                rtx_flow_window_bytes_, (size_t)PPP_MUX_RTX_HARD_FACTOR)) {
            ppp::telemetry::Count("mux.rtx.flow_cap", 1);
            ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
                "rtx flow hard cap exceeded: cid=%u inflight=%u incoming=%u window=%u; failing flow",
                (unsigned)cid, (unsigned)rtx_.CidBytes(cid), (unsigned)packet_length,
                (unsigned)rtx_flow_window_bytes_);
            fail_flow(cid, "rtx_flow_overflow");
            return false;
        }
        const size_t rtx_hard_cap = ppp::app::mux::MuxTxHardCap(rtx_cap_bytes_, (size_t)PPP_MUX_RTX_HARD_FACTOR);
        ppp::app::mux::MuxDeliveryStamp stamp;
        const size_t payload_bytes = h->cmd == cmd_push && packet_length > static_cast<int>(sizeof(vmux_hdr))
            ? static_cast<size_t>(packet_length) - sizeof(vmux_hdr) : 0;
        if (payload_bytes != 0 && link && link->path_estimator_ && link_id != 0) {
            if (link->first_delivery_send_tick_ == 0 || rtx_.LinkBytes(link_id) == 0) {
                link->first_delivery_send_tick_ = now;
                link->delivered_tick_ = now;
            }
            // A transient drain of the DRR queue can happen under backpressure.
            // Require an observed idle gap with no unacknowledged work.
            const auto idle_gap = std::max<uint64_t>(10, link->min_rtt_ms_ / 4);
            // A classified latency flow is app-limited even when another
            // flow is still draining the session. Charging its sparse request
            // bursts as a capacity sample would turn a 15 Mbps path into a
            // 15 KB/s pacing target and inflate the next request's RTT.
            const auto flow_intent_for_stamp = flow_intent(cid);
            const bool intent_app_limited = intent_routing_enabled() &&
                (flow_intent_for_stamp == ppp::app::mux::TcpIntent::Latency ||
                 (flow_intent_for_stamp == ppp::app::mux::TcpIntent::Unknown &&
                  payload_bytes <= 4096));
            const bool app_limited = intent_app_limited || (app_tx_idle_since_ms_ != 0 &&
                now - app_tx_idle_since_ms_ >= idle_gap &&
                tx_data_frames_ == 0 && rtx_.bytes() == 0 &&
                link->queued_bytes_ <= static_cast<size_t>(packet_length));
            app_tx_idle_since_ms_ = 0;
            stamp = {link->delivered_payload_bytes_, link->delivered_tick_,
                link->first_delivery_send_tick_, app_limited, true};
        }
        if (!rtx_.Track(key_cid, seq, packet, packet_length, now, rtx_hard_cap, link_id, stamp, payload_bytes)) {
            // Retransmit buffer exhausted: degrade exactly like an unrecovered gap.
            ppp::telemetry::Count("mux.rtx.cap", 1);
            ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
                "rtx hard cap exceeded: inflight=%u incoming=%u window=%u hard=%u; %s",
                (unsigned)rtx_.bytes(), (unsigned)packet_length,
                (unsigned)rtx_cap_bytes_, (unsigned)rtx_hard_cap,
                ordering_mode_ == ordering_flow_v2 ? "failing flow" : "rebuilding session");
            if (ordering_mode_ == ordering_flow_v2) {
                fail_flow(cid, "rtx_buffer_overflow");
            }
            else {
                close_exec();
            }
            return false;
        }
        return true;
    }

    void vmux_net::reinject_link_inflight(const vmux_linklayer_ptr& linklayer, uint64_t now) noexcept {
        if (!reliability_on_ || NULLPTR == linklayer || linklayer->id_ == 0) {
            return;
        }

        // Once a carrier stops delivering, every
        // unacked frame last written to it is re-sent on the survivors with its
        // original sequence (the receiver deduplicates anything that did land).
        const size_t before = rtx_pending_.size();
        rtx_.TakeLinkInflight(linklayer->id_, rtx_pending_);
        const size_t added = rtx_pending_.size() - before;
        if (added > 0) {
            ppp::telemetry::Count("mux.rtx.reinject", (int64_t)added);
            retransmit_pending(now);
        }
    }

    void vmux_net::retransmit_pending(uint64_t now) noexcept {
        if (rtx_pending_.empty() || base_.disposed_.load(std::memory_order_acquire)) {
            return;
        }

        size_t burst = (size_t)PPP_MUX_RELIABILITY_RXT_BURST;
        size_t consumed = 0;
        for (; consumed < rtx_pending_.size() && burst != 0; ++consumed) {
            const uint64_t key = rtx_pending_[consumed];
            ppp::app::mux::MuxRtxEntry* entry = rtx_.Find(key);
            if (NULLPTR == entry) {
                ppp::telemetry::Count("mux.rtx.spurious", 1);
                continue; // ACKed/released since it was scheduled.
            }
            if (entry->last_sent_tick == now && entry->last_link_id != 0) {
                continue; // Already (re)sent this turn (duplicate schedule); orphaned re-injections always go.
            }
            if (entry->attempts >= rtx_max_attempts_) {
                // Unrecoverable frame: degrade exactly like an unrecovered gap.
                ppp::telemetry::Count("mux.rtx.exhausted", 1);
                const uint32_t cid = ppp::app::mux::MuxRetransmitBuffer::KeyCid(key);
                if (ordering_mode_ == ordering_flow_v2) {
                    fail_flow(cid, "rtx_exhausted");
                    continue;
                }

                close_exec();
                return;
            }

            // Pick any free link with byte credit; retransmissions keep the
            // ORIGINAL sequence number so the receiver deduplicates them.
            // A retransmission on a draining/dead carrier would just be lost
            // again and waste the attempt: pick a listed, schedulable one.
            uint16_t avoid_link = 0;
            if (multipath_scheduler_policy_.load(std::memory_order_acquire) == 0 &&
                count_live_carriers({}) > 1) {
                for (const auto& link : rx_links_) {
                    if (link && link->id_ == entry->last_link_id && link->last_stall_check_ms_ != 0 &&
                        link->last_stall_check_ms_ >= link->last_rtt_sample_ms_) {
                        avoid_link = link->id_;
                        break;
                    }
                }
            }
            const uint32_t entry_cid = ppp::app::mux::MuxRetransmitBuffer::KeyCid(key);
            auto intent = flow_intent(entry_cid);
            if (intent_routing_enabled() && intent == ppp::app::mux::TcpIntent::Unknown &&
                entry->length <= 4096) {
                intent = ppp::app::mux::TcpIntent::Latency;
            }
            vmux_linklayer_ptr chosen = tx_link_pick(entry->length, false, avoid_link, intent);
            if (NULLPTR == chosen || !chosen->schedulable()) {
                break; // Reserve the next credit for this hole, not a later, smaller frame.
            }

            if (!underlyin_sent(chosen, entry->buffer, entry->length, tx_completion_ptr(), true)) {
                tx_link_withdraw(chosen); // the refusing carrier is going away
                break;
            }
            pool_retransmitted_bytes_ += static_cast<uint64_t>(entry->length);
            if (const auto& counters = chosen->path_runtime_counters_) {
                using Counters = vmux_linklayer::PathRuntimeCounters;
                counters->Add(&Counters::retransmit_bytes, static_cast<uint64_t>(entry->length));
                counters->Add(&Counters::retransmit_events, 1);
            }
            if (!link_has_byte_credit(chosen, (int)sizeof(vmux_hdr))) {
                tx_link_withdraw(chosen); // full for the next frame; re-offered at drain entry/completion
            }

            if (entry->last_link_id != 0) {
                for (const auto& link : rx_links_) {
                    if (link && link->id_ == entry->last_link_id) {
                        link->last_retransmit_ms_ = now;
                        break;
                    }
                }
            }
            rtx_.MarkRetransmitted(key, now, chosen->id_);
            ppp::telemetry::Count("mux.rtx.send", 1);
            --burst;
        }
        rtx_pending_.erase(rtx_pending_.begin(), rtx_pending_.begin() + consumed);
    }

    uint64_t vmux_net::current_pto() const noexcept {
        // Session-level fallback: use the session SRTT when per-link state is
        // unavailable (single link, no sample yet, or compat mode).
        uint64_t pto = (srtt_ms_ > 0) ? (srtt_ms_ * 2) : (uint64_t)PPP_MUX_RELIABILITY_PTO_INIT;
        if (pto < (uint64_t)PPP_MUX_RELIABILITY_PTO_MIN) {
            pto = (uint64_t)PPP_MUX_RELIABILITY_PTO_MIN;
        }
        if (pto > (uint64_t)PPP_MUX_RELIABILITY_PTO_MAX) {
            pto = (uint64_t)PPP_MUX_RELIABILITY_PTO_MAX;
        }
        return pto;
    }

    uint64_t vmux_net::current_pto(const vmux_linklayer_ptr& linklayer) const noexcept {
        // Per-link PTO using QUIC-style formula:
        //   PTO = SRTT + max(4*RTTVAR, granularity) + max_ack_delay
        // Currently max_ack_delay = 0 (no delayed-ACK delay configured).
        // Falls back to session-level PTO when the link has no RTT sample.
        if (NULLPTR == linklayer || !linklayer->has_rtt_sample_) {
            return current_pto();
        }
        const uint64_t srtt = linklayer->srtt_ms_;
        const uint64_t rttvar = linklayer->rttvar_ms_;
        const uint64_t granularity = 1; // 1 ms tick granularity floor.
        uint64_t backoff = rttvar * 4;
        if (backoff < granularity) {
            backoff = granularity;
        }
        uint64_t pto = srtt + backoff;
        if (pto < (uint64_t)PPP_MUX_RELIABILITY_PTO_MIN) {
            pto = (uint64_t)PPP_MUX_RELIABILITY_PTO_MIN;
        }
        if (pto > (uint64_t)PPP_MUX_RELIABILITY_PTO_MAX) {
            pto = (uint64_t)PPP_MUX_RELIABILITY_PTO_MAX;
        }
        return pto;
    }

    void vmux_net::collect_stalled_link_retries(uint64_t now) noexcept {
        if (multipath_scheduler_policy_.load(std::memory_order_acquire) != 0 ||
            !rtx_pending_.empty()) return;

        for (const auto& link : rx_links_) {
            if (!is_linklayer_active(link) || link->id_ == 0 || rtx_.LinkBytes(link->id_) == 0) continue;
            if (link->intent_path_probe_.TimedOut(now, link->last_delivery_ack_ms_)) continue;
            const auto start = std::max(link->last_rtt_sample_ms_, link->first_delivery_send_tick_);
            const auto threshold = std::max<uint64_t>(500, 2 * current_pto(link));
            if (start == 0 || now - start < threshold ||
                (link->last_stall_check_ms_ != 0 &&
                 now - link->last_stall_check_ms_ < 4 * PPP_MUX_RELIABILITY_TIMER_MS)) continue;

            // Cross-carrier reordering alone does not trigger a retry.
            // Require a stalled ACK clock and recent ACKs on a different carrier.
            const bool survivor = std::any_of(rx_links_.begin(), rx_links_.end(), [&](const auto& other) {
                return other != link && is_linklayer_active(other) && other->last_rtt_sample_ms_ > start &&
                    now - other->last_rtt_sample_ms_ <= std::max<uint64_t>(500, 2 * current_pto(other));
            });
            if (!survivor) continue;

            link->last_stall_check_ms_ = now;
            link->adaptive_weight_ = 25;
            link->pressure_streak_ = 0;
            rtx_.CollectExpired(now, threshold, std::max<uint64_t>(threshold, PPP_MUX_RELIABILITY_PTO_MAX),
                PPP_MUX_RELIABILITY_RXT_BURST, rtx_pending_, link->id_);
        }
    }

    void vmux_net::recover_intent_path_timeouts(uint64_t now) noexcept {
        if (!intent_routing_enabled() || ordering_mode_ != ordering_flow_v2) return;
        const bool reachable = std::any_of(rx_links_.begin(), rx_links_.end(), [&](const auto& link) {
            return is_linklayer_active(link) && link->intent_path_probe_.RttMs() != 0 &&
                !link->intent_path_probe_.TimedOut(now, link->last_delivery_ack_ms_);
        });
        if (!reachable) return;

        bool replay_ack = false;
        for (const auto& link : rx_links_) {
            if (is_linklayer_active(link) && link->intent_path_probe_.ConsumeTimeout(now, link->last_delivery_ack_ms_)) {
                reinject_link_inflight(link, now);
                replay_ack = true;
            }
        }
        if (replay_ack) {
            // A credit-releasing cumulative ACK may have gone into the failed
            // carrier before the probe expired. Reassert the receive ranges on
            // a survivor even if the peer has no remaining credit to send data.
            for (const auto& [cid, tracker] : ack_trackers_) {
                ack_pending_cids_.insert(cid);
            }
            ack_pending_count_ = static_cast<uint32_t>(ack_pending_cids_.size());
            ack_first_pending_tick_ = now;
            maybe_send_ack(now, true);
        }

        for (auto& [cid, opening] : intent_opening_frames_) {
            const auto opening_carrier_id = opening.carrier_id;
            const auto original = std::find_if(rx_links_.begin(), rx_links_.end(), [&](const auto& link) {
                return is_linklayer_active(link) && link->id_ == opening_carrier_id;
            });
            if (original != rx_links_.end() &&
                !(*original)->intent_path_probe_.TimedOut(now, (*original)->last_delivery_ack_ms_)) continue;
            auto link = tx_link_pick(opening.packet.length, false, opening.carrier_id,
                ppp::app::mux::TcpIntent::Latency);
            if (!link) continue;
            tx_packet retry{opening.packet, {}};
            if (tx_send_on(link, retry)) {
                opening.carrier_id = link->id_;
                ppp::telemetry::Count("mux.intent.open_retry", 1);
            }
        }

        // The dialing endpoint can replace a carrier left in TCP backoff.
        // Keep the old socket for transient stalls and only restart it while
        // a confirmed carrier on a distinct path can hold the VMUX session.
        if (link_exit_callback_) {
            const auto failed = std::find_if(rx_links_.begin(), rx_links_.end(), [&](const auto& link) {
                // The primary VMUX subchannel shares the control socket.
                // Its probe may trigger repair, but replacing it would also
                // disconnect the proxy listener from its control session.
                if (!is_linklayer_active(link) || link->pool_primary_ || link->path_id_.empty() ||
                    !link->intent_path_probe_.ShouldReplace(now, link->last_delivery_ack_ms_, true)) return false;
                bool sibling = false;
                bool alternate = false;
                for (const auto& peer : rx_links_) {
                    if (peer == link || !is_linklayer_active(peer) || peer->path_id_.empty() ||
                        peer->intent_path_probe_.RttMs() == 0 ||
                        peer->intent_path_probe_.TimedOut(now, peer->last_delivery_ack_ms_)) continue;
                    if (peer->path_id_ == link->path_id_) sibling = true;
                    else alternate = true;
                }
                return alternate && link->intent_path_probe_.ShouldReplace(now, link->last_delivery_ack_ms_, sibling);
            });
            if (failed != rx_links_.end()) {
                const auto link = *failed;
                ppp::telemetry::Count("mux.intent.carrier_restart", 1);
                on_link_exit(link, "sustained intent path timeout");
            }
        }
    }

    void vmux_net::reliability_tick() noexcept {
        if (base_.disposed_.load(std::memory_order_acquire) || NULLPTR == reliability_timer_) {
            return;
        }

        const uint64_t now = now_tick();
        if (base_.established_ && reliability_on_) {
            if (intent_routing_enabled() && ordering_mode_ == ordering_flow_v2) {
                for (const auto& link : rx_links_) {
                    if (link && link->intent_path_probe_.Due(now) &&
                        send_intent_path_probe(link, IntentPathProbeRequest, static_cast<uint32_t>(now))) {
                        link->intent_path_probe_.Sent(now);
                    }
                }
            }
            recover_intent_path_timeouts(now);
            maybe_send_ack(now, false);

            collect_stalled_link_retries(now);
            if (rtx_pto_on_) {
                const size_t before = rtx_pending_.size();
                rtx_.CollectExpired(now, current_pto(), (uint64_t)PPP_MUX_RELIABILITY_PTO_MAX, (size_t)PPP_MUX_RELIABILITY_RXT_BURST, rtx_pending_);
                if (rtx_pending_.size() > before) {
                    ppp::telemetry::Count("mux.rtx.pto", 1);
                }
            }
            // Also drives re-injected frames left over when no carrier had credit.
            if (!rtx_pending_.empty()) {
                retransmit_pending(now);
            }

            // Send-window backstop: flows parked on the retransmit window are
            // normally resumed by the ACK that frees bytes; bytes freed by a
            // flow reset (EraseCid) have no ACK, so retry here. Still-full
            // flows simply park again.
            if (drr_unpark_all()) {
                process_tx_all_packets();
            }

            if (fec_on_ && fec_encoder_.count() > 0 &&
                (fec_flush_due_ || (now - fec_encoder_.first_add_tick()) >= fec_flush_ms_)) {
                fec_flush_due_ = false;
                fec_flush_group();
            }
        }

        std::weak_ptr<vmux_net> weak = weak_from_this();
        reliability_timer_->expires_after(std::chrono::milliseconds(PPP_MUX_RELIABILITY_TIMER_MS));
        reliability_timer_->async_wait(
            [weak](const boost::system::error_code& ec) noexcept {
                if (ec) {
                    return;
                }
                if (std::shared_ptr<vmux_net> self = weak.lock()) {
                    self->reliability_tick();
                }
            });
    }

    void vmux_net::start_reliability_timer() noexcept {
        if (!reliability_on_ || NULLPTR != reliability_timer_ || NULLPTR == strand_) {
            return;
        }

        reliability_timer_ = std::make_shared<boost::asio::steady_timer>(*strand_);
        reliability_tick(); // Arms the wait chain; work is a no-op pre-establishment.
    }

    void vmux_net::release_flow_reliability_state(uint32_t connection_id) noexcept {
        intent_opening_frames_.erase(connection_id);
        if (ordering_mode_ == ordering_flow_v2) {
            rtx_.EraseCid(connection_id);
            ack_trackers_.erase(connection_id);
        }
        if (!fec_on_) {
            return;
        }

        for (vmux::list<uint64_t>::iterator it = fec_frame_cache_order_.begin(); it != fec_frame_cache_order_.end();) {
            if (ppp::app::mux::MuxRetransmitBuffer::KeyCid(*it) == connection_id) {
                vmux::unordered_map<uint64_t, fec_cached_frame>::iterator cit = fec_frame_cache_.find(*it);
                if (cit != fec_frame_cache_.end()) {
                    fec_frame_cache_bytes_ -= (size_t)cit->second.length;
                    fec_frame_cache_.erase(cit);
                }
                it = fec_frame_cache_order_.erase(it);
            }
            else {
                ++it;
            }
        }
        for (vmux::list<fec_rx_group>::iterator it = fec_groups_.begin(); it != fec_groups_.end();) {
            bool references = false;
            for (const ppp::app::mux::MuxFecFrameId& id : it->view.entries) {
                if (id.connection_id == connection_id) {
                    references = true;
                    break;
                }
            }
            if (references) {
                it = fec_groups_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void vmux_net::fec_note_sent(const std::shared_ptr<Byte>& packet, int packet_length, uint64_t now) noexcept {
        if (!fec_on_ || NULLPTR == packet) {
            return;
        }
        if (packet_length < (int)sizeof(vmux_hdr) || packet_length > PPP_MUX_FEC_MAX_FRAME) {
            return; // Oversize frames stay retransmit-only (parity must fit one frame).
        }

        const vmux_hdr* h = (const vmux_hdr*)packet.get();
        if (!is_per_flow_data(h->cmd)) {
            return; // Parity covers reliable data frames only.
        }

        if (fec_encoder_.count() == 0) {
            fec_encoder_.Reset(now);
        }
        fec_encoder_.Add(ntohl(h->connection_id), ntohl(h->seq), packet.get(), packet_length);
        if (fec_encoder_.count() >= fec_group_k_) {
            // Defer emission to the maintenance tick: fec_note_sent runs inside
            // the transmit drain, where re-entering the scheduler would corrupt it.
            fec_flush_due_ = true;
        }
    }

    void vmux_net::fec_flush_group() noexcept {
        if (fec_encoder_.count() == 0) {
            return;
        }

        const uint64_t now = now_tick();
        do {
            if (!fec_on_ || !base_.established_) {
                break;
            }

            const int payload_cap = (int)fec_encoder_.MaxPayloadSize();
            std::shared_ptr<Byte> packet = make_byte_array((int)sizeof(vmux_hdr) + payload_cap);
            if (NULLPTR == packet) {
                break;
            }

            const int payload_len = fec_encoder_.Build(packet.get() + sizeof(vmux_hdr), payload_cap);
            if (payload_len <= 0) {
                break;
            }

            vmux_hdr* h = (vmux_hdr*)packet.get();
            h->cmd = cmd_fec;
            h->connection_id = htonl(0);

            PostInternalAsynchronousCallback null_ac;
            if (post_internal(packet, (int)sizeof(vmux_hdr) + payload_len, false, null_ac)) {
                ppp::telemetry::Count("mux.fec.send", 1);
            }
        } while (false);

        fec_encoder_.Reset(now);
    }

    void vmux_net::fec_note_received(const vmux_linklayer_ptr& linklayer, uint32_t connection_id, uint32_t seq, const std::shared_ptr<Byte>& buffer, int length, uint64_t now) noexcept {
        if (!fec_on_ || NULLPTR == buffer || length < (int)sizeof(vmux_hdr)) {
            return;
        }

        // Cache the frame for later single-loss recovery (bounded FIFO).
        const uint64_t key = ppp::app::mux::MuxRetransmitBuffer::Key(connection_id, seq);
        if (fec_frame_cache_.find(key) == fec_frame_cache_.end()) {
            const size_t entry_cap = (size_t)PPP_MUX_FEC_WINDOW_GROUPS * (size_t)std::max(1, fec_group_k_);
            while (!fec_frame_cache_order_.empty() &&
                (fec_frame_cache_bytes_ + (size_t)length > (size_t)PPP_MUX_FEC_CACHE_BYTES ||
                    fec_frame_cache_.size() >= entry_cap)) {
                const uint64_t victim = fec_frame_cache_order_.front();
                fec_frame_cache_order_.pop_front();
                vmux::unordered_map<uint64_t, fec_cached_frame>::iterator vit = fec_frame_cache_.find(victim);
                if (vit != fec_frame_cache_.end()) {
                    fec_frame_cache_bytes_ -= (size_t)vit->second.length;
                    fec_frame_cache_.erase(vit);
                }
            }

            fec_cached_frame cached;
            cached.buffer = buffer;
            cached.length = length;
            fec_frame_cache_.emplace(key, cached);
            fec_frame_cache_order_.emplace_back(key);
            fec_frame_cache_bytes_ += (size_t)length;
        }

        fec_try_recover_groups(linklayer, connection_id, seq, now);
    }

    void vmux_net::fec_try_recover_groups(const vmux_linklayer_ptr& linklayer, uint32_t connection_id, uint32_t seq, uint64_t now) noexcept {
        for (vmux::list<fec_rx_group>::iterator git = fec_groups_.begin(); git != fec_groups_.end();) {
            fec_rx_group& group = *git;

            int slot = -1;
            for (size_t i = 0; i < group.view.entries.size(); ++i) {
                if (group.view.entries[i].connection_id == connection_id &&
                    group.view.entries[i].sequence == seq) {
                    slot = (int)i;
                    break;
                }
            }
            if (slot < 0) {
                ++git;
                continue;
            }

            if (NULLPTR == group.frames[slot]) {
                vmux::unordered_map<uint64_t, fec_cached_frame>::iterator cit =
                    fec_frame_cache_.find(ppp::app::mux::MuxRetransmitBuffer::Key(connection_id, seq));
                if (cit != fec_frame_cache_.end()) {
                    group.frames[slot] = cit->second.buffer;
                    group.lengths[slot] = cit->second.length;
                    group.missing--;
                }
            }

            bool erase_group = false;
            if (group.missing <= 0) {
                erase_group = true; // Complete without needing recovery.
            }
            else if (group.missing == 1) {
                int missing_index = -1;
                std::vector<const uint8_t*> present(group.frames.size(), nullptr);
                std::vector<int> present_lengths(group.lengths.size(), 0);
                for (size_t i = 0; i < group.frames.size(); ++i) {
                    if (NULLPTR == group.frames[i]) {
                        missing_index = (int)i;
                    }
                    else {
                        present[i] = group.frames[i].get();
                        present_lengths[i] = group.lengths[i];
                    }
                }

                const int cap = (int)group.view.parity.size();
                std::shared_ptr<Byte> recovered = make_byte_array(std::max(cap, (int)sizeof(vmux_hdr)));
                const int recovered_length = (NULLPTR != recovered)
                    ? ppp::app::mux::MuxFecRecover(group.view, present.data(), present_lengths.data(),
                        missing_index, recovered.get(), cap)
                    : 0;
                if (recovered_length >= (int)sizeof(vmux_hdr)) {
                    vmux_hdr* rh = (vmux_hdr*)recovered.get();
                    const ppp::app::mux::MuxFecFrameId& want = group.view.entries[missing_index];
                    if (is_per_flow_data(rh->cmd) &&
                        ntohl(rh->connection_id) == want.connection_id &&
                        ntohl(rh->seq) == want.sequence) {
                        ppp::telemetry::Count("mux.fec.recovered", 1);
                        note_ack_pending(want.connection_id, want.sequence, now);
                        const bool delivered = (ordering_mode_ == ordering_flow_v2)
                            ? packet_input_flow(linklayer, rh, recovered_length, now, recovered, true)
                            : packet_input_unorder(linklayer, rh, recovered_length, now, recovered, true);
                        if (!delivered) {
                            close_exec();
                            return;
                        }
                    }
                    else {
                        ppp::telemetry::Count("mux.fec.recover_invalid", 1);
                    }
                }
                else {
                    ppp::telemetry::Count("mux.fec.recover_failed", 1);
                }
                erase_group = true;
            }

            if (erase_group) {
                git = fec_groups_.erase(git);
            }
            else {
                ++git;
            }
        }
    }

    void vmux_net::packet_input_fec(const vmux_linklayer_ptr& linklayer, Byte* buffer, int buffer_size, uint64_t now) noexcept {
        if (!fec_on_) {
            ppp::telemetry::Count("mux.fec.unexpected", 1);
            return;
        }

        ppp::app::mux::MuxFecFrameView view;
        const int max_count = std::max(1, fec_group_k_ * 4);
        if (!ppp::app::mux::ParseMuxFecFrame((const uint8_t*)buffer, buffer_size, max_count, view)) {
            ppp::telemetry::Count("mux.fec.malformed", 1);
            return;
        }

        ppp::telemetry::Count("mux.fec.recv", 1);
        while (fec_groups_.size() >= (size_t)PPP_MUX_FEC_WINDOW_GROUPS) {
            fec_groups_.pop_front();
            ppp::telemetry::Count("mux.fec.group.evict", 1);
        }

        fec_rx_group group;
        group.frames.resize(view.entries.size());
        group.lengths.resize(view.entries.size(), 0);
        group.missing = 0;
        for (size_t i = 0; i < view.entries.size(); ++i) {
            const ppp::app::mux::MuxFecFrameId& id = view.entries[i];
            vmux::unordered_map<uint64_t, fec_cached_frame>::iterator cit =
                fec_frame_cache_.find(ppp::app::mux::MuxRetransmitBuffer::Key(id.connection_id, id.sequence));
            if (cit != fec_frame_cache_.end()) {
                group.frames[i] = cit->second.buffer;
                group.lengths[i] = cit->second.length;
            }
            else {
                group.missing++;
            }
        }
        group.view = std::move(view);
        fec_groups_.emplace_back(std::move(group));

        // Slots were filled from the cache above; evaluate immediately in case
        // the group is already one frame short (or complete).
        if (!fec_groups_.empty() && !fec_groups_.back().view.entries.empty()) {
            const ppp::app::mux::MuxFecFrameId& first = fec_groups_.back().view.entries.front();
            fec_try_recover_groups(linklayer, first.connection_id, first.sequence, now);
        }
    }

    /**
     * @brief Handles remote SYN by creating and accepting a vmux socket instance.
     */
    bool vmux_net::process_rx_connecting(std::shared_ptr<vmux_skt>& skt, uint32_t connection_id, const char* host, int host_size) noexcept {
        if (base_.disposed_.load(std::memory_order_acquire)) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionDisposed);
            return false;
        }

        const bool retry_open = intent_routing_enabled() && ordering_mode_ == ordering_flow_v2 && reliability_on_;
        if (retry_open) {
            const auto& opened = intent_remote_open_ids_.ranges();
            if (std::any_of(opened.begin(), opened.end(), [connection_id](const auto& range) {
                return range.start <= connection_id && connection_id <= range.end;
            })) {
                // An original SYN can arrive from a restored carrier after the
                // retried connection already completed and released its socket.
                skt.reset();
                ppp::telemetry::Count("mux.rx.syn.duplicate", 1);
                return true;
            }
        }

        if (max_open_flows_ > 0 && skts_.size() >= max_open_flows_) {
            ppp::telemetry::Count("mux.rx.flow.max_open", 1);
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetProcessRxConnectingIdConflict);
            return false;
        }

        vmux_skt_map::iterator tail = skts_.find(connection_id);
        vmux_skt_map::iterator endl = skts_.end();
        if (tail != endl) {
            if (NULLPTR != tail->second) {
                // Duplicate SYN for a live connection: ignore it, never tear the
                // existing flow down (skt stays empty so the caller does not close it).
                skt.reset();
                ppp::telemetry::Count("mux.rx.syn.duplicate", 1);
                return true;
            }
        }

        std::shared_ptr<vmux_net> self = shared_from_this();
        skt = ppp::make_shared_object<vmux_skt>(self, connection_id);

        if (NULLPTR == skt) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetProcessRxConnectingSocketAllocFailed);
            return false;
        }

        if (retry_open) {
            // Peer CIDs are sequential and never reused. Merge adjacent IDs
            // to remember recent opens with bounded per-session memory.
            intent_remote_open_ids_.Add(connection_id, PPP_MUX_FLOW_MAX_CONTEXTS);
        }
        skt->self_weak_ = skt;
        skts_[connection_id] = skt;
        const template_string target(host, host_size);
        template_string rule_host = target;
        int rule_port = 0;
        const auto separator = target.rfind(':');
        if (separator != template_string::npos && separator + 1 < target.size()) {
            const char* port_text = target.c_str() + separator + 1;
            char* end = nullptr;
            const long parsed = std::strtol(port_text, &end, 10);
            if (end != port_text && *end == '\0' && parsed > 0 && parsed <= UINT16_MAX) {
                rule_port = static_cast<int>(parsed);
                rule_host = target.substr(0, separator);
                if (rule_host.size() >= 2 && rule_host.front() == '[' && rule_host.back() == ']') {
                    rule_host = rule_host.substr(1, rule_host.size() - 2);
                }
            }
        }
        seed_flow_intent(connection_id, rule_host, rule_port);
        return skt->accept(target);
    }

    /** @brief Generates a non-zero session-local connection identifier.
     *  @details IDs are allocated from a per-session counter and never reused
     *           within the session. When the 32-bit space wraps, further
     *           allocation fails (returns 0) so callers rebuild the session
     *           rather than risk delayed frames landing on a recycled cid.
     */
    uint32_t vmux_net::generate_id() noexcept {
        if (connection_id_wrap_) {
            return 0;
        }

        // First call: pick a random non-zero start so peers do not share a fixed pattern.
        if (next_connection_id_ == 0) {
            next_connection_id_ = ftt_random_aid(1, INT32_MAX);
            if (next_connection_id_ == 0) {
                next_connection_id_ = 1;
            }
        }

        for (uint32_t attempts = 0; attempts < 0xffffffffu; ++attempts) {
            uint32_t n = next_connection_id_++;
            if (next_connection_id_ == 0) {
                // Exhausted; mark wrap so we do not recycle within this session.
                connection_id_wrap_ = true;
                if (n != 0 && skts_.find(n) == skts_.end()) {
                    return n;
                }
                ppp::telemetry::Count("mux.cid.wrap", 1);
                ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "mux",
                    "connection id space exhausted; refuse new logical connections until session rebuild");
                return 0;
            }
            if (n == 0) {
                continue;
            }
            if (skts_.find(n) == skts_.end()) {
                return n;
            }
            // Extremely unlikely collision with an in-flight id; try next.
        }

        connection_id_wrap_ = true;
        return 0;
    }

    /** @brief Looks up a vmux socket by logical connection identifier. */
    vmux_net::vmux_skt_ptr vmux_net::get_connection(uint32_t connection_id) noexcept {
        vmux_skt_ptr skt;
        if (connection_id != 0) {
            vmux_skt_map::iterator tail = skts_.find(connection_id);
            vmux_skt_map::iterator endl = skts_.end();
            if (tail != endl) {
                skt = tail->second;
            }
        }

        return skt;
    }

    /**
     * @brief Removes a vmux socket only when pointer identity matches the caller.
     */
    vmux_net::vmux_skt_ptr vmux_net::release_connection(uint32_t connection_id, vmux_skt* refer_pointer, bool drain_terminal_tx) noexcept {
        vmux_skt_ptr skt;
        if (connection_id != 0) {
            vmux_skt_map::iterator tail = skts_.find(connection_id);
            vmux_skt_map::iterator endl = skts_.end();
            if (tail != endl) {
                skt = tail->second;
                if (skt.get() == refer_pointer) {
                    auto intent = flow_intents_.find(connection_id);
                    if (intent != flow_intents_.end()) {
                        account_flow_intent(connection_id, intent->second);
                    }
                    skts_.erase(tail);
                    if (auto it = flows_.find(connection_id); it != flows_.end()) {
                        note_flow_unbuffered(it->second.flow_reorder_.buffered_bytes());
                        flows_.erase(it);
                        rx_pressure_flows_.erase(connection_id);
                        rx_maybe_resume();
                    }
                    tx_flow_seq_.erase(connection_id);    // drop per-flow send DSN counter (flow v2)
                    intent_classifiers_.erase(connection_id);
                    flow_intents_.erase(connection_id);
                    intent_accounted_flows_.erase(connection_id);
                    release_flow_reliability_state(connection_id); // free RTX/FEC state for this cid
                    // Locally generated terminal FIN and earlier numbered frames
                    // must leave in order even after the socket is gone.
                    if (drain_terminal_tx) return skt;
                    auto txit = tx_flows_.find(connection_id);
                    if (txit != tx_flows_.end()) {
                        if (tx_data_frames_ >= txit->second.queue.size()) {
                            tx_data_frames_ -= txit->second.queue.size();
                        }
                        else {
                            tx_data_frames_ = 0;
                        }
                        if (txit->second.bytes <= tx_data_bytes_total_) {
                            tx_data_bytes_total_ -= txit->second.bytes;
                        }
                        else {
                            tx_data_bytes_total_ = 0;
                        }
                        if (txit->second.active) {
                            active_tx_flows_.remove(connection_id);
                        }
                        if (txit->second.rtx_parked) {
                            tx_rtx_parked_flows_.remove(connection_id);
                        }
                        tx_flows_.erase(txit);
                    }
                }
            }
        }

        return skt;
    }

    /**
     * @brief Queues or directly dispatches a prepared packet frame for transmit.
     */
    bool vmux_net::post_internal(const std::shared_ptr<Byte>& packet, int packet_length, bool acceleration, const PostInternalAsynchronousCallback& posted_ac) noexcept {
        if (!is_strand_thread()) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeEventDispatchFailed);
            return false;
        }

        if (NULLPTR == packet || packet_length < sizeof(vmux_hdr)) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolFrameInvalid);
            return false;
        }
        
        if (base_.disposed_.load(std::memory_order_acquire) || !base_.established_) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetPostInternalNotEstablished);
            return false;
        }

        // Exactly-once completion for this post: tracked so a session close fails
        // the waiter instead of leaving it hanging. track_tx_completion() failing
        // means close already began (it has fired the completion with false).
        tx_completion_ptr completion = make_tx_completion(posted_ac);
        if (!track_tx_completion(completion)) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionDisposed);
            return false;
        }

        vmux_hdr* h = (vmux_hdr*)packet.get();
        if (h->cmd == cmd_push) {
            const auto payload = static_cast<uint64_t>(packet_length - sizeof(vmux_hdr));
            pool_offered_bytes_ += payload;
            if (get_pool_path_floor() > 0 &&
                flow_intent(ntohl(h->connection_id)) == ppp::app::mux::TcpIntent::Bandwidth) {
                pool_offered_bulk_bytes_ += payload;
            }
        }
        bool prioritize_ctrl = false;
        bool defer_seq = false; // compat: global sequence assigned when the frame is sent
        if (is_reliability_control(h->cmd)) {
            // Reliability control frames (ack/fec) are unordered in BOTH modes:
            // seq=0, never DSN-gated, never ACKed/retransmitted/FEC-protected.
            h->seq = htonl(0);
            // Both modes take the priority queue: under the retransmit send
            // window data flows can be parked, and an ACK stuck behind them
            // would stall the peer's window (and through it, ours).
            prioritize_ctrl = true;
        }
        else if (ordering_mode_ == ordering_flow_v2) {
            // flow v2: per-flow data frames carry a per-connection DSN; control
            // frames carry seq=0 (the receiver ignores their DSN). This keeps the
            // wire header unchanged while letting the receiver order each
            // connection independently.
            Byte cmd = h->cmd;
            if (is_per_flow_data(cmd)) {
                uint32_t cid = ntohl(h->connection_id);
                // Per-connection DSN starts at 1; 0 is reserved as the control-frame
                // placeholder so a data DSN is never confused with a control frame.
                uint32_t& dsn = tx_flow_seq_[cid];
                if (dsn == 0) {
                    dsn = 1;
                }
                h->seq = htonl(dsn++);
            }
            else {
                h->seq = htonl(0);
                // Control frames (syn/syn_ok/acceleration/keep_alived/mux_mode_set)
                // are not DSN-gated at the receiver, so under flow v2 they take the
                // high-priority queue and are never starved by a data backlog.
                prioritize_ctrl = is_session_control(cmd) || is_connection_control(cmd);
            }
        }
        else {
            // compat: number frames in send order (tx_assign_seq), not enqueue
            // order; DRR serves flows out of posting order.
            h->seq = htonl(0);
            defer_seq = true;
        }
        auto make_tx = [&](const tx_completion_ptr& c) noexcept {
            tx_packet p{ packet, packet_length, c };
            p.assign_seq = defer_seq;
            return p;
        };

        if (prioritize_ctrl) {
            // Control frames bypass acceleration and jump the data backlog. The
            // optional completion is fired immediately (the frame is queued for a
            // priority drain that runs at the top of process_tx_all_packets).
            // Hard cap: a malicious or buggy peer cannot flood the control queue
            // without bound. When full, drop the oldest non-critical frame to make
            // room; SYN/FIN are never dropped, only stale ACKs/keepalives.
            if (tx_ctrl_queue_.size() >= (size_t)PPP_MUX_TX_CTRL_MAX_FRAMES) {
                ppp::telemetry::Count("mux.tx.ctrl_cap", 1);
                bool dropped = false;
                for (auto it = tx_ctrl_queue_.begin(); it != tx_ctrl_queue_.end(); ++it) {
                    const vmux_hdr* fh = (const vmux_hdr*)it->buffer.get();
                    const Byte fcmd = fh->cmd;
                    // Only drop feedback/keepalive frames, never SYN/FIN/mode_set.
                    if (fcmd == cmd_ack || fcmd == cmd_keep_alived) {
                        tx_ctrl_queue_.erase(it);
                        dropped = true;
                        break;
                    }
                }
                if (!dropped) {
                    // All queued frames are critical (SYN/FIN); reject the new one.
                    return false;
                }
            }
            tx_packet ctrl{ packet, packet_length, completion };
            tx_ctrl_queue_.emplace_back(std::move(ctrl));
            return process_tx_all_packets();
        }

        // flow turbo: a new connection's first packet (cmd_syn) is sent over the
        // most-recently-active link to cut first-byte latency. This is a one-shot
        // hint only — the connection is NOT bound; all subsequent frames go through
        // the normal competition drain. Recency is an approximate signal (not RTT);
        // fail-open to competition when no turbo link is available or the link has
        // no send credit.
        if (turbo_ && h->cmd == cmd_syn) {
            vmux_linklayer_ptr turbo_link = select_turbo_linklayer();
            if (NULLPTR != turbo_link && turbo_link->in_tx_links_ && link_has_byte_credit(turbo_link, packet_length)) {
                tx_packet syn{ packet, packet_length, completion };
                syn.assign_seq = defer_seq;
                if (tx_send_on(turbo_link, syn)) {
                    ppp::telemetry::Count("mux.turbo.syn", 1);
                    return true;
                }
            }
            // fall through to the normal path when no free turbo link is available.
        }

        if (acceleration && base_.acceleration_) {
            vmux_linklayer_list::iterator linklayer_tail = tx_links_.begin();
            vmux_linklayer_list::iterator linklayer_endl = tx_links_.end();

            if (linklayer_tail != linklayer_endl) {
                // D11 backpressure: normally the acceleration fast-path fires the
                // completion immediately so the skt read-pump reads the next chunk
                // without waiting for the send to finish. That decouples reading
                // from draining and lets data TX grow unbounded when the carrier
                // stalls. Once the data queue reaches the high-water mark, fall back
                // to attaching the completion to the frame so it fires only when the
                // frame is actually sent — this re-couples the read-pump to drain
                // progress and throttles ingestion until the backlog clears.
                const uint32_t cid = peek_connection_id(packet, packet_length);
                // A flow parked on the retransmit window, or whose own queue is
                // half way to its cap, also re-couples its read-pump, so it waits
                // for the carrier instead of overflowing its per-flow TX quota.
                size_t flow_frames = 0;
                size_t flow_bytes = 0;
                if (auto fit = tx_flows_.find(cid); fit != tx_flows_.end()) {
                    flow_frames = fit->second.queue.size();
                    flow_bytes = fit->second.bytes;
                }
                const bool throttle = ppp::app::mux::MuxTxShouldCoupleCompletion(
                    tx_data_frames_, tx_queue_high_water_, tx_flow_parked(cid),
                    flow_frames, flow_bytes,
                    (size_t)PPP_MUX_TX_FLOW_MAX_FRAMES, (size_t)PPP_MUX_TX_FLOW_MAX_BYTES);
                if (throttle) {
                    if (!enqueue_flow_tx(cid, make_tx(completion))) {
                        return fail_tx_enqueue(completion);
                    }
                }
                else {
                    if (!enqueue_flow_tx(cid, make_tx(tx_completion_ptr()))) {
                        return fail_tx_enqueue(completion);
                    }
                    if (NULLPTR != completion) {
                        // Early-ack through the tracked completion so a concurrent
                        // session close still resolves it exactly once; when the
                        // executor is already stopped, fail it inline instead of
                        // dropping the waiter.
                        std::shared_ptr<vmux_net> self = weak_from_this().lock();
                        bool fired = NULLPTR != self &&
                            vmux_post_exec(context_, strand_,
                                [self, completion]() noexcept {
                                    self->finish_tx_completion(completion, true);
                                });
                        if (!fired) {
                            finish_tx_completion(completion, false);
                        }
                    }
                }

                return process_tx_all_packets();
            }
        }

        {
            const uint32_t cid = peek_connection_id(packet, packet_length);
            if (!enqueue_flow_tx(cid, make_tx(completion))) {
                return fail_tx_enqueue(completion);
            }
        }
        return process_tx_all_packets();
    }

    bool vmux_net::fail_tx_enqueue(const tx_completion_ptr& completion) noexcept {
        // The waiter learns the frame was not accepted (its stream closes).
        finish_tx_completion(completion, false);
        // flow_v2: enqueue_flow_tx already reset only that flow. compat: the
        // frame's global sequence is spent, so report failure and let post()
        // rebuild the session now instead of waiting out a gap timeout.
        return ordering_mode_ == ordering_flow_v2;
    }

    /** @brief True when an underlying link-layer endpoint is usable. */
    bool vmux_net::is_linklayer_active(const vmux_linklayer_ptr& linklayer) noexcept {
        if (NULLPTR == linklayer) {
            return false;
        }

        // Unified schedulability check (handshake complete + not retiring).
        if (!linklayer->schedulable()) {
            return false;
        }

        const IMuxTransportPtr& connection = linklayer->connection;
        return NULLPTR != connection && connection->IsLinked();
    }

    /** @brief Drains queued packets across free links using per-flow byte DRR. */
    bool vmux_net::process_tx_compat_packets(bool interactive_only) noexcept {
        while (!tx_links_.empty() && tx_data_frames_ > 0) {
            tx_packet nexting_packet;
            if (!drr_pop_next(nexting_packet, interactive_only)) {
                break;
            }
            if (tx_rtx_window_hold(nexting_packet)) {
                continue; // flow parked on the retransmit window; serve the others
            }

            const bool new_payload = nexting_packet.buffer &&
                nexting_packet.length > static_cast<int>(sizeof(vmux_hdr)) &&
                reinterpret_cast<const vmux_hdr*>(nexting_packet.buffer.get())->cmd == cmd_push;
            const uint32_t cid = peek_connection_id(nexting_packet.buffer, nexting_packet.length);
            const auto intent = flow_intent(cid);
            vmux_linklayer_ptr linklayer = tx_link_pick(nexting_packet.length, new_payload, 0, intent);
            if (NULLPTR == linklayer) {
                const bool bulk_head = nexting_packet.length > 4096 || intent == ppp::app::mux::TcpIntent::Bandwidth;
                drr_requeue_front(std::move(nexting_packet));
                // A large frame can exhaust its own pool's credit while a
                // separate flow still has an idle interactive carrier.
                if (bulk_head && !interactive_only && active_tx_flows_.size() > 1 && intent_routing_enabled() &&
                    ordering_mode_ == ordering_flow_v2 && multipath_scheduler_policy_.load(std::memory_order_acquire) >= 0) {
                    return process_tx_compat_packets(true);
                }
                break; // An ACK or local write completion returns credit.
            }

            if (!tx_send_on(linklayer, nexting_packet)) {
                drr_requeue_front(std::move(nexting_packet));
                continue; // the refusing carrier was withdrawn
            }
            if (interactive_only && rtx_pending_.empty()) {
                ppp::telemetry::Count("mux.intent.interactive_credit_bypass", 1);
            }

            // A small request or response can wait for a carrier TCP retry
            // after one lost segment. Hedge on a second carrier and a comparable
            // independent path without adding more traffic to a bulk queue.
            if (intent_routing_enabled() && ordering_mode_ == ordering_flow_v2 && reliability_on_ &&
                new_payload && nexting_packet.length <= 4096 &&
                intent != ppp::app::mux::TcpIntent::Bandwidth && !linklayer->path_id_.empty()) {
                const auto rtt = linklayer->intent_path_probe_.MinRttMs();
                vmux_linklayer_ptr same_path, alternate_path;
                uint64_t backup_rtt = 0;
                const auto backlog = [&](const vmux_linklayer_ptr& link) {
                    return link->queued_bytes_ + rtx_.LinkBytes(link->id_);
                };
                const auto can_copy = [&](const vmux_linklayer_ptr& peer) {
                    return peer != linklayer && is_linklayer_active(peer) && !peer->path_id_.empty() &&
                        link_has_byte_credit(peer, nexting_packet.length);
                };
                const auto comparable_rtt = [&](const vmux_linklayer_ptr& peer) -> uint64_t {
                    // Select the redundant path by its propagation RTT. A
                    // lost probe can briefly inflate the primary RTT enough
                    // to admit a slow path while hiding the faster backup.
                    const auto peer_rtt = peer->intent_path_probe_.MinRttMs();
                    return peer_rtt <= 2 * rtt ? peer_rtt : 0;
                };
                for (const auto& peer : rx_links_) {
                    if (!can_copy(peer)) continue;
                    if (peer->path_id_ == linklayer->path_id_) {
                        if (!same_path || backlog(peer) < backlog(same_path)) same_path = peer;
                        continue;
                    }
                    const auto peer_rtt = comparable_rtt(peer);
                    if (!peer_rtt) continue;
                    if (alternate_path && peer->path_id_ == alternate_path->path_id_) {
                        // A comparable RTT can hide bulk already queued in a
                        // carrier's socket. Send a copy on the freer sibling.
                        if (backlog(peer) < backlog(alternate_path)) alternate_path = peer;
                        backup_rtt = std::min(backup_rtt, peer_rtt);
                    }
                    else if (!alternate_path || peer_rtt < backup_rtt) {
                        alternate_path = peer;
                        backup_rtt = peer_rtt;
                    }
                }
                vmux_linklayer_ptr idle_alternate;
                if (alternate_path) {
                    for (const auto& peer : rx_links_) {
                        if (peer != alternate_path && can_copy(peer) && peer->path_id_ == alternate_path->path_id_ &&
                            backlog(peer) == 0 && comparable_rtt(peer)) {
                            idle_alternate = peer;
                            break;
                        }
                    }
                }
                for (const auto& copy : {same_path, alternate_path, idle_alternate}) {
                    if (copy && underlyin_sent(copy, nexting_packet.buffer, nexting_packet.length, {}, true)) {
                        ppp::telemetry::Count("mux.intent.latency_copy", 1);
                    }
                }
            }
        }

        return true;
    }

    /** @brief Drains queued packets for flow mode.
     *  @details flow is the latency-oriented "new direction": pure competition on
     *           the send side (any free link sends the next queued frame — no
     *           per-connection binding, which would risk load imbalance and the
     *           single-TCP degeneration the competition model is designed to avoid),
     *           with global ordering on receive (no per-flow reordering wait). The
     *           optional turbo path (best-link-first first packet + prewarmed carrier
     *           links) layers on top via --mux-mode-turbo without changing this
     *           competition core. */
    bool vmux_net::process_tx_flow_packets() noexcept {
        return process_tx_compat_packets();
    }

    /** @brief Reads the connection_id field stored in a queued vmux frame buffer. */
    uint32_t vmux_net::peek_connection_id(const std::shared_ptr<Byte>& packet, int packet_length) noexcept {
        if (NULLPTR == packet || packet_length < (int)sizeof(vmux_hdr)) {
            return 0;
        }

        const vmux_hdr* h = (const vmux_hdr*)packet.get();
        return ntohl(h->connection_id);
    }

    /** @brief Picks the next active link-layer round-robin for stripe mode. */
    vmux_net::vmux_linklayer_ptr vmux_net::select_striped_linklayer() noexcept {
        size_t count = rx_links_.size();
        if (count == 0) {
            return NULLPTR;
        }

        for (size_t i = 0; i < count; i++) {
            const vmux_linklayer_ptr& linklayer = rx_links_[stripe_cursor_ % count];
            stripe_cursor_++;
            if (is_linklayer_active(linklayer)) {
                return linklayer;
            }
        }

        return NULLPTR;
    }

    /** @brief Picks the best available link for a turbo first packet. */
    vmux_net::vmux_linklayer_ptr vmux_net::select_turbo_linklayer() noexcept {
        // Prefer lowest outstanding local write bytes, then recency (last_active_).
        // Still not RTT; still a one-shot SYN hint with no connection binding.
        vmux_linklayer_ptr best;
        for (const vmux_linklayer_ptr& linklayer : rx_links_) {
            if (!is_linklayer_active(linklayer)) {
                continue;
            }
            if (!link_has_byte_credit(linklayer, (int)sizeof(vmux_hdr))) {
                continue;
            }

            if (NULLPTR == best) {
                best = linklayer;
                continue;
            }

            if (linklayer->queued_bytes_ < best->queued_bytes_) {
                best = linklayer;
            }
            else if (linklayer->queued_bytes_ == best->queued_bytes_ &&
                     linklayer->last_active_ >= best->last_active_) {
                best = linklayer;
            }
        }

        return best;
    }

    bool vmux_net::linklayer_id_in_use(uint16_t id, const vmux_linklayer_ptr& except) noexcept {
        if (id == 0) {
            return true;
        }

        for (const vmux_linklayer_ptr& linklayer : rx_links_) {
            if (NULLPTR != linklayer && linklayer != except && linklayer->id_ == id) {
                return true;
            }
        }

        return false;
    }

    uint16_t vmux_net::allocate_linklayer_id(const vmux_linklayer_ptr& linklayer) noexcept {
        if (NULLPTR == linklayer || status_.pool_hard_max == 0) {
            return 0;
        }

        if (linklayer->id_ != 0 && linklayer->id_ <= status_.pool_hard_max && !linklayer_id_in_use(linklayer->id_, linklayer)) {
            return linklayer->id_;
        }

        for (uint32_t id = 1; id <= status_.pool_hard_max; id++) {
            uint16_t candidate = (uint16_t)id;
            if (!linklayer_id_in_use(candidate, linklayer)) {
                linklayer->id_ = candidate;
                return candidate;
            }
        }

        return 0;
    }

    /**
     * @brief Send-side scheduling for balance mode.
     * @details balance uses the same competition policy as compat (any link with
     *          send credit sends the next queued frame; no per-connection binding).
     *          Per-connection link binding was removed deliberately: pinning a
     *          connection to a link makes load unpredictable and, in the worst case
     *          (several heavy flows landing on one link), degenerates to a single
     *          TCP — defeating multi-link VMUX. Instead balance keeps competition on
     *          send and adds per-flow DSN reordering on receive (negotiated flow v2),
     *          so a slow/blocked connection only head-of-line blocks itself, not the
     *          others, while every link stays fully and adaptively utilized.
     */
    bool vmux_net::process_tx_balance_packets() noexcept {
        return process_tx_compat_packets();
    }

    /** @brief Drains DRR-ordered packets; prefers striped link when it has credit. */
    bool vmux_net::process_tx_stripe_packets() noexcept {
        while (!tx_links_.empty() && tx_data_frames_ > 0) {
            tx_packet nexting_packet;
            if (!drr_pop_next(nexting_packet)) {
                return true;
            }
            if (tx_rtx_window_hold(nexting_packet)) {
                continue; // flow parked on the retransmit window; serve the others
            }

            // Prefer the round-robin target when it currently has send credit;
            // otherwise use any free link so a busy target does not stall output.
            const uint32_t cid = peek_connection_id(nexting_packet.buffer, nexting_packet.length);
            const auto intent = flow_intent(cid);
            vmux_linklayer_ptr linklayer =
                (intent_routing_enabled() && ordering_mode_ == ordering_flow_v2 &&
                    intent != ppp::app::mux::TcpIntent::Unknown)
                    ? tx_link_pick(nexting_packet.length, false, 0, intent)
                    : select_striped_linklayer();
            if (NULLPTR == linklayer || !linklayer->in_tx_links_ || !link_has_byte_credit(linklayer, nexting_packet.length)) {
                linklayer = tx_link_pick(nexting_packet.length, false, 0, intent);
            }
            if (NULLPTR == linklayer) {
                drr_requeue_front(std::move(nexting_packet));
                return true;
            }

            if (!tx_send_on(linklayer, nexting_packet)) {
                // Same as compat: the refusing carrier stays out of tx_links_
                // (its exit is reported by forwarding); the frame waits for the
                // next carrier rather than failing the session.
                drr_requeue_front(std::move(nexting_packet));
                continue;
            }
        }
        return true;
    }

    /** @brief Drains the high-priority control-frame queue (flow v2). */
    bool vmux_net::process_tx_ctrl_packets() noexcept {
        // Control frames are unordered under flow v2 (seq=0, delivered inline by
        // the receiver). Every control frame, ACKs included, competes for any
        // live carrier with credit through the normal scheduler: feedback must
        // never be pinned to a carrier whose reverse direction may be busy with
        // bulk data. Budget the drain so a ctrl flood cannot starve data for an
        // entire turn.
        size_t budget = tx_ctrl_budget_frames_ > 0 ? tx_ctrl_budget_frames_ : (size_t)PPP_MUX_TX_CTRL_BUDGET_FRAMES;
        const auto control_intent = intent_routing_enabled() &&
            ordering_mode_ == ordering_flow_v2
            ? ppp::app::mux::TcpIntent::Latency
            : ppp::app::mux::TcpIntent::Unknown;
        while (!tx_ctrl_queue_.empty() && budget > 0) {
            vmux_linklayer_ptr linklayer = tx_link_pick(tx_ctrl_queue_.front().length,
                false, 0, control_intent);
            if (NULLPTR == linklayer) {
                return true; // no credit right now; a completion will re-drive us.
            }

            tx_packet_ssqueue::iterator packet_tail = tx_ctrl_queue_.begin();
            tx_packet nexting_packet = *packet_tail;
            tx_ctrl_queue_.erase(packet_tail);

            if (!tx_send_on(linklayer, nexting_packet)) {
                // A carrier refusing a write is going away (its forwarding loop
                // reports the exit). Keep the frame for the next carrier instead
                // of failing the whole session: a lost SYN / keep-alive here used
                // to tear down every logical connection.
                ppp::telemetry::Count("mux.tx.ctrl.send_failed", 1);
                tx_ctrl_queue_.emplace_front(std::move(nexting_packet));
                continue;
            }
            if (intent_routing_enabled() && reliability_on_) {
                const auto* header = reinterpret_cast<const vmux_hdr*>(nexting_packet.buffer.get());
                if (header->cmd == cmd_syn || header->cmd == cmd_syn_ok) {
                    const auto cid = ntohl(header->connection_id);
                    intent_opening_frames_.insert_or_assign(cid, IntentOpeningFrame{nexting_packet, linklayer->id_});
                }
            }
            --budget;
        }

        return true;
    }

    /** @brief Drains queued packets according to selected scheduler mode. */
    bool vmux_net::process_tx_all_packets() noexcept {
        // The listed set must reflect current residency. Completions are the
        // usual re-offer point, but a carrier withdrawn while it had nothing in
        // flight (a failed carrier post, a frame that did not fit the residency
        // line) would otherwise never be listed again: no completion exists to
        // re-offer it, and it silently leaves the scheduler's candidate set
        // while the session stays up. Cheap and idempotent; O(carriers).
        tx_links_reoffer();

        // Always flush the high-priority control queue first (flow v2 only; empty
        // under compat). Keeps new-connection setup and heartbeats alive even when
        // the data queue is backlogged.
        if (!tx_ctrl_queue_.empty()) {
            if (!process_tx_ctrl_packets()) {
                return false;
            }
        }

        // Completions return carrier credit here. Give retained frames the
        // first data credit, rather than letting a busy DRR queue consume it
        // before the next ACK/timer gets a chance to repair the receiver's gap.
        if (!rtx_pending_.empty()) {
            retransmit_pending(now_tick());
            if (base_.disposed_.load(std::memory_order_acquire)) return false;
            if (!rtx_pending_.empty()) {
                // A bulk repair waiting for its own path credit must leave
                // the interactive path usable by independent flow-v2 DSNs.
                if (intent_routing_enabled() && ordering_mode_ == ordering_flow_v2 &&
                    multipath_scheduler_policy_.load(std::memory_order_acquire) >= 0) {
                    return process_tx_compat_packets(true);
                }
                return true;
            }
        }

        if (multipath_scheduler_policy_.load(std::memory_order_acquire) >= 0) {
            return process_tx_compat_packets();
        }

        switch (mode_) {
        case mux_mode_flow:
            return process_tx_flow_packets();
        case mux_mode_balance:
            return process_tx_balance_packets();
        case mux_mode_stripe:
            return process_tx_stripe_packets();
        default:
            return process_tx_compat_packets();
        }
    }
    bool vmux_net::post_frame(Byte cmd, const std::shared_ptr<Byte>& frame, int payload_length, uint32_t connection_id, bool acceleration, const PostInternalAsynchronousCallback& posted_ac) noexcept {
        if (NULLPTR == frame || payload_length < 0 || payload_length > max_buffers_size) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::NetworkPacketTooLarge);
            close_exec();
            return false;
        }

        vmux_hdr* h = (vmux_hdr*)frame.get();
        h->cmd = cmd;
        h->connection_id = htonl(connection_id);
        bool ok = post_internal(frame, (int)sizeof(vmux_hdr) + payload_length, acceleration, posted_ac);
        if (!ok) {
            close_exec();
        }
        return ok;
    }

    /**
     * @brief Builds a vmux frame from command/payload and schedules transmit.
     */
    bool vmux_net::post_internal(Byte cmd, const void* buffer, int buffer_size, uint32_t connection_id, bool acceleration, const PostInternalAsynchronousCallback& posted_ac) noexcept {
        if (!is_strand_thread()) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeEventDispatchFailed);
            return false;
        }

        if (NULLPTR != buffer && buffer_size < 0) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetPostInternalNegativeBufferSize);
            return false;
        }

        if (base_.disposed_.load(std::memory_order_acquire) || !base_.established_) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetPostFrameNotEstablished);
            return false;
        }

        // Ensure packet length does not exceed negotiated max buffer size.
        if (buffer_size > max_buffers_size) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::NetworkPacketTooLarge);
            return false;
        }

        int packet_length = sizeof(vmux_hdr) + buffer_size;
        std::shared_ptr<Byte> packet_managed = make_byte_array(packet_length);

        if (NULLPTR == packet_managed) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetPostFrameAllocFailed);
            return false;
        }

        Byte* packet_memory = packet_managed.get();
        if (NULLPTR != buffer && buffer_size > 0) {
            // memcpy with validated length to avoid overflow.
            memcpy(packet_memory + sizeof(vmux_hdr), buffer, buffer_size);
        }

        vmux_hdr* h = (vmux_hdr*)packet_memory;
        h->cmd = cmd;
        h->connection_id = htonl(connection_id);
        
        return post_internal(packet_managed, packet_length, acceleration, posted_ac);
    }

    /**
     * @brief Attaches one validated carrier to the live link containers.
     * @note Caller holds syncobj_. Handshake/forwarding starts in add_linklayer().
     */
    bool vmux_net::attach_linklayer_locked(
        const IMuxTransportPtr& connection,
        vmux_linklayer_ptr& linklayer) noexcept {
        linklayer = ppp::make_shared_object<vmux_linklayer>();
        if (NULLPTR == linklayer) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetAddLinklayerAllocFailed);
            return false;
        }

        std::shared_ptr<Byte> buffer = make_byte_array(max_buffers_size);
        if (NULLPTR == buffer) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetAddLinklayerBufferAllocFailed);
            return false;
        }

        linklayer->connection = connection;
        try {
            linklayer->path_id_ = connection->GetPathId();
            if (!linklayer->path_id_.empty() || multipath_scheduler_policy_.load(std::memory_order_acquire) >= 0) {
                linklayer->path_estimator_ = ppp::app::multipath::MakePathEstimator(configured_path_estimator());
                linklayer->path_runtime_counters_ = std::make_shared<vmux_linklayer::PathRuntimeCounters>();
                linklayer->generation_ = ++next_carrier_generation_;
            }
            if (!linklayer->path_id_.empty()) {
                auto found = path_runtime_counters_.find(linklayer->path_id_);
                if (found == path_runtime_counters_.end()) {
                    auto counters = std::make_shared<vmux_linklayer::PathRuntimeCounters>();
                    found = path_runtime_counters_.emplace(linklayer->path_id_,
                        std::move(counters)).first;
                }
                linklayer->path_runtime_counters_->path_totals = found->second;
            }
        }
        catch (...) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::MemoryAllocationFailed);
            return false;
        }
        linklayer->in_tx_links_ = true; // base carriers are listed before their handshake; post requires establishment
        tx_links_.emplace_back(linklayer);
        rx_links_.emplace_back(linklayer);
        ++pool_topology_;
        refresh_runtime_active_links();

        ppp::telemetry::Log(Level::kInfo, "mux", "link open");
        ppp::telemetry::Count("mux.link.open", 1);
        ppp::telemetry::Log(Level::kDebug, "mux", "link count=%d", static_cast<int>(rx_links_.size()));
        return true;
    }

    /**
     * @brief Adds a new transport linklayer and optionally starts full forwarding.
     */
    bool vmux_net::add_linklayer(const IMuxTransportPtr& connection, vmux_linklayer_ptr& linklayer, const vmux_native_add_linklayer_after_success_before_callback& cb) noexcept {
        if (NULLPTR == connection) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetAddLinklayerNullConnection);
            return false;
        }

        SynchronizationObjectScope __SCOPE__(syncobj_);
        if (base_.disposed_.load(std::memory_order_acquire)) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionDisposed);
            return false;
        }

        if (!connection->IsLinked()) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionTransportMissing);
            return false;
        }

        if (rx_links_.size() >= status_.pool_hard_max) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionQuotaExceeded);
            return false;
        }

        if (!attach_linklayer_locked(connection, linklayer)) {
            return false;
        }

        // Runtime addition: the session is already established (turbo dynamic pool
        // grow on either end). Attach exactly this one link and spawn exactly ONE
        // forwarding coroutine for it — never iterate rx_links_ (which would
        // re-spawn forwarding on existing links = double-forwarding). The optional
        // cb (server-side DoMuxON ack) still runs.
        if (base_.established_) {
            // Keep the pending link in rx_links_ so it counts toward the hard
            // ceiling, but withhold send credit until its handshake completes.
            tx_link_withdraw(linklayer);

            if (NULLPTR != cb && !cb()) {
                remove_linklayer_locked(linklayer);
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolMuxFailed);
                return false;
            }

            uint16_t connection_id = 0;
            if (base_.server_or_client_) {
                connection_id = allocate_linklayer_id(linklayer);
                if (connection_id == 0) {
                    remove_linklayer_locked(linklayer);
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionQuotaExceeded);
                    return false;
                }
            }

            std::shared_ptr<vmux_net> self = shared_from_this();
            vmux_linklayer_ptr added = linklayer;

            ContextPtr connection_context = connection->GetContext();
            StrandPtr connection_strand = connection->GetStrand();

            auto process =
                [self, this, added, connection_id, connection_context, connection_strand](ppp::coroutines::YieldContext& y) noexcept {
                    bool ok = handshake(added, connection_id, y);
                    if (ok) {
                        ok = vmux_post_exec(context_, strand_,
                            [self, this, added]() noexcept {
                                if (base_.disposed_.load(std::memory_order_acquire) || added->drain_.retiring()) {
                                    return false;
                                }

                                tx_link_offer(added);
                                linklayer_update(added);
                                if (!process_tx_all_packets()) {
                                    close_exec();
                                    return false;
                                }

                                return true;
                            });
                    }

                    if (ok) {
                        (void)forwarding(added, y);
                    }

                    // Runtime grow links share on_link_exit: isolate when others live.
                    vmux_post_exec(context_, strand_,
                        [self, this, added]() noexcept {
                            on_link_exit(added, "runtime_forwarding_end");
                            return true;
                        });
                };
            if (!ppp::coroutines::YieldContext::Spawn(BufferAllocator.get(), *connection_context, connection_strand.get(), process)) {
                remove_linklayer_locked(linklayer);
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeCoroutineSpawnFailed);
                return false;
            }

            ppp::telemetry::Count("mux.link.open.runtime", 1);
            return true;
        }

        bool unlimited = rx_links_.size() < get_pool_floor();
        if (unlimited) {
            if (NULLPTR != cb && !cb()) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolMuxFailed);
                return false;
            }

            return true;
        }
        elif(NULLPTR != cb && !cb()) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolMuxFailed);
            return false;
        }

        uint64_t now = now_tick();
        active(now);

        /**
         * @brief Complex startup block:
         * once enough linklayers are attached, spawn one forwarding coroutine per
         * linklayer and execute handshake + forwarding lifecycle on each.
         */
        std::shared_ptr<vmux_net> self = shared_from_this();
        for (vmux_linklayer_ptr& linklayer : rx_links_) {

            uint16_t connection_id = 0;
            if (base_.server_or_client_) {
                connection_id = allocate_linklayer_id(linklayer);
                if (connection_id == 0) {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionQuotaExceeded);
                    return false;
                }
            }

            auto& connection = linklayer->connection;
            ContextPtr connection_context = connection->GetContext();
            StrandPtr connection_strand = connection->GetStrand();

            auto process =
                [self, this, linklayer, connection_id, connection_context, connection_strand](ppp::coroutines::YieldContext& y) noexcept {
                    if (handshake(linklayer, connection_id, y)) {
                        forwarding(linklayer, y);
                    }

                    // Carrier exit: isolate if other live carriers remain.
                    vmux_post_exec(context_, strand_,
                        [self, this, linklayer]() noexcept {
                            on_link_exit(linklayer, "forwarding_end");
                            return true;
                        });
                };

            if (!ppp::coroutines::YieldContext::Spawn(BufferAllocator.get(), *connection_context, connection_strand.get(), process)) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeCoroutineSpawnFailed);
                return false;
            }

            linklayer_update(linklayer);
        }

        return true;
    }

    /**
     * @brief Begins retiring the least-recently-active carrier link at runtime (C-B4).
     */
    bool vmux_net::retire_linklayer_runtime(const vmux_linklayer_ptr& trial) noexcept {
        vmux_linklayer_vector reaped;
        {
            SynchronizationObjectScope __SCOPE__(syncobj_);
            if (base_.disposed_.load(std::memory_order_acquire) || !base_.established_) {
                return false;
            }

            // Never shrink below the base (--tun-mux): the base pool must always stand.
            // Count links that are not already retiring.
            size_t live = 0;
            for (const vmux_linklayer_ptr& l : rx_links_) {
                if (NULLPTR != l && l->schedulable()) {
                    live++;
                }
            }

            if (live <= std::max(get_pool_floor(), get_pool_path_floor()) && (!trial || trial->schedulable())) {
                return false;
            }

            // Shrink victim selection: prefer idle candidates (zero queued + zero
            // inflight) to avoid disrupting in-flight data. Among non-idle
            // candidates, pick the one with the lowest composite score
            // (inflight + queued + recency penalty) so we retire the weakest
            // contributor rather than simply the oldest.
            vmux_linklayer_ptr victim;
            vmux_linklayer_ptr idle_victim;
            uint64_t best_score = 0;
            for (const vmux_linklayer_ptr& l : rx_links_) {
                if (!l || l->pool_primary_ || (trial ? (l != trial || l->drain_.retiring()) : !l->schedulable())) {
                    continue;
                }
                if (get_pool_path_floor() > 0 && !l->path_id_.empty() &&
                    std::none_of(rx_links_.begin(), rx_links_.end(), [&](const auto& peer) {
                        return peer != l && peer && peer->schedulable() && peer->path_id_ == l->path_id_;
                    })) {
                    continue;
                }

                const size_t inflight = l->drain_.inflight();
                const size_t queued = l->queued_bytes_;

                // Prefer idle candidates (no queued or in-flight data).
                if (inflight == 0 && queued == 0) {
                    if (NULLPTR == idle_victim || l->last_active_ <= idle_victim->last_active_) {
                        idle_victim = l;
                    }
                    continue;
                }

                // Composite score: lower = weaker contributor = better victim.
                // In-flight bytes dominate, then queued bytes, then recency.
                const uint64_t score =
                    (uint64_t)inflight * 4 +
                    (uint64_t)queued +
                    (l->last_active_ / 1000); // coarse recency (seconds)

                if (NULLPTR == victim || score < best_score) {
                    victim = l;
                    best_score = score;
                }
            }

            // Prefer an idle candidate if one exists; otherwise use the
            // lowest-score non-idle candidate.
            if (NULLPTR != idle_victim) {
                victim = idle_victim;
            }

            if (NULLPTR == victim) {
                return false;
            }

            victim->drain_.BeginRetire();
            if (reliability_on_) {
                victim->retire_unacked_bytes_ = rtx_.LinkBytes(victim->id_);
                victim->retire_ack_deadline_ = now_tick() + reliability_gap_timeout_ms_;
            }
            refresh_runtime_active_links();

            // Stop sending new frames on the victim: remove it from the free-link list.
            // (If it is currently busy it is not in tx_links_; its completion sees
            // retiring_ and will not re-credit it.)
            tx_link_withdraw(victim);

            ppp::telemetry::Count("mux.link.retire.begin", 1);
            ppp::telemetry::Log(Level::kInfo, "mux", "link retiring (runtime shrink), inflight=%d", (int)victim->drain_.inflight());

            // If it already has no in-flight writes, reap immediately. Reaped
            // transports are disposed after the lock is released (no I/O under syncobj_).
            reap_retired_linklayers_locked(reaped);
        }

        finish_reaped_links(reaped);
        return true;
    }

    /**
     * @brief Collects carrier links that finished retiring (inflight_ == 0); caller holds syncobj_.
     */
    void vmux_net::reap_retired_linklayers_locked(vmux_linklayer_vector& reaped) noexcept {
        // Container ops only: reaped transports are disposed by the caller after
        // the lock is released (no I/O under syncobj_).
        bool erased_any = false;
        for (vmux_linklayer_vector::iterator it = rx_links_.begin(); it != rx_links_.end();) {
            vmux_linklayer_ptr linklayer = *it;
            if (NULLPTR != linklayer && linklayer->drain_.reapable()) {
                // Local socket completion is not peer delivery. Keep an orderly
                // carrier readable until its outstanding frames are confirmed;
                // the deadline bounds a lack of progress, not total drain time.
                if (reliability_on_) {
                    const auto outstanding = rtx_.LinkBytes(linklayer->id_);
                    if (outstanding != 0) {
                        const auto now = now_tick();
                        if (outstanding < linklayer->retire_unacked_bytes_) {
                            linklayer->retire_unacked_bytes_ = outstanding;
                            linklayer->retire_ack_deadline_ = now + reliability_gap_timeout_ms_;
                        }
                        if (now < linklayer->retire_ack_deadline_) {
                            ++it;
                            continue;
                        }
                        rtx_.TakeLinkInflight(linklayer->id_, rtx_pending_);
                    }
                }
                it = rx_links_.erase(it);
                linklayer->exited_ = true; // A late forwarding exit must not touch a reused carrier id.
                ++pool_topology_;
                erased_any = true;

                // Ensure it is not left in the free-link list.
                tx_link_withdraw(linklayer);

                reaped.emplace_back(linklayer);
            }
            else {
                ++it;
            }
        }

        if (erased_any) {
            refresh_runtime_active_links();
        }
    }

    /**
     * @brief Disposes carrier links that finished retiring (inflight_ == 0).
     */
    void vmux_net::reap_retired_linklayers() noexcept {
        vmux_linklayer_vector reaped;
        {
            SynchronizationObjectScope __SCOPE__(syncobj_);
            reap_retired_linklayers_locked(reaped);
        }

        finish_reaped_links(reaped);
    }

    void vmux_net::finish_reaped_links(vmux_linklayer_vector& reaped) noexcept {
        for (vmux_linklayer_ptr& linklayer : reaped) {
            if (NULLPTR != linklayer && NULLPTR != linklayer->connection) {
                if (link_exit_callback_) {
                    try { link_exit_callback_(linklayer->connection->GetTransmission(), true); }
                    catch (...) {}
                }
                linklayer->connection->Dispose();
            }
            ppp::telemetry::Count("mux.link.retire.done", 1);
            ppp::telemetry::Log(Level::kInfo, "mux", "link retired (runtime shrink), links=%d", (int)rx_links_.size());
        }
        if (!reaped.empty()) retransmit_pending(now_tick());
    }

    void vmux_net::turbo_controller_tick(uint64_t now) noexcept {
        using namespace ppp::app::multipath;
        if (base_.server_or_client_ || !pool_grow_ || !base_.established_ || base_.disposed_ ||
            (!is_pool_lazy() && (!turbo_ || multipath_scheduler_policy_.load() >= 0))) return;
        PoolSample s;
        s.now = now;
        s.floor = std::max(get_pool_floor(), get_pool_path_floor());
        s.limit = get_pool_hard_max();
        s.reliable = reliability_on_;
        s.offered = pool_offered_bytes_;
        s.acked = pool_acked_payload_bytes_;
        s.retransmitted = pool_retransmitted_bytes_;
        s.pressure = tx_data_bytes_total_ != 0 && (s.reliable ||
            tx_data_frames_ >= std::max<size_t>(1, tx_queue_high_water_ / 2));
        s.idle = tx_data_frames_ == 0 && (!s.reliable || rtx_.bytes() == 0);
        if (get_pool_path_floor() > 0) {
            const auto delivered = pool_received_bulk_bytes_ - pool_previous_received_bulk_bytes_;
            pool_previous_received_bulk_bytes_ = pool_received_bulk_bytes_;
            const auto bulk_flows = std::count_if(flow_intents_.begin(), flow_intents_.end(),
                [](const auto& flow) { return flow.second == ppp::app::mux::TcpIntent::Bandwidth; });
            // The client dials new carriers for traffic in either direction.
            // Count observed downloads as demand and compare their delivered
            // rate before and during the same bounded trial used for uploads.
            s.offered = pool_offered_bulk_bytes_ + pool_received_bulk_bytes_;
            s.acked += pool_received_bulk_bytes_;
            s.pressure = bulk_flows > 0 && (s.pressure || (bulk_flows > 1 && delivered >= (1u << 20)));
            s.idle = bulk_flows == 0;
            s.reprobe_after_idle = true;
        }
        {
            SynchronizationObjectScope __SCOPE__(syncobj_);
            s.topology = pool_topology_;
            s.occupied = static_cast<unsigned>(rx_links_.size());
            for (const auto& link : rx_links_) {
                if (!link) continue;
                const bool active = link->schedulable();
                if (link == pool_trial_link_) {
                    s.trial_attached = true;
                    s.trial_ready = active;
                }
                if (!active) continue;
                ++s.live;
                if (link->has_rtt_sample_) {
                    s.rtt_ms = std::max(s.rtt_ms, link->srtt_ms_);
                    // An unconfirmed min_rtt (first sample only) is not a
                    // queueing baseline; publishing the whole SRTT as queue
                    // delay would misreport the startup sample.
                    const auto queue_delay = link->min_rtt_ms_ != 0 &&
                        link->srtt_ms_ > link->min_rtt_ms_
                        ? link->srtt_ms_ - link->min_rtt_ms_ : 0;
                    s.queue_delay_ms = std::max(s.queue_delay_ms, queue_delay);
                }
            }
        }
        if (pool_dial_in_flight_ && !pool_token_) return; // expired; wait for the dial coroutine to return
        switch (const auto action = pool_controller_.Step(s); action) {
        case PoolAction::Hold: return;
        case PoolAction::Dial:
            if (get_pool_path_floor() > 0) {
                std::uint64_t most_payload = 0;
                pool_trial_path_.clear();
                for (const auto& [path, counters] : path_runtime_counters_) {
                    if (!counters) continue;
                    const auto payload = counters->rx_unique_payload_bytes.load(std::memory_order_relaxed) +
                        counters->tx_unique_payload_bytes.load(std::memory_order_relaxed);
                    if (payload > most_payload) {
                        most_payload = payload;
                        pool_trial_path_ = path;
                    }
                }
            }
            pool_token_ = ++pool_next_token_;
            pool_dial_in_flight_ = pool_token_;
            ppp::telemetry::Count("mux.pool.dial.requested", 1);
            if (!pool_grow_(pool_token_)) pool_dial_failed(pool_token_);
            return;
        case PoolAction::Keep:
            pool_target_.store(static_cast<uint16_t>(s.live), std::memory_order_release);
            ppp::telemetry::Count(s.reliable ? "mux.pool.gain.keep" : "mux.pool.queue.keep", 1);
            break;
        case PoolAction::Reject:
        case PoolAction::Inconclusive:
            if (s.trial_attached) {
                // Local send residency cannot reveal in-flight downloads on
                // the peer. Keep the trial until the receiving workload is
                // idle so shrinking cannot truncate its remote TCP queue.
                if (get_pool_path_floor() > 0 && !s.idle && s.trial_ready) {
                    pool_target_.store(static_cast<uint16_t>(s.live), std::memory_order_release);
                } else {
                    retire_linklayer_runtime(pool_trial_link_);
                }
            }
            ppp::telemetry::Count(action == PoolAction::Reject ? "mux.pool.gain.reject" : "mux.pool.gain.unknown", 1);
            break;
        case PoolAction::Shrink:
            if (retire_linklayer_runtime()) {
                pool_target_.store(static_cast<uint16_t>(s.live - 1), std::memory_order_release);
                ppp::telemetry::Count("mux.pool.idle.retire", 1);
            }
            break;
        }
        pool_token_ = 0;
        pool_trial_link_.reset();
    }

    /**
     * @brief Performs server/client handshake for one attached linklayer.
     */
    bool vmux_net::handshake(const vmux_linklayer_ptr& linklayer, uint16_t connection_id, ppp::coroutines::YieldContext& y) noexcept {
        ppp::telemetry::SpanScope span("mux.link.setup");
        auto setup_started_at = std::chrono::steady_clock::now();

        if (base_.disposed_.load(std::memory_order_acquire)) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionDisposed);
            return false;
        }

        IMuxTransportPtr& linklayer_socket = linklayer->connection;
        if (!linklayer_socket->IsLinked()) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionTransportMissing);
            return false;
        }

        ITransmissionPtr linklayer_transmission = linklayer_socket->GetTransmission();
        if (NULLPTR == linklayer_transmission) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionTransportMissing);
            return false;
        }

        /** @brief Packed handshake acknowledgement payload carrying receive id. */
#pragma pack(push, 1)
        typedef struct 
#if defined(__GNUC__) || defined(__clang__)
            __attribute__((packed)) 
#endif
        {
            uint16_t receive_id;
        } vmux_linlayer_add_ack_packet;
#pragma pack(pop)

        if (base_.server_or_client_) {
            if (connection_id == 0 || connection_id > status_.pool_hard_max) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolMuxFailed);
                return false;
            }

            vmux_linlayer_add_ack_packet packet;
            packet.receive_id = htons(connection_id);

            if (!linklayer_transmission->Write(y, &packet, sizeof(vmux_linlayer_add_ack_packet))) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolMuxFailed);
                return false;
            }
        }
        else {
            int buffer_size = 0;
            std::shared_ptr<Byte> packet_memory = linklayer_transmission->Read(y, buffer_size);
            if (NULLPTR == packet_memory || buffer_size < sizeof(vmux_linlayer_add_ack_packet)) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolFrameInvalid);
                return false;
            }

            vmux_linlayer_add_ack_packet* packet = (vmux_linlayer_add_ack_packet*)packet_memory.get();
            uint32_t receive_id = ntohs(packet->receive_id);

            // receive_id is assigned by the server and may exceed the base pool
            // when turbo grows carrier links at runtime.
            if (receive_id == 0 || receive_id > status_.pool_hard_max) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolMuxFailed);
                return false;
            }

            SynchronizationObjectScope __SCOPE__(syncobj_);
            if (linklayer_id_in_use((uint16_t)receive_id, linklayer)) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolMuxFailed);
                return false;
            }

            linklayer->id_ = (uint16_t)receive_id;
        }

        {
            SynchronizationObjectScope __SCOPE__(syncobj_);
            if (base_.disposed_ || linklayer->drain_.retiring()) return false;
            linklayer->handshake_complete_ = true;
            carrier_resume_deadline_ms_.store(0, std::memory_order_release);
            if (!base_.established_ && status_.opened_connections < get_pool_floor()) {
                status_.opened_connections++;
            }
            refresh_runtime_active_links();
        }

        linklayer_established();
        auto setup_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - setup_started_at).count();
        ppp::telemetry::Histogram("mux.link.setup.us", setup_elapsed);
        return true;
    }

    /** @brief Updates mux established state once enough linklayers are opened. */
    void vmux_net::linklayer_established() noexcept {
        SynchronizationObjectScope __SCOPE__(syncobj_);
        if (!base_.established_) {
            base_.established_ = 
                status_.opened_connections >= get_pool_floor();

            ppp::telemetry::Log(Level::kDebug, "mux", "linklayer handshake complete, links=%d", static_cast<int>(status_.opened_connections));

            uint64_t now = now_tick();
            status_.last_heartbeat_ = now;

            active(now);
            switch_to_next_heartbeat_timeout();
        }
    }

    /**
     * @brief Runs continuous read/dispatch forwarding on one linklayer.
     */
    bool vmux_net::forwarding(const vmux_linklayer_ptr& linklayer, ppp::coroutines::YieldContext& y) noexcept {
        if (base_.disposed_.load(std::memory_order_acquire)) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionDisposed);
            return false;
        }

        IMuxTransportPtr& linklayer_socket = linklayer->connection;
        if (!linklayer_socket->IsLinked()) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionTransportMissing);
            return false;
        }

        ITransmissionPtr linklayer_transmission = linklayer_socket->GetTransmission();
        if (NULLPTR == linklayer_transmission) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionTransportMissing);
            return false;
        }

        int buffer_size = 0;
        boost::system::error_code ec;

        // Read once: scheduler_trace_writer() is a function-local static, and the
        // receive loop must not touch the clock or any trace counter unless the
        // operator explicitly opened OPENPPP2_MULTIPATH_SCHEDULER_TRACE_JSON.
        const bool trace_enabled = scheduler_trace_writer().enabled;
        bool any = false;
        std::shared_ptr<vmux_net> self = shared_from_this();

        linklayer_update(linklayer);
        for (;;) {
            if (base_.disposed_.load(std::memory_order_acquire)) {
                break;
            }

            if (!linklayer_socket->IsLinked()) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionTransportMissing);
                break;
            }

            // Receive backpressure: while the vmux strand has paused this carrier
            // (reorder budget over its soft cap), stop reading so the carrier's own
            // TCP flow control throttles the sender. The strand clears the flag as
            // the reorder buffer drains. One pause is bounded: the missing frame
            // may itself be queued on this carrier, so after the lease it reads
            // again (and is re-paused by the strand if it is still ahead). Fail
            // open if the sleep cannot be armed.
            while (linklayer->rx_paused_.load(std::memory_order_acquire) && linklayer->schedulable()) {
                if (base_.disposed_.load(std::memory_order_acquire) || !linklayer_socket->IsLinked()) {
                    break;
                }
                const uint64_t paused_since = linklayer->rx_paused_since_.load(std::memory_order_relaxed);
                if (now_tick() - paused_since >= (uint64_t)PPP_MUX_RX_PAUSE_MAX_MS) {
                    linklayer->rx_paused_.store(false, std::memory_order_release);
                    ppp::telemetry::Count("mux.rx.backpressure.lease_expired", 1);
                    break;
                }
                if (!ppp::coroutines::asio::async_sleep(y, PPP_MUX_RX_PAUSE_POLL_MS)) {
                    break;
                }
            }
            if (base_.disposed_.load(std::memory_order_acquire)) {
                break;
            }

            std::shared_ptr<Byte> buffer_memory = linklayer_transmission->Read(y, buffer_size);
            const std::uint64_t trace_read_done_us = trace_enabled ? trace_now_us() : 0;
            if (NULLPTR == buffer_memory || buffer_size < sizeof(vmux_hdr)) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolFrameInvalid);
                break;
            }

            vmux_hdr* h = (vmux_hdr*)buffer_memory.get();
            Byte cmd = h->cmd;
            if (cmd <= cmd_none || cmd >= cmd_max) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolPacketActionInvalid);
                break;
            }

            bool posted = vmux_post_exec(context_, strand_,
                [self, this, linklayer, buffer_memory, h, buffer_size, trace_enabled, trace_read_done_us]() noexcept {
                    const std::uint64_t trace_handler_start_us = trace_enabled ? trace_now_us() : 0;
                    uint64_t now = now_tick();
                    if (const auto& counters = linklayer->path_runtime_counters_) {
                        using Counters = vmux_linklayer::PathRuntimeCounters;
                        counters->Add(&Counters::rx_bytes, static_cast<uint64_t>(buffer_size));
                    }
                    note_inbound_reliability_frame(linklayer, buffer_memory, h, buffer_size, now);
                    bool delivered = (ordering_mode_ == ordering_flow_v2)
                        ? packet_input_flow(linklayer, h, buffer_size, now, buffer_memory)
                        : packet_input_unorder(linklayer, h, buffer_size, now, buffer_memory);
                    if (trace_enabled) {
                        // Dispatch = carrier-read completion to vmux-strand entry;
                        // handler = frame handling on the vmux strand (reorder admit,
                        // ACK bookkeeping, socket input). Crypto is not on this path:
                        // the transport encrypts/decrypts inside Read()/Write().
                        trace_record_rx_frame(*linklayer, trace_read_done_us,
                            trace_handler_start_us, trace_now_us());
                    }
                    if (delivered) {
                        return true;
                    }
                    else {
                        close_exec();
                        return false;
                    }
                });

            if (!posted) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeTaskPostFailed);
            }

            any |= posted;
        }
        
        return any;
    }

    /** @brief Refreshes activity on the underlying linklayer connection. */
    void vmux_net::linklayer_update(const vmux_linklayer_ptr& linklayer) noexcept {
        if (NULLPTR == linklayer) {
            return;
        }

        // Stamp the most-recent-inbound tick used by turbo's approximate
        // best-link selection (recency, not RTT). Atomic: written by the vmux
        // strand on every inbound frame and by the carrier forwarding coroutine.
        linklayer->last_active_ = now_tick();

        IMuxTransportPtr& connection = linklayer->connection;
        if (NULLPTR != connection && connection->IsLinked()) {
            connection->Update();
        }
    }

    /** @brief Validates preconditions for initiating a logical vmux connect. */
    bool vmux_net::connect_require(
        const std::shared_ptr<boost::asio::ip::tcp::socket>& sk,
        const template_string&                               host,
        int                                                  port) noexcept {

        if (base_.disposed_.load(std::memory_order_acquire) || !base_.established_) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetConnectRequireNotEstablished);
            return false;
        }

        if (host.empty() || port <= 0 || port > UINT16_MAX) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetConnectRequireHostOrPortInvalid);
            return false;
        }

        if (NULLPTR == sk) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetConnectRequireNullSocket);
            return false;
        }

        if (max_open_flows_ > 0 && skts_.size() >= max_open_flows_) {
            ppp::telemetry::Count("mux.tx.flow.max_open", 1);
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionQuotaExceeded);
            return false;
        }

        return true;
    }

    /**
     * @brief Coroutine-friendly connect helper that waits for async completion.
     */
    bool vmux_net::connect_yield(
        ppp::coroutines::YieldContext&                       y,
        const ContextPtr&                                    context,
        const StrandPtr&                                     strand,
        const std::shared_ptr<boost::asio::ip::tcp::socket>& sk,
        const template_string&                               host,
        int                                                  port,
        const std::shared_ptr<vmux_skt_ptr>&                 return_connection) noexcept {

        if (!y || !return_connection) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetConnectYieldInvalidArguments);
            return false;
        }

        if (NULLPTR == context) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::AppContextUnavailable);
            return false;
        }

        if (!connect_require(sk, host, port)) {
            return false;
        }

        std::shared_ptr<vmux_net::atomic_int> status = ppp::make_shared_object<vmux_net::atomic_int>(-1);
        if (NULLPTR == status) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetConnectYieldStatusAllocFailed);
            return false;
        }

        // Guard Suspend() behind the post result: if the executor is unavailable the
        // lambda (and every ppp::coroutines::asio::R() inside it) will never run, so
        // calling Suspend() would park the coroutine with no future Resume() �?a
        // permanent coroutine leak.
        // Capture a strong reference to keep *this alive until the posted
        // lambda finishes.  finalize() does not drain strand_, so a bare
        // this would dangle after the last shared_ptr is released.
        std::shared_ptr<vmux_net> self = shared_from_this();

        const ppp::coroutines::YieldContext::Anchor anchor = y.Arm();
        if (!anchor) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeStateTransitionInvalid);
            return false;
        }

        bool posted = vmux_post_exec(context_, strand_,
            [self, sk, host, port, status, context, strand, return_connection, anchor]() noexcept {
                bool ok = self->connect(context, strand, sk, host, port,
                    [status, return_connection, anchor](vmux_skt* sender, bool success) noexcept {

                        ppp::coroutines::asio::R(anchor, *status, success, 
                            [return_connection, sender]() noexcept {
                                /* The socket may already be inside its destructor when
                                 * this failure callback fires (session teardown while a
                                 * logical connect is in flight). shared_from_this() on a
                                 * dying object throws bad_weak_ptr across noexcept
                                 * boundaries -> std::terminate. Locking the weak
                                 * self-reference instead returns an empty pointer
                                 * without throwing, which safely reports the failure. */
                                std::shared_ptr<vmux_skt> connection =
                                    NULLPTR != sender ? sender->self_weak_.lock() : NULLPTR;
                                if (NULLPTR != connection) {
                                    *return_connection = connection;
                                }
                            });
                    });

                if (!ok) {
                    ppp::coroutines::asio::R(anchor, *status, false);
                }
            });

        if (!posted) {
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeTaskPostFailed);
            return false;
        }

        y.Suspend(anchor);
        return status->load() > 0;
    }

    /**
     * @brief Starts an asynchronous logical vmux connection creation workflow.
     */
    bool vmux_net::connect(const ContextPtr& context, const StrandPtr& strand, const std::shared_ptr<boost::asio::ip::tcp::socket>& sk, const template_string& host, int port, const ConnectAsynchronousCallback& ac) noexcept {
        if (NULLPTR == context || !connect_require(sk, host, port)) {
            if (NULLPTR == context) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::AppContextUnavailable);
            }
            return false;
        }

        vmux_skt_ptr skt;
        std::shared_ptr<vmux_net> self = shared_from_this();

        for (;;) {
            uint32_t connection_id = generate_id();
            if (connection_id == 0) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionQuotaExceeded);
                return false;
            }

            vmux_skt_map::iterator skt_tail = skts_.find(connection_id);
            vmux_skt_map::iterator skt_endl = skts_.end();
            if (skt_tail != skt_endl) {
                continue;
            }

            skt = ppp::make_shared_object<vmux_skt>(self, connection_id);
            if (NULLPTR == skt) {
                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VmuxNetConnectSocketAllocFailed);
                return false;
            }

            skt->self_weak_ = skt;
            skt->tx_socket_ = sk;
            skts_[connection_id] = skt;
            seed_flow_intent(connection_id, host, port);
            trace_flow_open(*sk, connection_id, port);
            break;
        }

        if (skt->connect(context, strand, host, port, ac)) {
            return true;
        }
        else {
            skt->clear_event();
            skt->close();
            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocketConnectFailed);
            return false;
        }
    }
}
