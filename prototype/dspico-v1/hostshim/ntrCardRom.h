// Host stand-in for DSpico's src/ntrCardRom.h.
//
// Reproduces exactly the subset the GekkoPAK overlay depends on: the ROM
// emulator context, the PIO data-phase primitives, and the DMA-to-bus call.
// The field layout and the primitive semantics are copied from upstream so the
// overlay compiles unmodified against both this and the real header.
//
// Upstream: LNH-team/dspico-firmware @ 472c9d8e9957ad18df367f14b9cc337b9b887e65
//
// The one deliberate difference is pio_hw_t::txf. On the RP2040 that is a
// memory-mapped FIFO register; here it is a recording proxy, so the harness can
// see the directive word and the data words the handler pushed and reconstruct
// what the DS would have received.

#pragma once

#include "common.h"

#include <cstddef>

namespace dspico_shim {

// One recorded write to the PIO transmit FIFO.
void RecordFifoWrite(u32 value);

struct FifoSlot {
    void operator=(u32 value) { RecordFifoWrite(value); }
};

} // namespace dspico_shim

typedef struct pio_hw_t {
    dspico_shim::FifoSlot txf[4];
} pio_hw_t;

struct ntr_rom_emu_t;

typedef void (*ntrc_cmd_handler_t)(struct ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);
typedef void (*ntrc_read_data_complete_handler_t)(struct ntr_rom_emu_t* romEmu);

typedef struct ntr_rom_emu_t {
    int wordIdx;
    u32 cmd0;
    u32 cmd1;
    u32* readDataDestination;
    u32 readDataLimit;
    ntrc_read_data_complete_handler_t readDataCompleteHandler;
} ntr_rom_emu_t;

// Sends `data` to the cartridge bus by DMA. `length` must be a multiple of 4.
void ntrc_dmaToBus(const void* data, u32 length);

// The current command has no payload.
static inline void ntrc_noPayload(pio_hw_t* pio) {
    pio->txf[0] = 0;
}

// The current command has a DSpico -> DS payload of `bytes` bytes.
static inline void ntrc_beginWrite(pio_hw_t* pio, int bytes) {
    pio->txf[0] = static_cast<u32>(bytes - 1);
}

// The current command has a DS -> DSpico payload of `bytes` bytes.
static inline void ntrc_beginRead(pio_hw_t* pio, int bytes) {
    pio->txf[0] = 0x80000000u | static_cast<u32>(bytes - 1);
}

// Queue a word to be sent to the DS.
static inline void ntrc_writeWord(pio_hw_t* pio, u32 word) {
    pio->txf[0] = word;
}
