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

#ifdef __cplusplus
}
#endif
