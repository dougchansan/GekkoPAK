#pragma once

#include <memory>
#include "common/common_types.h"

class BackingMem;

// GekkoPAK virtual NTRCARD device.
//
// The register window is an MMIO region, not backing memory: the device has to
// observe individual reads and writes, because reading the data FIFO is what
// advances a transfer and clears DATA_READY on real hardware. The overlay adds
// an MMIO page type to Azahar's memory core for exactly this. See
// docs/AZAHAR_MMIO.md.

namespace GekkoPakNtr {

constexpr u32 PhysicalBase = 0x10164000u;
constexpr u32 VirtualBase = 0x1EC64000u;
constexpr u32 RegisterPageSize = 0x1000u;

/// Registers the NTRCARD window with Azahar's MMIO registry.
///
/// This is the only place Azahar's own sources name GekkoPAK. The memory core
/// dispatches MMIO by address and knows nothing about any particular device;
/// until this runs, no window is claimed and an IO-area mapping is refused
/// exactly as it was before the overlay. It has to be called explicitly rather
/// than run from a static initialiser, because citra_core is a static library
/// and the linker would drop a translation unit nothing references.
void Install();

/// Returns the device to a freshly powered state.
void Reset();

/// True once Reset() has run, i.e. once the window has been mapped.
bool IsMapped();

/// One 32-bit access to the register window. `offset` is relative to the base
/// of the window.
u32 Read32(u32 offset);
void Write32(u32 offset, u32 value);

} // namespace GekkoPakNtr
