// Host stand-in for DSpico's src/ntrCardRomGameNoScramble.h.
//
// Copied verbatim from upstream (minus the C linkage block, which the host
// build does not need): these three inlines are the whole contract between a
// command handler and the assembly dispatcher.
//
// Upstream: LNH-team/dspico-firmware @ 472c9d8e9957ad18df367f14b9cc337b9b887e65

#pragma once

#include "common.h"
#include "ntrCardRom.h"

/// Finishes handling the first command word in unscrambled game mode.
static inline void ntrc_finishGameNoScrambleCmd0(ntr_rom_emu_t* romEmu) {
    romEmu->wordIdx = 1;
}

/// Finishes handling the second command word in unscrambled game mode.
static inline void ntrc_finishGameNoScrambleCmd1(ntr_rom_emu_t* romEmu) {
    romEmu->wordIdx = 0;
}

/// Finishes handling the second command word with a read payload (DS -> DSpico).
static inline void ntrc_finishGameNoScrambleCmd1WithReadPayload(
    ntr_rom_emu_t* romEmu, u32* payloadDestination, u32 payloadLength,
    ntrc_read_data_complete_handler_t payloadCompleteCallback) {
    romEmu->readDataDestination = payloadDestination;
    romEmu->readDataLimit = (payloadLength >> 2) + 1;
    romEmu->readDataCompleteHandler = payloadCompleteCallback;
    romEmu->wordIdx = 2;
}
