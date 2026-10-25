#pragma once

// Direct-upload data queue shared by VirtualEthernetTcpipConnection and its
// focused tests (production and tests use this exact class).
//
// Invariants:
//  - one serialized producer (the XTCP shard strand calling Push) and one
//    serialized consumer (the direct upload writer, plus the disposing owner);
//  - the chunk owns its upload credit until the gather write completes or the
//    chunk is dropped, so credit is released exactly once;
//  - byte/item counters cover queued plus in-flight chunks and reach zero once
//    every admitted chunk has been released or cleared;
//  - the backpressure flag is raised at most once per episode and the
//    low-watermark resume is reported exactly once per raise.
//
// Deque mode: every call is made under the owner's control lock (direct_sync_).

#include <ppp/app/protocol/XtcpDirectIo.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace ppp::app::protocol {

class XtcpDirectUploadQueue final {
public:
    using ChunkPtr = std::shared_ptr<XtcpUploadChunk>;

    static constexpr std::size_t kMaxItems = 4096;
    static constexpr std::size_t kLowItems = kMaxItems / 2;
    static constexpr std::size_t kMaxBytes = 32 * 1024 * 1024;
    static constexpr std::size_t kLowBytes = kMaxBytes / 2;
    static constexpr std::size_t kGatherWaitMaxItems = 16;

    struct PushOutcome final {
        XtcpDirectResult result = XtcpDirectResult::Closed;
        bool backpressure_raised = false; // false -> true transition happened
    };

    // Copies the borrowed bytes only after the per-flow caps admitted them.
    // On rejection the credit is left with the caller.
    PushOutcome Push(const std::uint8_t* data, std::size_t length,
        XtcpUploadBudget::Reservation&& credit) noexcept {
        PushOutcome outcome;
        if (items_ >= kMaxItems || length > kMaxBytes - bytes_) {
            outcome.result = XtcpDirectResult::Backpressured;
            outcome.backpressure_raised = !backpressured_;
            backpressured_ = true;
            return outcome;
        }
        try {
            queue_.emplace_back(std::make_shared<XtcpUploadChunk>(data, length, std::move(credit)));
        }
        catch (...) {
            return outcome;
        }
        bytes_ += length;
        ++items_;
        outcome.result = XtcpDirectResult::Accepted;
        return outcome;
    }

    bool Empty() const noexcept { return queue_.empty(); }
    std::size_t Bytes() const noexcept { return bytes_; }
    std::size_t Items() const noexcept { return items_; }
    bool Backpressured() const noexcept { return backpressured_; }
    void ResetBackpressure() noexcept { backpressured_ = false; }

    // True when a short bounded wait may let adjacent segments join the write.
    bool ShouldWaitForGather(std::size_t gather_limit) const noexcept {
        return bytes_ < gather_limit && queue_.size() > 1 &&
            queue_.size() < kGatherWaitMaxItems;
    }

    // Pops the head chunk, merging following same-budget chunks up to
    // gather_limit bytes. Credits are merged only after every copy succeeded.
    bool DrainGather(std::size_t gather_limit, ChunkPtr& payload, std::size_t& items) noexcept {
        if (queue_.empty()) return false;
        payload = queue_.front();
        items = 1;
        if (gather_limit > 1 && payload->bytes.size() < gather_limit && queue_.size() > 1) {
            std::size_t gathered_bytes = payload->bytes.size();
            std::size_t gathered_items = 1;
            for (; gathered_items < queue_.size(); ++gathered_items) {
                const ChunkPtr& candidate = queue_[gathered_items];
                if (!candidate || candidate->bytes.empty() ||
                    !payload->credit.SameBudget(candidate->credit) ||
                    candidate->bytes.size() > gather_limit - gathered_bytes) {
                    break;
                }
                gathered_bytes += candidate->bytes.size();
            }
            if (gathered_items > 1) {
                try {
                    auto combined = std::make_shared<XtcpUploadChunk>();
                    combined->bytes.reserve(gathered_bytes);
                    for (std::size_t i = 0; i < gathered_items; ++i) {
                        const std::vector<std::uint8_t>& item = queue_[i]->bytes;
                        combined->bytes.insert(combined->bytes.end(), item.begin(), item.end());
                    }
                    for (std::size_t i = 0; i < gathered_items; ++i) {
                        const bool merged = combined->credit.Merge(std::move(queue_[i]->credit));
                        assert(merged); // SameBudget was checked above.
                        (void)merged;
                    }
                    payload = std::move(combined);
                    items = gathered_items;
                    for (std::size_t i = 0; i < gathered_items; ++i) {
                        queue_.pop_front();
                    }
                    return true;
                }
                catch (...) {
                }
            }
        }
        queue_.pop_front();
        return true;
    }

    // Accounts a finished write. Returns true exactly once when a successful
    // write drops a backpressured queue to the low watermark.
    bool Release(std::size_t bytes, std::size_t items, bool sent,
        std::size_t& completed_bytes, std::size_t& completed_items) noexcept {
        completed_bytes = std::min(bytes, bytes_);
        bytes_ -= completed_bytes;
        completed_items = std::min(items, items_);
        items_ -= completed_items;
        if (sent && backpressured_ && bytes_ <= kLowBytes && items_ <= kLowItems) {
            backpressured_ = false;
            return true;
        }
        return false;
    }

    // Drops queued chunks (releasing their credit) and zeroes the counters.
    // Returns whether the queue was backpressured.
    bool Clear(std::size_t& abandoned_bytes, std::size_t& abandoned_items) noexcept {
        abandoned_bytes = std::exchange(bytes_, 0);
        abandoned_items = std::exchange(items_, 0);
        queue_.clear();
        return std::exchange(backpressured_, false);
    }

private:
    std::deque<ChunkPtr> queue_;
    std::size_t bytes_ = 0;
    std::size_t items_ = 0;
    bool backpressured_ = false;
};

} // namespace ppp::app::protocol
