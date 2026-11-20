#include <ppp/app/mux/MuxFecCodec.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
using ppp::app::mux::MuxFecRecover;
using ppp::app::mux::ParseMuxFecFrame;

constexpr std::size_t kMaxInput = 4096;
constexpr std::size_t kMaxFrames = 16;
constexpr std::size_t kMaxFrameSize = 256;
constexpr int kParseMaxCount = static_cast<int>(kMaxFrames);

std::uint8_t InputByte(const std::uint8_t* data, std::size_t size,
    std::size_t index, std::uint8_t salt) noexcept {
    if (data != nullptr && size != 0) {
        return static_cast<std::uint8_t>(data[index % size] + salt);
    }
    return static_cast<std::uint8_t>((index * 29u) + salt);
}

void ParseMutations(const std::uint8_t* data, std::size_t size,
    const std::uint8_t* source, std::size_t source_size) {
    // Parse the untrusted input itself first. The length cap also bounds the
    // parity copy made by ParseMuxFecFrame.
    ppp::app::mux::MuxFecFrameView view;
    ParseMuxFecFrame(data, static_cast<int>(size), kParseMaxCount, view);

    if (source == nullptr || source_size == 0) {
        return;
    }

    std::vector<std::uint8_t> mutated(source, source + source_size);
    const std::size_t flips = source_size < 6 ? source_size : 6;
    for (std::size_t i = 0; i < flips; ++i) {
        const std::size_t position =
            (static_cast<std::size_t>(InputByte(data, size, i * 3, 0x51)) + i * 17u) % source_size;
        mutated[position] ^= static_cast<std::uint8_t>(
            InputByte(data, size, i * 5 + 1, static_cast<std::uint8_t>(0xA5u + i)) | 1u);
    }

    // Every parse below receives a pointer into the bounded copy and a length
    // no larger than that copy, including deliberately truncated payloads.
    ParseMuxFecFrame(mutated.data(), static_cast<int>(mutated.size()), kParseMaxCount, view);
    const std::size_t truncations = mutated.size() < 5 ? mutated.size() : 5;
    for (std::size_t i = 0; i < truncations; ++i) {
        const std::size_t length =
            static_cast<std::size_t>(InputByte(data, size, i * 7 + 2, 0xC3)) % (mutated.size() + 1);
        ParseMuxFecFrame(mutated.data(), static_cast<int>(length), kParseMaxCount, view);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::size_t bounded_size = size < kMaxInput ? size : kMaxInput;

    // Exercise the parser directly on arbitrary bytes before constructing a
    // valid bounded group. ParseMuxFecFrame is required to tolerate truncation.
    ParseMutations(data, bounded_size, nullptr, 0);

    ppp::app::mux::MuxFecEncoder encoder;
    encoder.Reset(static_cast<std::uint64_t>(bounded_size));

    const std::size_t frame_count =
        2u + (static_cast<std::size_t>(InputByte(data, bounded_size, 0, 0x17)) % (kMaxFrames - 1u));
    std::vector<std::vector<std::uint8_t>> frames;
    frames.reserve(kMaxFrames);

    for (std::size_t i = 0; i < frame_count; ++i) {
        const std::size_t length = 1u +
            (static_cast<std::size_t>(InputByte(data, bounded_size, 1u + i * 3u, 0x29)) % kMaxFrameSize);
        frames.emplace_back(length);
        for (std::size_t j = 0; j < length; ++j) {
            frames.back()[j] = InputByte(data, bounded_size, 7u + i * 13u + j, static_cast<std::uint8_t>(i + j));
        }

        const std::uint32_t connection_id =
            (static_cast<std::uint32_t>(InputByte(data, bounded_size, 2u + i * 4u, 0x11)) << 24) |
            (static_cast<std::uint32_t>(InputByte(data, bounded_size, 3u + i * 4u, 0x23)) << 16) |
            (static_cast<std::uint32_t>(InputByte(data, bounded_size, 4u + i * 4u, 0x37)) << 8) |
            static_cast<std::uint32_t>(InputByte(data, bounded_size, 5u + i * 4u, 0x49));
        const std::uint32_t sequence = static_cast<std::uint32_t>(i) +
            static_cast<std::uint32_t>(InputByte(data, bounded_size, 6u + i * 4u, 0x5B));
        encoder.Add(connection_id, sequence, frames.back().data(), static_cast<int>(length));
    }

    const std::size_t payload_capacity = encoder.MaxPayloadSize();
    std::vector<std::uint8_t> wire(payload_capacity);
    const int wire_length = encoder.Build(wire.data(), static_cast<int>(wire.size()));
    if (wire_length <= 0) {
        return 0;
    }

    ppp::app::mux::MuxFecFrameView view;
    if (!ParseMuxFecFrame(wire.data(), wire_length, kParseMaxCount, view)) {
        return 0;
    }

    // Exercise mutation and truncation against a real, well-formed frame too.
    ParseMutations(data, bounded_size, wire.data(), static_cast<std::size_t>(wire_length));

    std::array<const std::uint8_t*, kMaxFrames> present{};
    std::array<int, kMaxFrames> present_lengths{};
    const std::size_t missing =
        static_cast<std::size_t>(InputByte(data, bounded_size, 11, 0x6D)) % frame_count;
    for (std::size_t i = 0; i < frame_count; ++i) {
        if (i != missing) {
            present[i] = frames[i].data();
            present_lengths[i] = static_cast<int>(frames[i].size());
        }
    }

    std::array<std::uint8_t, kMaxFrameSize> recovered{};
    const int recovered_length = MuxFecRecover(view, present.data(), present_lengths.data(),
        static_cast<int>(missing), recovered.data(), static_cast<int>(recovered.size()));
    if (recovered_length != static_cast<int>(frames[missing].size()) ||
        std::memcmp(recovered.data(), frames[missing].data(), frames[missing].size()) != 0) {
        __builtin_trap();
    }

    return 0;
}
