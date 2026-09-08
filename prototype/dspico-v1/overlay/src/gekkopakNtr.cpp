// GekkoPAK F0-F5 handlers for DSpico's unscrambled game-mode dispatcher.
//
// This file is the DSpico transport adapter. It owns the cartridge bus and
// nothing else: PIO command decode, the read/data phases, the 512-byte staging
// buffers, and the IRQ-path budget. Every protocol decision lives in
// gekkopak::Device, which the Azahar core device and the host model also drive.
//
// It is compiled two ways, from this one source:
//   - into RP2040 firmware, against the real DSpico headers;
//   - on the host, against prototype/dspico-v1/hostshim, so the conformance
//     vectors can be replayed through the real handler code without hardware.
//
// ---------------------------------------------------------------------------
// The cartridge IRQ budget
// ---------------------------------------------------------------------------
//
// Two rules govern everything below, and breaking either of them produced the
// F5 readback defect recorded in docs/HARDWARE_DSPICO_V1.md.
//
// 1. Handlers live in scratch RAM. Every one of DSpico's own 22 cartridge IRQ
//    handlers is marked __scratch_y("cpu0"); ours were not, so they executed
//    from XIP flash. A flash fetch on a cold cache costs far more than the
//    ~4.8 us a 32-bit word takes at the 6.7 MHz card clock, and the console
//    does not wait.
//
// 2. The PIO is armed first, and the data it will send is prepared in advance.
//    ntrc_beginWrite() tells the state machine how long the data phase is; if
//    that has not happened by the time the console starts clocking, the opening
//    words are lost and the payload arrives shifted. Every DSpico handler that
//    drives 512 bytes to the console arms before doing anything else, and DMAs
//    from a buffer some earlier, non-critical code already filled.
//
// So the F5 completion block is staged outside the IRQ path and the handler
// only starts a transfer that is already ready to go.

#include "gekkopakNtr.h"

#include <cstring>

#include "gekkopak/device.h"
#include "gekkopak/protocol.h"
#include "ntrCardRomGameNoScramble.h"

// RAM placement for the cartridge IRQ path.
//
// These handlers must not execute from XIP flash: a fetch on a cold cache costs
// far more than the ~4.8 us a 32-bit word takes at the 6.7 MHz card clock, and
// the console does not wait.
//
// They go in .time_critical, which the Pico linker script puts in main RAM --
// *not* in a scratch bank. SCRATCH_Y looks like the obvious home, and DSpico's
// own handlers live there, but core0's stack shares that same 4 KiB region:
//
//     .scratch_y      -> SCRATCH_Y   (0x20041000, 4 KiB)
//     .stack_dummy    -> SCRATCH_Y   __StackBottom = 0x20041a00
//
// and the only ASSERT in memmap_default.ld guards RAM, not this. Filling
// SCRATCH_Y therefore links cleanly and then lets the stack quietly overwrite
// whatever code sits above __StackBottom. That is not a theoretical hazard: it
// is what happened, and it killed every GekkoPAK handler on hardware while
// DSpico's own B8 handler -- which happened to land lower -- kept answering.
//
// Main RAM has ~40 KiB spare here and no such overlap, so that is where these
// belong.

#ifdef GEKKOPAK_HOST_SHIM
#define GEKKOPAK_IRQ_FN(name) name
#else
#define GEKKOPAK_IRQ_FN(name) __not_in_flash_func(name)
#endif

namespace {

namespace gp = gekkopak::protocol;

// Device-local memory. 64 KiB of an RP2040's 264 KiB; the firmware reports what
// it actually has rather than the emulator's 32 MiB target.
#ifndef GEKKOPAK_LOCAL_BYTES
#define GEKKOPAK_LOCAL_BYTES (64u * 1024u)
#endif
constexpr u32 kLocalBytes = GEKKOPAK_LOCAL_BYTES;

constexpr u32 kCommandDiscriminator = 0x00474B00u;
constexpr u32 kCommandLowMask = 0xFFFFFF00u;
constexpr std::size_t kBlockBytes = gp::kBlockBytes;

// F5 selector reserved for the raw data-phase diagnostic. It returns a fixed
// pattern with no allocator, job queue or completion queue involved, so a
// hardware failure can be attributed to the transport rather than the protocol.
constexpr u8 kSelectorDiagnostic = 0x7E;

alignas(4) u8 sLocal[kLocalBytes];
// Console -> cartridge staging, filled by the NTR data phase.
alignas(4) u8 sBlockTx[kBlockBytes];
// Cartridge -> console staging, filled *before* a transfer is armed.
//
// Double-buffered, like DSpico's own SD read path. The DMA streams from one
// buffer for as long as the console keeps clocking, which outlasts the cmd1
// handler; restaging into the same buffer would rewrite it mid-transfer.
alignas(4) u8 sBlockRx[2][kBlockBytes];
u32 sStageIndex;
// The diagnostic pattern, built once at reset.
alignas(4) u8 sBlockDiag[kBlockBytes];
// Driven when the console asks for a window that does not exist. The bus is
// going to be clocked either way, so it gets zeros rather than stale data.
alignas(4) const u8 sBlockZeroes[kBlockBytes] = {};

gekkopak::Device sDevice;

// How many meaningful bytes the staged completion block holds; the records
// behind them are dropped once the console has been sent them.
u32 sStagedBytes;

// F4/F5 instrumentation.
//
// From the DS side a transfer that never arrives, one that arrives but whose
// payload is not captured, and one whose descriptor is rejected are
// indistinguishable: nothing appears and no error is reported. These counters
// separate them, and are read back through F2 at indices 0xF0-0xF4.
u32 sF4Enter;    // F4 cmd1 handler entered at all
u32 sF4Accepted; // passed the opcode/index/length checks and began a read
u32 sF4Complete; // payload fully received, completion callback fired
u32 sF4Parsed;   // descriptors accepted by the shared core
u32 sF5Sent;     // F5 data phases armed

bool GEKKOPAK_IRQ_FN(commandMatches)(const ntr_rom_emu_t* romEmu, u8 opcode) {
    const u32 expected = (static_cast<u32>(opcode) << 24) | kCommandDiscriminator;
    return (romEmu->cmd0 & kCommandLowMask) == expected;
}

u8 GEKKOPAK_IRQ_FN(commandIndex)(const ntr_rom_emu_t* romEmu) {
    return static_cast<u8>(romEmu->cmd0 & 0xFFu);
}

// Builds the deterministic diagnostic block. Each 32-bit word encodes its own
// index twice, in two different transformations, under a constant tag:
//
//     word[i] = 0xF5 << 24 | i << 16 | (i ^ 0x7F) << 8 | (i + 0xA5)
//
// so a dump of the first bytes makes word order, byte order, a repeated FIFO
// word, a stale buffer, truncation and any starting offset each look different
// from the others.
void buildDiagnosticBlock() {
    for (u32 i = 0; i < kBlockBytes / 4u; ++i) {
        const u32 word = (0xF5u << 24) | (i << 16) | ((i ^ 0x7Fu) << 8) | ((i + 0xA5u) & 0xFFu);
        std::memcpy(sBlockDiag + i * 4u, &word, sizeof(word));
    }
}

// Fills sBlockRx with the completion records currently queued. Runs outside the
// timing-critical window: at reset, after an F4 batch is parsed, and after an
// F5 transfer has been sent.
void stageCompletionBlock() {
    // A peek, not a read: the records stay queued until the console has
    // actually been sent them, so an F2 event-depth check still sees them.
    sStagedBytes = static_cast<u32>(
        sDevice.PeekBlock(gp::kSelectorCompletion,
                          gp::EncodeReadBlockWord(0, static_cast<u16>(kBlockBytes)),
                          sBlockRx[sStageIndex], kBlockBytes));
}

// Stages into the buffer that is not currently being sent, then flips.
void restageCompletionBlock() {
    sStageIndex = 1u - sStageIndex;
    stageCompletionBlock();
}

// Called once the NTR data phase has delivered all 512 bytes. Parsing and job
// execution happen here rather than in the command handler, so the
// timing-critical path is only "start a DMA and return".
void GEKKOPAK_IRQ_FN(blockWriteComplete)(ntr_rom_emu_t* romEmu) {
    ++sF4Complete;
    const u32 word = romEmu->cmd1;
    if (sDevice.WriteBlock(commandIndex(romEmu), word, sBlockTx, kBlockBytes) == gp::kOk) {
        ++sF4Parsed;
    }
    // A completion is now queued; have it ready before the console asks.
    stageCompletionBlock();
}

void GEKKOPAK_IRQ_FN(finishCmd0)(ntr_rom_emu_t* romEmu) {
    ntrc_finishGameNoScrambleCmd0(romEmu);
}

} // namespace

extern "C" void gekkopak_ntr_reset(void) {
    std::memset(sBlockTx, 0, sizeof(sBlockTx));
    std::memset(sBlockRx, 0, sizeof(sBlockRx));
    sStageIndex = 0;
    sF4Enter = sF4Accepted = sF4Complete = sF4Parsed = sF5Sent = 0;
    sStagedBytes = 0;

    buildDiagnosticBlock();

    gekkopak::Device::Config config;
    config.pool = sLocal;
    config.pool_bytes = kLocalBytes;
    config.reported_local_bytes = kLocalBytes;
    sDevice.Reset(config);
}

// --------------------------------------------------------------------------
// F0 WRITE_REG
// --------------------------------------------------------------------------

extern "C" void GEKKOPAK_IRQ_FN(ntrc_gekkopakWriteRegCmd0)(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}

extern "C" void GEKKOPAK_IRQ_FN(ntrc_gekkopakWriteRegCmd1)(ntr_rom_emu_t* romEmu, u32 word,
                                                       pio_hw_t* pio) {
    // Release the bus before touching the device: the state machine must be
    // advanced whatever the command turns out to be.
    ntrc_noPayload(pio);
    ntrc_finishGameNoScrambleCmd1(romEmu);
    if (commandMatches(romEmu, gp::kWireWriteReg)) {
        sDevice.WriteReg(commandIndex(romEmu), word);
    }
}

// --------------------------------------------------------------------------
// F1 EXEC
// --------------------------------------------------------------------------

extern "C" void GEKKOPAK_IRQ_FN(ntrc_gekkopakExecCmd0)(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}

extern "C" void GEKKOPAK_IRQ_FN(ntrc_gekkopakExecCmd1)(ntr_rom_emu_t* romEmu, u32 word,
                                                   pio_hw_t* pio) {
    ntrc_noPayload(pio);
    ntrc_finishGameNoScrambleCmd1(romEmu);
    if (commandMatches(romEmu, gp::kWireExec)) {
        sDevice.Exec(commandIndex(romEmu), word);
    }
}

// --------------------------------------------------------------------------
// F2 READ_REG / EVENT
// --------------------------------------------------------------------------

extern "C" void GEKKOPAK_IRQ_FN(ntrc_gekkopakReadRegCmd0)(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}

extern "C" void GEKKOPAK_IRQ_FN(ntrc_gekkopakReadRegCmd1)(ntr_rom_emu_t* romEmu, u32, pio_hw_t* pio) {
    u32 value = 0;
    if (commandMatches(romEmu, gp::kWireReadReg)) {
        const u8 index = commandIndex(romEmu);
        // Firmware-local diagnostic window, outside the protocol.
        if (index == 0xF0)
            value = sF4Enter;
        else if (index == 0xF1)
            value = sF4Accepted;
        else if (index == 0xF2)
            value = sF4Complete;
        else if (index == 0xF3)
            value = sF4Parsed;
        else if (index == 0xF4)
            value = sF5Sent;
        else
            value = sDevice.ReadReg(index);
    }
    // Four bytes has enough slack to be armed from cmd1, and enough to tolerate
    // running from flash: F2 register reads were measured good on hardware
    // (31/32 at latency 4, 32/32 at 8 and above) while the 512-byte F5 in the
    // same firmware failed every time. So this one pays for the scratch-RAM
    // budget that the block handlers need.
    ntrc_beginWrite(pio, 4);
    ntrc_writeWord(pio, value);
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

// --------------------------------------------------------------------------
// F3 WRITE_PAYLOAD_WORD
// --------------------------------------------------------------------------

extern "C" void GEKKOPAK_IRQ_FN(ntrc_gekkopakPayloadWordCmd0)(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}

extern "C" void GEKKOPAK_IRQ_FN(ntrc_gekkopakPayloadWordCmd1)(ntr_rom_emu_t* romEmu, u32 word,
                                                          pio_hw_t* pio) {
    ntrc_noPayload(pio);
    ntrc_finishGameNoScrambleCmd1(romEmu);
    if (commandMatches(romEmu, gp::kWireWritePayloadWord)) {
        sDevice.WritePayloadWord(commandIndex(romEmu), word);
    }
}

// --------------------------------------------------------------------------
// F4 WRITE_BLOCK -- 512 bytes console -> cartridge
// --------------------------------------------------------------------------

extern "C" void GEKKOPAK_IRQ_FN(ntrc_gekkopakWriteBlockCmd0)(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}

extern "C" void GEKKOPAK_IRQ_FN(ntrc_gekkopakWriteBlockCmd1)(ntr_rom_emu_t* romEmu, u32 word,
                                                         pio_hw_t* pio) {
    ++sF4Enter;
    const u32 meaningful = word & 0xFFFFu;
    if (!commandMatches(romEmu, gp::kWireWriteBlock) ||
        commandIndex(romEmu) != gp::kSelectorSubmission || meaningful == 0 ||
        meaningful > kBlockBytes) {
        // The data phase has to be refused here -- the cartridge cannot start a
        // 512-byte read it has nowhere to put -- but the rejection still has to
        // reach the RESULT register, or a client cannot tell a refused block
        // from an accepted one.
        ntrc_noPayload(pio);
        ntrc_finishGameNoScrambleCmd1(romEmu);
        if (commandMatches(romEmu, gp::kWireWriteBlock)) {
            sDevice.WriteBlock(commandIndex(romEmu), word, nullptr, 0);
        }
        return;
    }
    ++sF4Accepted;
    // Start the data phase and return. The payload is parsed in
    // blockWriteComplete(), off the cartridge IRQ-critical path.
    ntrc_beginRead(pio, kBlockBytes);
    ntrc_finishGameNoScrambleCmd1WithReadPayload(
        romEmu, reinterpret_cast<u32*>(sBlockTx), kBlockBytes, blockWriteComplete);
}

// --------------------------------------------------------------------------
// F5 READ_BLOCK -- 512 bytes cartridge -> console
// --------------------------------------------------------------------------
//
// Armed in cmd0, from a buffer that is already staged. This is the shape every
// working DSpico cartridge-to-console path uses, and the reason the previous
// implementation failed on hardware: it built the block and armed the PIO in
// cmd1, by which point the console had already begun clocking the data phase.

extern "C" void GEKKOPAK_IRQ_FN(ntrc_gekkopakReadBlockCmd0)(ntr_rom_emu_t* romEmu, u32,
                                                        pio_hw_t* pio) {
    // Arm first, unconditionally. The console programmed a 512-byte data phase
    // and is going to clock it whatever we decide about the command.
    ntrc_beginWrite(pio, kBlockBytes);

    // The selector lives in the low byte of cmd0, so the source can be chosen
    // here without waiting for the value word.
    const u8 selector = commandIndex(romEmu);
    const u8* source = sBlockZeroes;
    if (commandMatches(romEmu, gp::kWireReadBlock)) {
        if (selector == kSelectorDiagnostic) {
            source = sBlockDiag;
        } else if (selector == gp::kSelectorCompletion) {
            source = sBlockRx[sStageIndex];
        }
    }
    ntrc_dmaToBus(source, kBlockBytes);
    ++sF5Sent;
    finishCmd0(romEmu);
}

extern "C" void GEKKOPAK_IRQ_FN(ntrc_gekkopakReadBlockCmd1)(ntr_rom_emu_t* romEmu, u32 word,
                                                        pio_hw_t* pio) {
    (void)pio;
    ntrc_finishGameNoScrambleCmd1(romEmu);

    // Bookkeeping, after the transfer is under way.
    if (!commandMatches(romEmu, gp::kWireReadBlock)) {
        return;
    }
    const u8 selector = commandIndex(romEmu);
    if (selector == kSelectorDiagnostic) {
        // The diagnostic window is stateless and outside the protocol, so it
        // consumes nothing and reports nothing.
        return;
    }
    // The data phase had to be armed before the value word arrived, so the
    // verdict on the command is delivered now.
    if (sDevice.ValidateReadBlock(selector, word) == gp::kOk) {
        sDevice.DropCompletions(sStagedBytes / sizeof(gp::CompletionRecordV1));
        restageCompletionBlock();
    }
}
