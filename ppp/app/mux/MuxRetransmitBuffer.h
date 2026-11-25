#pragma once

/**
 * @file MuxRetransmitBuffer.h
 * @brief Sender-side retransmission buffer for the vmux reliability
 *        sub-protocol (QUIC-style ACK + fast retransmit + PTO).
 * @license GPL-3.0
 *
 * Retains a shared_ptr copy of every reliably-sent frame until the peer's ACK
 * covers it, so a lost frame can be re-sent on another live carrier link with
 * its ORIGINAL sequence number (the receiver deduplicates). Keyed by
 * (connection_id, sequence); under ordering_compat the caller uses
 * connection_id 0 because the sequence space is already session-global.
 *
 * Bounded: a session-wide byte cap (Track fails when exceeded; the caller
 * degrades to fail_flow / session rebuild) and per-frame attempt accounting
 * (the caller tears down when attempts are exhausted). The caller also uses
 * bytes() / CidBytes() as a send window before frames go out (see
 * MuxTxBackpressure.h), so the Track cap is only a hard bound.
 *
 * Not thread-safe: strand-affine by design, like the rest of vmux state.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include <ppp/app/mux/MuxAckTracker.h>

namespace ppp::app::mux {

// State of the carrier delivery clock when a new payload frame is sent.
struct MuxDeliveryStamp final {
    std::uint64_t prior_delivered = 0;
    std::uint64_t delivered_tick = 0;
    std::uint64_t first_sent_tick = 0;
    bool is_app_limited = false;
    bool valid = false;
};

struct MuxAckDelivery final {
    std::uint64_t bytes = 0;
    std::uint64_t sent_tick = 0;
    MuxDeliveryStamp stamp;

    void Merge(const MuxAckDelivery& delivery) noexcept {
        bytes += delivery.bytes;
        if (!stamp.valid || delivery.stamp.prior_delivered > stamp.prior_delivered ||
            (delivery.stamp.prior_delivered == stamp.prior_delivered &&
                (delivery.sent_tick > sent_tick || (delivery.sent_tick == sent_tick &&
                    stamp.is_app_limited && !delivery.stamp.is_app_limited)))) {
            sent_tick = delivery.sent_tick;
            stamp = delivery.stamp;
        }
    }
};

/** One retained outbound frame awaiting peer acknowledgment. */
struct MuxRtxEntry final {
    std::shared_ptr<std::uint8_t> buffer;         ///< Full vmux frame (header + payload).
    int                           length = 0;     ///< Frame length in bytes.
    std::uint64_t                 first_sent_tick = 0; ///< Tick of the first transmission.
    std::uint64_t                 last_sent_tick = 0;  ///< Tick of the most recent (re)transmission.
    std::uint32_t                 attempts = 0;   ///< Retransmissions already performed.
    const char*                   retry_source = nullptr; ///< One outstanding retry request across ACK, PTO and carrier exit.
    std::uint64_t                 retry_delay_ms = 0; ///< Initial carrier RTT for fast loss detection, then the replacement feedback interval.
    std::uint32_t                 fast_rtx_mark = 0;   ///< largest-acked value at the last fast retransmit (dedup).
    std::uint16_t                 orig_link_id = 0;  ///< Carrier link the frame was FIRST sent on (for per-link RTT attribution).
    std::uint16_t                 last_link_id = 0;  ///< Carrier link the frame was most recently (re)sent on (for per-link PTO).
    MuxDeliveryStamp delivery_stamp;
    std::size_t payload_bytes = 0;
    std::list<std::uint64_t>::iterator order;     ///< Position in the insertion-ordered list (stable iterator).
};

class MuxRetransmitBuffer final {
public:
    /** Pack (connection_id, sequence) into one ordering key. */
    static constexpr std::uint64_t Key(std::uint32_t connection_id, std::uint32_t sequence) noexcept {
        return (static_cast<std::uint64_t>(connection_id) << 32) | sequence;
    }

    static constexpr std::uint32_t KeyCid(std::uint64_t key) noexcept {
        return static_cast<std::uint32_t>(key >> 32);
    }

    /**
     * Retain one freshly-sent frame.
     * @return false for an empty/invalid frame or when the byte cap would be exceeded.
     */
    bool Track(
        std::uint32_t connection_id,
        std::uint32_t sequence,
        const std::shared_ptr<std::uint8_t>& buffer,
        int length,
        std::uint64_t now,
        std::size_t byte_cap,
        std::uint16_t link_id = 0,
        const MuxDeliveryStamp& delivery_stamp = {},
        std::size_t payload_bytes = 0,
        std::uint64_t initial_fast_delay_ms = 0) {
        const std::uint64_t key = Key(connection_id, sequence);
        if (entries_.find(key) != entries_.end()) {
            return true; // Already tracked (defensive; sends are unique per key).
        }
        if (length <= 0 || (byte_cap > 0 && bytes_ + static_cast<std::size_t>(length) > byte_cap)) {
            return false;
        }

        order_.emplace_back(key);
        MuxRtxEntry entry;
        entry.buffer = buffer;
        entry.length = length;
        entry.first_sent_tick = now;
        entry.last_sent_tick = now;
        entry.orig_link_id = link_id;
        entry.last_link_id = link_id;
        entry.delivery_stamp = delivery_stamp;
        entry.payload_bytes = payload_bytes;
        entry.retry_delay_ms = initial_fast_delay_ms;
        entry.order = (--order_.end());
        entries_.emplace(key, std::move(entry));
        bytes_ += static_cast<std::size_t>(length);
        cid_flight_[connection_id].bytes += static_cast<std::size_t>(length);
        AddLinkBytes(link_id, static_cast<std::size_t>(length));
        return true;
    }

    /**
     * Release every entry of @p connection_id covered by the ACK ranges and
     * collect fast-retransmit candidates. A gap must be at least
     * @p fast_threshold sequence numbers behind @p largest and the most recent
     * send must be at least @p fast_time_threshold old. For a retransmitted
     * frame, its carrier feedback interval must also have elapsed and the
     * peer must have acknowledged data first sent after the retransmission.
     * A candidate is reported at most once per advancing @p largest. When
     * @p collect_fast is false, holes are neither reported nor marked.
     * If supplied, @p cumulative_payload accumulates newly acknowledged payload
     * bytes across calls, saturating at UINT64_MAX regardless of carrier attribution.
     * If supplied, @p acknowledged_keys receives the keys released by this ACK.
     * @return RTT sample in ms from the first newly-acked entry that was never
     *         retransmitted (Karn's rule); 0 when no usable sample exists.
     */
    std::uint64_t Ack(
        std::uint32_t connection_id,
        std::uint32_t largest,
        const std::vector<MuxAckRange>& ranges,
        std::uint64_t now,
        std::uint32_t fast_threshold,
        std::uint64_t fast_time_threshold,
        std::vector<std::uint64_t>& fast_candidates,
        std::uint16_t* out_link_id = nullptr,
        std::unordered_map<std::uint16_t, std::size_t>* out_link_acked_bytes = nullptr,
        std::unordered_map<std::uint16_t, MuxAckDelivery>* out_deliveries = nullptr,
        std::uint64_t* cumulative_payload = nullptr,
        bool collect_fast = true,
        std::vector<std::uint64_t>* acknowledged_keys = nullptr) {
        std::uint64_t rtt_sample = 0;
        std::vector<std::uint64_t> acked;
        if (nullptr != out_link_id) {
            *out_link_id = 0;
        }
        if (nullptr != out_link_acked_bytes) {
            out_link_acked_bytes->clear();
        }

        if (out_deliveries) out_deliveries->clear();

        // Late ACKs for a drained/reset flow require no scan of other flows.
        auto flow = cid_flight_.find(connection_id);
        if (flow == cid_flight_.end()) return 0;
        const auto candidate_start = fast_candidates.size();
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (KeyCid(it->first) != connection_id) {
                ++it;
                continue;
            }
            const auto seq = static_cast<std::uint32_t>(it->first);
            const bool acknowledged = std::any_of(ranges.begin(), ranges.end(),
                [seq](const MuxAckRange& range) { return seq >= range.start && seq <= range.end; });
            if (!acknowledged) {
                auto& entry = it->second;
                if (collect_fast && !entry.retry_source && largest > seq && largest - seq >= fast_threshold &&
                    entry.fast_rtx_mark != largest &&
                    now >= entry.last_sent_tick && now - entry.last_sent_tick >= std::max(fast_time_threshold, entry.retry_delay_ms)) {
                    fast_candidates.push_back(it->first);
                }
                ++it;
                continue;
            }
            if (rtt_sample == 0 && it->second.attempts == 0 && now >= it->second.first_sent_tick) {
                rtt_sample = now - it->second.first_sent_tick;
                if (nullptr != out_link_id) {
                    *out_link_id = it->second.orig_link_id;
                }
            }
            // Delivery belongs to the link carrying the final transmission.
            // RTT sampling above intentionally remains tied to orig_link_id
            // and Karn's rule; these are separate accounting concepts.
            if (nullptr != out_link_acked_bytes && it->second.last_link_id != 0) {
                (*out_link_acked_bytes)[it->second.last_link_id] +=
                    static_cast<std::size_t>(it->second.length);
            }
            // VMUX ACKs can confirm an earlier copy after a retransmission.
            // Only an unambiguous first send can anchor a delivery sample.
            const auto& entry = it->second;
            if (out_deliveries && entry.last_link_id != 0 && entry.attempts == 0 &&
                entry.delivery_stamp.valid && entry.payload_bytes != 0) {
                (*out_deliveries)[entry.last_link_id].Merge(
                    {entry.payload_bytes, entry.first_sent_tick, entry.delivery_stamp});
            }
            if (cumulative_payload) {
                *cumulative_payload = entry.payload_bytes > UINT64_MAX - *cumulative_payload
                    ? UINT64_MAX : *cumulative_payload + entry.payload_bytes;
            }
            if (acknowledged_keys) acked.push_back(it->first);
            ReleaseLinkBytes(it->second.last_link_id, static_cast<std::size_t>(it->second.length));
            bytes_ -= static_cast<std::size_t>(it->second.length);
            flow->second.bytes -= static_cast<std::size_t>(entry.length);
            flow->second.latest_acked_send = std::max(flow->second.latest_acked_send, entry.first_sent_tick);
            order_.erase(it->second.order);
            it = entries_.erase(it);
        }

        // Frames retain their DSN on retry. Later ACKs from the original flight
        // can advance largest for many RTTs without saying anything about the
        // replacement. Require delivery of a frame first sent after that retry.
        auto accepted = fast_candidates.begin() + candidate_start;
        for (auto candidate = accepted; candidate != fast_candidates.end(); ++candidate) {
            auto& entry = entries_.find(*candidate)->second;
            if (entry.attempts != 0 && flow->second.latest_acked_send <= entry.last_sent_tick) continue;
            entry.fast_rtx_mark = largest;
            *accepted++ = *candidate;
        }
        fast_candidates.erase(accepted, fast_candidates.end());
        if (acknowledged_keys) acknowledged_keys->swap(acked);
        if (flow->second.bytes == 0) cid_flight_.erase(flow);
        return rtt_sample;
    }

    /**
     * Collect keys whose last (re)transmission is older than the per-entry
     * exponential-backoff PTO, oldest first, bounded by @p max_count.
     * A nonzero link_id restricts collection to that carrier's current flight.
     *
     * Each entry's timeout is  base_pto << min(attempts, 5)  using saturating
     * shift-left (so it never overflows to zero), then clamped to @p pto_max.
     * This prevents the 8 retransmissions from firing in a flat 8*PTO burst
     * and instead follows the QUIC/RFC 6298 exponential backoff principle.
     */
    void CollectExpired(std::uint64_t now, std::uint64_t base_pto,
        std::uint64_t pto_max, std::size_t max_count,
        std::vector<std::uint64_t>& expired, std::uint16_t link_id = 0,
        const std::unordered_map<std::uint16_t, std::uint64_t>* link_ptos = nullptr) const {
        for (const auto key : order_) {
            if (expired.size() >= max_count) {
                break;
            }
            const MuxRtxEntry& entry = entries_.find(key)->second;
            if (entry.retry_source || (link_id != 0 && entry.last_link_id != link_id)) {
                continue;
            }
            if (now < entry.last_sent_tick) {
                continue;
            }
            // Saturating shift-left: base_pto << shift, clamped to UINT64_MAX.
            std::uint64_t effective = base_pto;
            if (link_ptos) {
                const auto carrier = link_ptos->find(entry.last_link_id);
                if (carrier != link_ptos->end()) effective = carrier->second;
            }
            effective = BackedOffPto(effective, entry.attempts, pto_max);
            if (entry.attempts != 0) effective = std::max(effective, entry.retry_delay_ms);
            if ((now - entry.last_sent_tick) >= effective) {
                expired.push_back(key);
            }
        }
    }

    MuxRtxEntry* Find(std::uint64_t key) noexcept {
        auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : &it->second;
    }

    static std::uint64_t BackedOffPto(std::uint64_t base, std::uint32_t attempts,
        std::uint64_t maximum) noexcept {
        const auto shift = std::min<std::uint32_t>(attempts, 5);
        return base > (maximum >> shift) ? maximum : base << shift;
    }

    /** Multiple loss signals share one pending retry until it is actually written. */
    bool QueueRetry(std::uint64_t key, std::vector<std::uint64_t>& pending, const char* source) {
        auto* entry = Find(key);
        if (!entry || entry->retry_source) return false;
        pending.push_back(key);
        entry->retry_source = source;
        return true;
    }

    /** Account one retransmission of @p key on @p link_id. */
    void MarkRetransmitted(std::uint64_t key, std::uint64_t now,
        std::uint16_t link_id = 0, std::uint64_t retry_delay_ms = 0) noexcept {
        MuxRtxEntry* entry = Find(key);
        if (nullptr != entry) {
            entry->retry_source = nullptr;
            entry->retry_delay_ms = retry_delay_ms;
            if (entry->last_link_id != link_id) {
                try {
                    AddLinkBytes(link_id, static_cast<std::size_t>(entry->length));
                } catch (...) {
                    ReleaseLinkBytes(entry->last_link_id, static_cast<std::size_t>(entry->length));
                    entry->last_link_id = 0;
                    entry->attempts++;
                    entry->last_sent_tick = now;
                    return;
                }
                ReleaseLinkBytes(entry->last_link_id, static_cast<std::size_t>(entry->length));
            }
            entry->attempts++;
            entry->last_sent_tick = now;
            entry->last_link_id = link_id;
        }
    }

    /**
     * Carrier exit: collect, oldest first, every unacked entry whose most recent
     * (re)transmission went out on @p link_id, and orphan it (last_link_id = 0,
     * last_sent_tick = 0) so it is re-sent on a live carrier exactly once and a
     * later carrier reusing the same id cannot claim it. link_id 0 is "unknown"
     * and never matches.
     */
    void TakeLinkInflight(std::uint16_t link_id, std::vector<std::uint64_t>& out) {
        if (link_id == 0) {
            return;
        }
        for (std::uint64_t key : order_) {
            auto it = entries_.find(key);
            if (it == entries_.end() || it->second.last_link_id != link_id) {
                continue;
            }
            ReleaseLinkBytes(link_id, static_cast<std::size_t>(it->second.length));
            // The numeric ID may be reused before this frame is retransmitted.
            // A late ACK must not supply an RTT/rate sample to its replacement.
            it->second.orig_link_id = 0;
            it->second.last_link_id = 0;
            it->second.last_sent_tick = 0;
            if (it->second.retry_source) it->second.retry_source = "exit";
            else QueueRetry(key, out, "exit");
        }
    }

    /** Drop every entry of one connection space (flow reset / flow release). */
    void EraseCid(std::uint32_t connection_id) {
        cid_flight_.erase(connection_id);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (KeyCid(it->first) == connection_id) {
                bytes_ -= static_cast<std::size_t>(it->second.length);
                ReleaseLinkBytes(it->second.last_link_id, static_cast<std::size_t>(it->second.length));
                order_.erase(it->second.order);
                it = entries_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void Clear() noexcept {
        entries_.clear();
        order_.clear();
        cid_flight_.clear();
        link_bytes_.clear();
        bytes_ = 0;
    }

    std::size_t size() const noexcept { return entries_.size(); }
    std::size_t bytes() const noexcept { return bytes_; }

    /** Unacked bytes retained for one connection space (released by Ack / EraseCid). */
    std::size_t CidBytes(std::uint32_t connection_id) const noexcept {
        auto it = cid_flight_.find(connection_id);
        return it == cid_flight_.end() ? 0 : it->second.bytes;
    }

    /** Unacked bytes whose most recent transmission is assigned to @p link_id. */
    std::size_t LinkBytes(std::uint16_t link_id) const noexcept {
        if (link_id == 0) return 0;
        auto it = link_bytes_.find(link_id);
        return it == link_bytes_.end() ? 0 : it->second;
    }

private:
    void AddLinkBytes(std::uint16_t link_id, std::size_t length) {
        if (link_id != 0) {
            link_bytes_[link_id] += length;
        }
    }

    void ReleaseLinkBytes(std::uint16_t link_id, std::size_t length) noexcept {
        if (link_id == 0) return;
        auto it = link_bytes_.find(link_id);
        if (it == link_bytes_.end()) return;
        if (it->second <= length) it->second = 0;
        else it->second -= length;
    }

    struct CidFlight {
        std::size_t bytes = 0;
        std::uint64_t latest_acked_send = 0;
    };

    // std::list iterators are stable across insert/erase, so each entry can
    // store its position for O(1) removal from the insertion-ordered list.
    std::list<std::uint64_t>                                       order_;
    std::unordered_map<std::uint64_t, MuxRtxEntry>                 entries_;
    std::unordered_map<std::uint32_t, CidFlight>                   cid_flight_;
    std::unordered_map<std::uint16_t, std::size_t>                 link_bytes_;
    std::size_t                                                    bytes_ = 0;
};

} // namespace ppp::app::mux
