#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <ppp/app/mux/MuxAckTracker.h>

namespace {

constexpr std::size_t kMaxBlocks = 8;
constexpr std::size_t kMaxRanges = 24;
constexpr std::size_t kMaxSequences = 128;
constexpr std::size_t kDecodeInputLimit = 4096;

class FuzzReader final {
public:
    FuzzReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    std::uint8_t Byte() noexcept {
        if (data_ == nullptr || offset_ >= size_) {
            return 0;
        }
        return data_[offset_++];
    }

    std::uint32_t ReadLittleEndian(unsigned bytes) noexcept {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) {
            value |= static_cast<std::uint32_t>(Byte()) << (8 * i);
        }
        return value;
    }

    std::uint16_t U16() noexcept { return static_cast<std::uint16_t>(ReadLittleEndian(2)); }
    std::uint32_t U32() noexcept { return ReadLittleEndian(4); }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
};

void AssertDecodedStructure(const std::vector<ppp::app::mux::MuxAckBlock>& blocks) {
    assert(!blocks.empty());
    assert(blocks.size() <= kMaxBlocks);
    for (const ppp::app::mux::MuxAckBlock& block : blocks) {
        assert(!block.ranges.empty());
        assert(block.ranges.size() <= kMaxRanges);
        for (const ppp::app::mux::MuxAckRange& range : block.ranges) {
            assert(range.start <= range.end);
            assert(range.end <= block.largest);
        }
    }
}

void AssertTrackerRanges(const ppp::app::mux::MuxAckTracker& tracker) {
    assert(tracker.size() <= kMaxRanges);
    const std::vector<ppp::app::mux::MuxAckRange>& ranges = tracker.ranges();
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        assert(ranges[i].start <= ranges[i].end);
        if (i != 0) {
            assert(ranges[i - 1].end < ranges[i].start);
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::size_t decode_size = size < kDecodeInputLimit ? size : kDecodeInputLimit;
    std::vector<ppp::app::mux::MuxAckBlock> arbitrary_decoded;
    const bool arbitrary_valid = ppp::app::mux::DecodeMuxAckFrame(
        data, decode_size, kMaxBlocks, kMaxRanges, arbitrary_decoded);
    if (arbitrary_valid) {
        AssertDecodedStructure(arbitrary_decoded);
    }
    else {
        assert(arbitrary_decoded.empty());
    }

    FuzzReader reader(data, size);
    const std::size_t block_count = 1 + (reader.Byte() % kMaxBlocks);
    std::vector<ppp::app::mux::MuxAckBlock> blocks;
    blocks.reserve(block_count);
    for (std::size_t b = 0; b < block_count; ++b) {
        ppp::app::mux::MuxAckBlock block;
        block.connection_id = reader.U32();
        block.largest = 0x10000u + (reader.U16() & 0x7FFFu);

        const std::size_t range_count = 1 + (reader.Byte() % kMaxRanges);
        block.ranges.reserve(range_count);
        std::uint32_t cursor = reader.U16() & 0x03FFu;
        for (std::size_t r = 0; r < range_count; ++r) {
            cursor += 1u + (reader.Byte() % 4u);
            const std::uint32_t start = cursor;
            const std::uint32_t end = start + (reader.Byte() % 8u);
            block.ranges.push_back({start, end});
            cursor = end;
        }
        blocks.push_back(std::move(block));
    }

    const std::size_t wire_capacity = ppp::app::mux::MuxAckFrameMaxSize(kMaxBlocks, kMaxRanges);
    std::vector<std::uint8_t> wire(wire_capacity);
    const std::size_t encoded = ppp::app::mux::EncodeMuxAckFrame(
        blocks.data(), blocks.size(), wire.data(), wire.size(), kMaxRanges);
    assert(encoded != 0 && encoded <= wire.size());

    std::vector<ppp::app::mux::MuxAckBlock> roundtrip;
    assert(ppp::app::mux::DecodeMuxAckFrame(
        wire.data(), encoded, kMaxBlocks, kMaxRanges, roundtrip));
    assert(roundtrip.size() == blocks.size());
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        assert(roundtrip[b].connection_id == blocks[b].connection_id);
        assert(roundtrip[b].largest == blocks[b].largest);
        assert(roundtrip[b].ranges.size() == blocks[b].ranges.size());
        for (std::size_t r = 0; r < blocks[b].ranges.size(); ++r) {
            assert(roundtrip[b].ranges[r].start == blocks[b].ranges[r].start);
            assert(roundtrip[b].ranges[r].end == blocks[b].ranges[r].end);
        }
    }

    ppp::app::mux::MuxAckTracker tracker;
    const std::size_t sequence_count = reader.Byte() % (kMaxSequences + 1);
    for (std::size_t i = 0; i < sequence_count; ++i) {
        tracker.Add(reader.U32(), kMaxRanges);
        AssertTrackerRanges(tracker);
    }

    return 0;
}
