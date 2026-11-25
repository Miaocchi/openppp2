#pragma once

#include <ppp/stdafx.h>
#include <limits>

namespace ppp::transmissions {
// Wire-read limit, not a decoded payload or secure record limit. The byte-wise
// base94 codec can double a binary frame; allow room for its framing overhead.
inline constexpr int MaxEncodedTransmissionFrame = 2 * PPP_BUFFER_SIZE + 64;

inline constexpr int TransmissionBinaryHeaderBytes = 3;
inline constexpr int TransmissionBase94MinHeaderBytes = 4;
inline constexpr int TransmissionBase94MaxHeaderBytes = 7;

/**
 * Largest raw next frame that fits the logical WriteBytes queue, including
 * writes already posted to the carrier strand but not yet completed. Each
 * logical Write adds one binary header. Base94 can double every binary byte
 * and adds its own four- or seven-byte header.
 */
inline std::uint64_t TransmissionWriteCredit(bool base94, int max_items, int max_bytes,
    std::uint64_t pending_items, std::uint64_t pending_raw_bytes) noexcept {
    if (max_items > 0 && pending_items >= static_cast<std::uint64_t>(max_items)) return 0;
    if (max_bytes == 0) return std::numeric_limits<std::uint64_t>::max();

    const std::uint64_t limit = static_cast<std::uint64_t>(max_bytes);
    const unsigned expansion = base94 ? 2 : 1;
    const unsigned frame_overhead = expansion * TransmissionBinaryHeaderBytes +
        (base94 ? TransmissionBase94MaxHeaderBytes : 0);
    // Include the candidate's header. Divide first to avoid overflowing even
    // if the caller has accumulated writes under previously unlimited limits.
    if (pending_items >= limit / frame_overhead) return 0;
    const auto max_raw_bytes = (limit - (pending_items + 1) * frame_overhead) / expansion;
    return max_raw_bytes > pending_raw_bytes ? max_raw_bytes - pending_raw_bytes : 0;
}

// A Base94 write between its best- and worst-case bounds has to be encoded
// before its exact length is known. Classify one frame on an empty queue.
enum class TransmissionFrameFit { TooLarge, EncodingDependent, Guaranteed };

inline TransmissionFrameFit EmptyTransmissionQueueFit(bool base94, int max_bytes,
    std::uint64_t frame_bytes) noexcept {
    if (frame_bytes <= TransmissionWriteCredit(base94, 0, max_bytes, 0, 0)) {
        return TransmissionFrameFit::Guaranteed;
    }
    constexpr unsigned minimum = TransmissionBinaryHeaderBytes + TransmissionBase94MinHeaderBytes;
    if (base94 && max_bytes >= static_cast<int>(minimum) &&
        frame_bytes <= static_cast<unsigned>(max_bytes) - minimum) {
        return TransmissionFrameFit::EncodingDependent;
    }
    return TransmissionFrameFit::TooLarge;
}
}
