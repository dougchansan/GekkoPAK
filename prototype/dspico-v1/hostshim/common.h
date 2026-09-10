// Host stand-in for DSpico's src/common.h.
//
// Only the fixed-width typedefs the GekkoPAK overlay uses. The real header also
// pulls in the Pico SDK and the board pin map, neither of which exists on the
// host and neither of which the overlay touches.
//
// Upstream: LNH-team/dspico-firmware @ 472c9d8e9957ad18df367f14b9cc337b9b887e65

#pragma once

#include <cstdint>

// Tells the GekkoPAK overlay it is being built for the host, where the Pico
// SDK's scratch-RAM placement attributes do not exist.
#define GEKKOPAK_HOST_SHIM 1

typedef std::uint8_t u8;
typedef std::int8_t s8;
typedef std::uint16_t u16;
typedef std::int16_t s16;
typedef std::uint32_t u32;
typedef std::int32_t s32;
typedef std::uint64_t u64;
typedef std::int64_t s64;
