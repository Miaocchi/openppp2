#pragma once

#include <ppp/stdafx.h>

namespace ppp::transmissions {
// Wire-read limit, not a decoded payload or secure record limit. The byte-wise
// base94 codec can double a binary frame; allow room for its framing overhead.
inline constexpr int MaxEncodedTransmissionFrame = 2 * PPP_BUFFER_SIZE + 64;
}
