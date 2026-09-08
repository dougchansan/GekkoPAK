// Host harness for the DSpico GekkoPAK overlay.
//
// Drives prototype/dspico-v1/overlay/src/gekkopakNtr.cpp -- the same file that
// is copied into RP2040 firmware -- from an ordinary host process, so the
// golden conformance vectors can be replayed through the real handler code
// without a console, a cartridge or a toolchain.
//
// What it models faithfully:
//   - the cmd0 / cmd1 two-word dispatch,
//   - the PIO transmit-FIFO directive that selects the data-phase direction
//     and length, followed by the data words,
//   - the DS -> DSpico read payload and its completion callback,
//   - the DSpico -> DS DMA response,
//   - the wire byte order: cmd0 and cmd1 arrive big-endian from the PIO's
//     left-shifting input, and payload bytes are byte-transparent.
//
// What it does not model: timing, the IRQ, scrambling, and the dropped-first-
// transaction-after-a-pause behaviour recorded in docs/HARDWARE_DSPICO_V1.md.
// Those are properties of the silicon, not of the protocol.

#pragma once

#include <cstddef>
#include <cstdint>

namespace dspico_shim {

// Resets the overlay's device state. Equivalent to DSpico's resetNtrCard().
void Reset();

struct CommandResult {
    // Bytes the cartridge drove back onto the bus (F2: 4, F5: 512, else 0).
    std::size_t response_bytes = 0;
    // True when the handler declared a data phase whose direction did not match
    // what the caller supplied.
    bool direction_mismatch = false;
    // True when the handler declared a data phase of one length and then
    // supplied a different number of bytes for it.
    //
    // On the RP2040 this desynchronises the bus rather than failing cleanly:
    // the console clocks out exactly as many words as the directive promised,
    // so a short handler leaves the console reading undriven data and a long
    // one leaves words stranded in the FIFO for the *next* transaction to
    // return first. Either way the payload arrives shifted, which is what an
    // F5 record appearing at a non-zero byte offset looks like.
    bool payload_length_mismatch = false;
    // What the directive promised and what the handler actually supplied,
    // for diagnostics.
    std::size_t declared_bytes = 0;
    std::size_t supplied_bytes = 0;
};

// Issues one complete NTR transaction.
//
// `command` is the 8-byte wire command in canonical big-endian form.
// `in_data`/`in_bytes` is the console -> cartridge data phase (F4); pass
// nullptr/0 when the command has none. `out`/`out_capacity` receives the
// cartridge -> console data phase (F2, F5).
CommandResult IssueCommand(const std::uint8_t command[8], const std::uint8_t* in_data,
                           std::size_t in_bytes, std::uint8_t* out, std::size_t out_capacity);

} // namespace dspico_shim
