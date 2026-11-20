#include <ppp/app/mux/MuxFlowContextAdmission.h>
#include <ppp/app/mux/MuxFlowReorderBuffer.h>
#include <ppp/app/mux/MuxRxQueueLimit.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace {

using ppp::app::mux::AdmitFlowContext;
using ppp::app::mux::MuxFlowReorderBuffer;
using ppp::app::mux::MuxRxQueueOverflows;

constexpr std::size_t kMaxOperations = 512;
constexpr std::size_t kMaxByteCap = 4096;
constexpr std::size_t kMaxEntryCap = 32;

class InputCursor final {
public:
    InputCursor(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    std::uint8_t ReadByte() noexcept {
        assert(position_ < size_);
        return data_[position_++];
    }

    std::uint32_t ReadUint32() noexcept {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8) {
            value |= static_cast<std::uint32_t>(ReadByte()) << shift;
        }
        return value;
    }

    std::size_t remaining() const noexcept { return size_ - position_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_ = 0;
};

bool ContainsPendingSequence(
    const std::array<bool, kMaxOperations>& pending,
    const std::array<std::uint32_t, kMaxOperations>& sequences,
    std::uint32_t sequence) noexcept {
    for (std::size_t i = 0; i != kMaxOperations; ++i) {
        if (pending[i] && sequences[i] == sequence) {
            return true;
        }
    }
    return false;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2 || data == nullptr) {
        return 0;
    }

    // Keep every resource bound independent of the input length.
    const std::size_t byte_cap = 1 + (data[0] % kMaxByteCap);
    const std::size_t entry_cap = 1 + (data[1] % kMaxEntryCap);
    InputCursor input(data + 2, size - 2);

    MuxFlowReorderBuffer<std::uint32_t> reorder;
    std::array<bool, kMaxOperations> pending{};
    std::array<bool, kMaxOperations> delivered{};
    std::array<std::uint32_t, kMaxOperations> packet_sequences{};
    std::array<std::size_t, kMaxOperations> packet_bytes{};
    std::size_t pending_count = 0;
    std::size_t tracked_bytes = 0;

    for (std::size_t operation = 0;
         operation != kMaxOperations && input.remaining() >= 10;
         ++operation) {
        const std::uint8_t opcode = input.ReadByte();
        const std::uint32_t sequence = input.ReadUint32();
        const std::uint32_t reference = input.ReadUint32();
        const std::size_t payload_bytes = input.ReadByte() % (byte_cap + 2);
        const unsigned action = opcode & 3u;

        // This is the receive-queue guard used before attempting an insert.
        // Its inputs are bounded, so the helper itself cannot overflow while
        // the fuzzer explores large sequence and reference values.
        const int soft_congestion = static_cast<int>((reference >> 8) & 0x7ffu);
        const bool queue_overflow = MuxRxQueueOverflows(
            reorder.size(), static_cast<std::int64_t>(tracked_bytes),
            static_cast<int>(payload_bytes), soft_congestion);

        if (action == 0) {
            const std::uint32_t packet_id = static_cast<std::uint32_t>(operation);
            const bool inserted = !queue_overflow && reorder.TryInsert(
                sequence, reference, packet_id, payload_bytes,
                byte_cap, entry_cap);
            if (inserted) {
                assert(!ContainsPendingSequence(
                    pending, packet_sequences, sequence));
                pending[operation] = true;
                packet_sequences[operation] = sequence;
                packet_bytes[operation] = payload_bytes;
                ++pending_count;
                tracked_bytes += payload_bytes;
            }
        } else if (action == 1) {
            std::uint32_t packet_id = 0;
            if (reorder.Take(sequence, packet_id)) {
                // Every value inserted above has a unique operation id.  A
                // successful Take must deliver one still-pending packet and
                // must never deliver that packet id a second time.
                assert(packet_id < kMaxOperations);
                assert(pending[packet_id]);
                assert(!delivered[packet_id]);
                assert(packet_sequences[packet_id] == sequence);
                assert(ContainsPendingSequence(
                    pending, packet_sequences, sequence));

                pending[packet_id] = false;
                delivered[packet_id] = true;
                assert(pending_count != 0);
                --pending_count;
                assert(tracked_bytes >= packet_bytes[packet_id]);
                tracked_bytes -= packet_bytes[packet_id];
            }
        } else if (action == 2) {
            std::uint32_t selected = 0;
            if (reorder.FirstSequence(reference, selected)) {
                assert(ContainsPendingSequence(
                    pending, packet_sequences, selected));
            }
        } else {
            const bool already_tracked = (opcode & 4u) != 0;
            const bool socket_exists = (opcode & 8u) != 0;
            const std::size_t current_contexts = sequence % 64u;
            const std::size_t context_cap = reference % 64u;
            const auto admission = AdmitFlowContext(
                sequence, already_tracked, socket_exists,
                current_contexts, context_cap);
            assert(static_cast<unsigned>(admission) <=
                   static_cast<unsigned>(
                       ppp::app::mux::FlowContextAdmission::RejectZero));
        }

        // These invariants hold after both accepted and rejected operations.
        assert(reorder.size() == pending_count);
        assert(reorder.size() <= entry_cap);
        assert(reorder.buffered_bytes() == tracked_bytes);
        assert(reorder.buffered_bytes() <= byte_cap);
    }

    return 0;
}
