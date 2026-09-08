// GekkoPAK F0-F5 handlers for DSpico's unscrambled game-mode dispatcher.
//
// This file is the DSpico transport adapter. It owns the cartridge bus and
// nothing else: PIO command decode, the read/write data phases, the 512-byte
// staging buffers, and the IRQ-path budget. Every protocol decision -- the
// staging registers, the allocator, job handles, SUBMIT/POLL/COLLECT/FREE, the
// GKD1/GKC1 block ABI and the completion queue -- lives in gekkopak::Device,
// which the Azahar core device and the host model also drive.
//
// It is compiled two ways, from this one source:
//   - into RP2040 firmware, against the real DSpico headers;
//   - on the host, against prototype/dspico-v1/hostshim, so the conformance
//     vectors can be replayed through the real handler code without hardware.

#include "gekkopakNtr.h"

#include <cstring>

#include "gekkopak/device.h"
#include "gekkopak/protocol.h"
#include "ntrCardRomGameNoScramble.h"

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

alignas(4) u8 sLocal[kLocalBytes];
// Two 512-byte staging buffers, one per direction. The write buffer is filled
// by the NTR data phase and parsed afterwards; the read buffer is handed
// straight to the bus DMA.
alignas(4) u8 sBlockTx[kBlockBytes];
alignas(4) u8 sBlockRx[kBlockBytes];

gekkopak::Device sDevice;

// F4 instrumentation.
//
// From the DS side an F4 that never arrives, one that arrives but whose payload
// is not captured, and one whose descriptor is rejected are indistinguishable:
// no completion appears and the transfer reports no error. These counters
// separate them, and are read back through F2 at indices 0xF0-0xF3.
u32 sF4Enter;    // cmd1 handler entered at all
u32 sF4Accepted; // passed the opcode/index/length checks and began a read
u32 sF4Complete; // payload fully received, completion callback fired
u32 sF4Parsed;   // descriptors accepted by the shared core

bool commandMatches(const ntr_rom_emu_t* romEmu, u8 opcode) {
    const u32 expected = (static_cast<u32>(opcode) << 24) | kCommandDiscriminator;
    return (romEmu->cmd0 & kCommandLowMask) == expected;
}

u8 commandIndex(const ntr_rom_emu_t* romEmu) {
    return static_cast<u8>(romEmu->cmd0 & 0xFFu);
}

// Called once the NTR data phase has delivered all 512 bytes. Parsing and job
// execution happen here rather than in the command handler, so the
// timing-critical path is only "start a DMA and return".
void blockWriteComplete(ntr_rom_emu_t* romEmu) {
    ++sF4Complete;
    const u32 word = romEmu->cmd1;
    if (sDevice.WriteBlock(commandIndex(romEmu), word, sBlockTx, kBlockBytes) == gp::kOk) {
        ++sF4Parsed;
    }
}

void finishCmd0(ntr_rom_emu_t* romEmu) {
    ntrc_finishGameNoScrambleCmd0(romEmu);
}

} // namespace

extern "C" void gekkopak_ntr_reset(void) {
    std::memset(sBlockTx, 0, sizeof(sBlockTx));
    std::memset(sBlockRx, 0, sizeof(sBlockRx));
    sF4Enter = sF4Accepted = sF4Complete = sF4Parsed = 0;

    gekkopak::Device::Config config;
    config.pool = sLocal;
    config.pool_bytes = kLocalBytes;
    config.reported_local_bytes = kLocalBytes;
    sDevice.Reset(config);
}

// --------------------------------------------------------------------------
// F0 WRITE_REG
// --------------------------------------------------------------------------

extern "C" void ntrc_gekkopakWriteRegCmd0(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}

extern "C" void ntrc_gekkopakWriteRegCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio) {
    ntrc_noPayload(pio);
    if (commandMatches(romEmu, gp::kWireWriteReg)) {
        sDevice.WriteReg(commandIndex(romEmu), word);
    }
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

// --------------------------------------------------------------------------
// F1 EXEC
// --------------------------------------------------------------------------

extern "C" void ntrc_gekkopakExecCmd0(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}

extern "C" void ntrc_gekkopakExecCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio) {
    ntrc_noPayload(pio);
    if (commandMatches(romEmu, gp::kWireExec)) {
        sDevice.Exec(commandIndex(romEmu), word);
    }
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

// --------------------------------------------------------------------------
// F2 READ_REG / EVENT
// --------------------------------------------------------------------------

extern "C" void ntrc_gekkopakReadRegCmd0(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}

extern "C" void ntrc_gekkopakReadRegCmd1(ntr_rom_emu_t* romEmu, u32, pio_hw_t* pio) {
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
        else
            value = sDevice.ReadReg(index);
    }
    // Hand the response to the PIO before doing anything else: the first
    // transaction after a pause is dropped if the handler is still busy when
    // the next CEB edge arrives. See docs/HARDWARE_DSPICO_V1.md.
    ntrc_beginWrite(pio, 4);
    ntrc_writeWord(pio, value);
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

// --------------------------------------------------------------------------
// F3 WRITE_PAYLOAD_WORD
// --------------------------------------------------------------------------

extern "C" void ntrc_gekkopakPayloadWordCmd0(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}

extern "C" void ntrc_gekkopakPayloadWordCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio) {
    ntrc_noPayload(pio);
    if (commandMatches(romEmu, gp::kWireWritePayloadWord)) {
        sDevice.WritePayloadWord(commandIndex(romEmu), word);
    }
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

// --------------------------------------------------------------------------
// F4 WRITE_BLOCK -- 512 bytes console -> cartridge
// --------------------------------------------------------------------------

extern "C" void ntrc_gekkopakWriteBlockCmd0(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}

extern "C" void ntrc_gekkopakWriteBlockCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio) {
    ++sF4Enter;
    const u32 meaningful = word & 0xFFFFu;
    if (!commandMatches(romEmu, gp::kWireWriteBlock) ||
        commandIndex(romEmu) != gp::kSelectorSubmission || meaningful == 0 ||
        meaningful > kBlockBytes) {
        // The data phase has to be refused here -- the cartridge cannot start a
        // 512-byte read it has nowhere to put -- but the rejection still has to
        // reach the RESULT register, or a client cannot tell a refused block
        // from an accepted one. Route it through the core so the status comes
        // from the same validation every other transport uses.
        if (commandMatches(romEmu, gp::kWireWriteBlock)) {
            sDevice.WriteBlock(commandIndex(romEmu), word, nullptr, 0);
        }
        ntrc_noPayload(pio);
        ntrc_finishGameNoScrambleCmd1(romEmu);
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

extern "C" void ntrc_gekkopakReadBlockCmd0(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}

extern "C" void ntrc_gekkopakReadBlockCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio) {
    if (commandMatches(romEmu, gp::kWireReadBlock)) {
        sDevice.ReadBlock(commandIndex(romEmu), word, sBlockRx, kBlockBytes);
    } else {
        std::memset(sBlockRx, 0, sizeof(sBlockRx));
    }
    // The data phase is always a full 512 bytes so cmd0 can start the transfer
    // without waiting for a length field.
    ntrc_beginWrite(pio, kBlockBytes);
    ntrc_dmaToBus(sBlockRx, kBlockBytes);
    ntrc_finishGameNoScrambleCmd1(romEmu);
}
