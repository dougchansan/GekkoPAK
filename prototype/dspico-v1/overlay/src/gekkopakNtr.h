#pragma once

#include "common.h"
#include "ntrCardRom.h"

#ifdef __cplusplus
extern "C" {
#endif

// F0-F5 handlers installed into DSpico's unscrambled game-mode dispatch tables.
void ntrc_gekkopakWriteRegCmd0(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);
void ntrc_gekkopakWriteRegCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);
void ntrc_gekkopakExecCmd0(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);
void ntrc_gekkopakExecCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);
void ntrc_gekkopakReadRegCmd0(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);
void ntrc_gekkopakReadRegCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);
void ntrc_gekkopakPayloadWordCmd0(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);
void ntrc_gekkopakPayloadWordCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);
void ntrc_gekkopakWriteBlockCmd0(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);
void ntrc_gekkopakWriteBlockCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);
void ntrc_gekkopakReadBlockCmd0(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);
void ntrc_gekkopakReadBlockCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio);

void gekkopak_ntr_reset(void);

// --------------------------------------------------------------------------
// Introspection
// --------------------------------------------------------------------------
//
// Read by the host link (gekkopakLink.cpp) so that a run can be inspected from
// a PC rather than photographed off the DS screen. These are plain accessors
// over state the handlers already keep: nothing here is on the IRQ path, and
// nothing here may be allowed to become a second source of truth for it.

// Buffers to dump. Selected by name rather than by pointer so the link cannot
// be handed an address the adapter did not intend to expose.
enum {
    GEKKOPAK_BUFFER_STAGED = 0,  // the completion block an F5 would send now
    GEKKOPAK_BUFFER_DIAG   = 1,  // the deterministic data-phase pattern
    GEKKOPAK_BUFFER_LAST_IN = 2  // the last 512 bytes an F4 delivered
};

// Returns a pointer to `which`, or null if it is not a known buffer. Always
// 512 bytes.
const u8* gekkopak_ntr_buffer(u32 which);

typedef struct {
    u32 f4_enter, f4_accepted, f4_complete, f4_parsed;
    u32 f5_sent;
    u32 staged_bytes;   // meaningful bytes in the staged completion block
    u32 stage_index;    // which of the two staging buffers is live
    u32 event_depth;    // completions the device has queued
    u32 local_bytes;    // device-local pool size
    u32 result;         // the device's RESULT register
} gekkopak_ntr_state_t;

void gekkopak_ntr_state(gekkopak_ntr_state_t* out);

#ifdef __cplusplus
}
#endif
