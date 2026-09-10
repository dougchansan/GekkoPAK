// Neutralised remains of DSpico's DS-side USB proxy.
//
// Upstream hands the RP2040's USB device controller to the console over card
// commands E8-EB, so the ARM9 can run a USB stack across the cartridge bus.
// GekkoPAK's host link owns that same controller from the cartridge side
// instead (gekkopakLink.cpp), and there is only one of them, so the two cannot
// both exist in a build.
//
// The dispatch tables in ntrCardIrq.S are upstream's and are left alone -- an
// overlay that rewrites a hand-written assembly jump table is an overlay that
// stops applying the next time upstream touches it. So the entries stay and
// point here, where each command is answered correctly and does nothing.
//
// "Answered correctly" is the part that matters. A console that issues E8-EB
// against this firmware gets a well-formed refusal: every command still
// completes, and the one that reads 512 bytes still drives 512 bytes, because
// the console clocks the data phase whether or not the cartridge has anything
// to say. Leaving these unimplemented would hang the bus instead.

#include "common.h"

#include "gekkopakTrace.h"
#include "ntrCardRom.h"
#include "ntrCardRomGameNoScramble.h"

namespace {

constexpr u32 kUsbBlockBytes = 512;

// Driven when the console asks to read an endpoint buffer that no longer
// exists. Const, so it costs flash rather than RAM.
alignas(4) const u8 kZeroes[kUsbBlockBytes] = {};

} // namespace

extern "C" {

void GEKKOPAK_IRQ_FN(ntrc_gameReqUsbCommandCmd1)(ntr_rom_emu_t* romEmu, u32, pio_hw_t* pio) {
    ntrc_noPayload(pio);
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

void GEKKOPAK_IRQ_FN(ntrc_gameWriteUsbDataCmd0)(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    ntrc_finishGameNoScrambleCmd0(romEmu);
}

void GEKKOPAK_IRQ_FN(ntrc_gameWriteUsbDataCmd1)(ntr_rom_emu_t* romEmu, u32, pio_hw_t* pio) {
    // Upstream reads a 512-byte payload here. Refusing the data phase is the
    // correct answer now that there is nowhere for it to go, and it is what the
    // F4 handler does for a block it will not accept.
    ntrc_noPayload(pio);
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

void GEKKOPAK_IRQ_FN(ntrc_gameReadUsbDataCmd0)(ntr_rom_emu_t* romEmu, u32, pio_hw_t* pio) {
    // Armed in cmd0 from a buffer that already exists -- the same rule the F5
    // handler follows, and for the same reason.
    ntrc_beginWrite(pio, kUsbBlockBytes);
    ntrc_dmaToBus(kZeroes, kUsbBlockBytes);
    ntrc_finishGameNoScrambleCmd0(romEmu);
}

void GEKKOPAK_IRQ_FN(ntrc_gameReadUsbDataCmd1)(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

void GEKKOPAK_IRQ_FN(ntrc_gameUsbGetEventCmd0)(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    ntrc_finishGameNoScrambleCmd0(romEmu);
}

void GEKKOPAK_IRQ_FN(ntrc_gameUsbGetEventCmd1)(ntr_rom_emu_t* romEmu, u32, pio_hw_t* pio) {
    ntrc_beginWrite(pio, 4);
    // USB_EVENT_NONE. A console polling the queue sees it as permanently empty,
    // which is true.
    ntrc_writeWord(pio, 0);
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

// Called from resetNtrCard() on every DS reset, which is a rising edge on the
// cartridge RST line -- so, on every console reboot.
//
// Upstream tears the USB controller down here, because the console owned it and
// the console has just restarted. The host link is not the console's, and
// surviving a reboot is most of its value: a transcript that ends when the DS
// resets cannot show a cold start, which is exactly the case the repeated-pass
// requirement is about. So this does nothing.
void ntrc_resetUsb(void) {}

} // extern "C"
