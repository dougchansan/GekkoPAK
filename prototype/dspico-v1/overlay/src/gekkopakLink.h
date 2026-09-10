// Host link: a serial port on the cartridge's own USB connector.
//
// DSpico already has a USB path, but it runs the wrong way for this purpose:
// upstream exposes the RP2040's device controller *to the DS* over card
// commands E8-EB, so the console drives USB across the cartridge bus. That is
// the right design for a flashcart serving disk sectors, and the wrong one for
// measuring a cartridge protocol -- the results channel and the thing being
// measured would be the same wire. It cannot observe a run in progress, and it
// has to be off entirely during a timing campaign.
//
// This owns the USB peripheral from the cartridge side instead. It is off the
// NTR bus completely, so it costs the transport nothing and stays up while the
// console runs, reboots, or hangs.
//
// The two are mutually exclusive -- there is one device controller -- so a
// build with GEKKOPAK_HOST_LINK drops upstream's proxy. See
// docs/HARDWARE_DSPICO_V1.md.
//
// ---------------------------------------------------------------------------
// Where this runs
// ---------------------------------------------------------------------------
//
// On core0, from the main loop, at thread priority. Not on core1, which runs
// the scrambler ring generator with all interrupts masked; and not in any
// interrupt of its own beyond the USB controller's, which upstream already
// pins at 0x80 against the cartridge IRQ's 0x40. So the card interrupt
// preempts USB, never the other way round -- the ordering upstream's own proxy
// already depends on.

#pragma once

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the USB device controller and enumerates as a CDC serial port.
// Call once, after the card transport is initialised.
void gekkopak_link_init(void);

// Services USB and moves bytes in both directions. Call from the main loop.
// Returns quickly when nothing is connected.
//
// Must not be called from an interrupt, and must not be called between a
// command handler arming a data phase and the console finishing it.
void gekkopak_link_task(void);

#ifdef __cplusplus
}
#endif
