#include "dspico_host_shim.h"

#include <cstring>

#include "common.h"
#include "gekkopak/protocol.h"
#include "gekkopakNtr.h"
#include "ntrCardRom.h"

namespace {

// Recorded PIO transmit-FIFO traffic for the transaction in flight.
constexpr std::size_t kMaxFifoWrites = 8;
u32 gFifo[kMaxFifoWrites];
std::size_t gFifoCount;

// The most recent ntrc_dmaToBus() call.
const void* gDmaData;
u32 gDmaLength;

pio_hw_t gPio;
ntr_rom_emu_t gRomEmu;

void BeginTransaction() {
    gFifoCount = 0;
    gDmaData = nullptr;
    gDmaLength = 0;
    std::memset(&gRomEmu, 0, sizeof(gRomEmu));
}

} // namespace

namespace dspico_shim {

void RecordFifoWrite(u32 value) {
    if (gFifoCount < kMaxFifoWrites) {
        gFifo[gFifoCount] = value;
    }
    ++gFifoCount;
}

} // namespace dspico_shim

void ntrc_dmaToBus(const void* data, u32 length) {
    gDmaData = data;
    gDmaLength = length;
}

namespace dspico_shim {
namespace {

namespace gp = gekkopak::protocol;

struct HandlerPair {
    ntrc_cmd_handler_t cmd0;
    ntrc_cmd_handler_t cmd1;
};

// The same table the overlay installs into DSpico's F0-F5 dispatch slots.
HandlerPair HandlersFor(u8 opcode) {
    switch (opcode) {
    case gp::kWireWriteReg:
        return {ntrc_gekkopakWriteRegCmd0, ntrc_gekkopakWriteRegCmd1};
    case gp::kWireExec:
        return {ntrc_gekkopakExecCmd0, ntrc_gekkopakExecCmd1};
    case gp::kWireReadReg:
        return {ntrc_gekkopakReadRegCmd0, ntrc_gekkopakReadRegCmd1};
    case gp::kWireWritePayloadWord:
        return {ntrc_gekkopakPayloadWordCmd0, ntrc_gekkopakPayloadWordCmd1};
    case gp::kWireWriteBlock:
        return {ntrc_gekkopakWriteBlockCmd0, ntrc_gekkopakWriteBlockCmd1};
    case gp::kWireReadBlock:
        return {ntrc_gekkopakReadBlockCmd0, ntrc_gekkopakReadBlockCmd1};
    default:
        return {nullptr, nullptr};
    }
}

} // namespace

void Reset() {
    gekkopak_ntr_reset();
    BeginTransaction();
}

CommandResult IssueCommand(const std::uint8_t command[8], const std::uint8_t* in_data,
                           std::size_t in_bytes, std::uint8_t* out, std::size_t out_capacity) {
    CommandResult result;
    BeginTransaction();

    // The PIO shifts input left, so the first byte received lands in bits 31:24
    // of each command word. That makes both words big-endian.
    gRomEmu.cmd0 = (static_cast<u32>(command[0]) << 24) | (static_cast<u32>(command[1]) << 16) |
                   (static_cast<u32>(command[2]) << 8) | static_cast<u32>(command[3]);
    gRomEmu.cmd1 = (static_cast<u32>(command[4]) << 24) | (static_cast<u32>(command[5]) << 16) |
                   (static_cast<u32>(command[6]) << 8) | static_cast<u32>(command[7]);

    const HandlerPair handlers = HandlersFor(command[0]);
    if (handlers.cmd0 == nullptr) {
        return result; // opcode outside the GekkoPAK range: no handler installed
    }

    handlers.cmd0(&gRomEmu, gRomEmu.cmd0, &gPio);
    handlers.cmd1(&gRomEmu, gRomEmu.cmd1, &gPio);

    if (gFifoCount == 0) {
        return result; // handler declared nothing
    }

    const u32 directive = gFifo[0];
    if (directive == 0) {
        return result; // no payload
    }

    if ((directive & 0x80000000u) != 0) {
        // DS -> DSpico read payload.
        const std::size_t expected = (directive & 0x7FFFFFFFu) + 1u;
        if (in_data == nullptr || in_bytes < expected) {
            result.direction_mismatch = true;
            return result;
        }
        // ntrCardIrq.S applies `rev` to each received word before storing, so
        // bytes land in cartridge SRAM in the order the DS emitted them.
        std::memcpy(gRomEmu.readDataDestination, in_data, expected);
        if (gRomEmu.readDataCompleteHandler != nullptr) {
            gRomEmu.readDataCompleteHandler(&gRomEmu);
        }
        return result;
    }

    // DSpico -> DS write payload.
    const std::size_t declared = directive + 1u;
    if (out == nullptr || out_capacity < declared) {
        result.direction_mismatch = true;
        return result;
    }
    std::memset(out, 0, declared);

    if (gDmaData != nullptr) {
        const std::size_t copy = gDmaLength < declared ? gDmaLength : declared;
        std::memcpy(out, gDmaData, copy);
        result.response_bytes = copy;
        return result;
    }

    // Words pushed one at a time. The PIO shifts output right, LSB first, so a
    // word arrives at the DS as four little-endian bytes.
    std::size_t written = 0;
    for (std::size_t i = 1; i < gFifoCount && written + 4 <= declared; ++i) {
        const u32 word = gFifo[i];
        out[written + 0] = static_cast<std::uint8_t>(word);
        out[written + 1] = static_cast<std::uint8_t>(word >> 8);
        out[written + 2] = static_cast<std::uint8_t>(word >> 16);
        out[written + 3] = static_cast<std::uint8_t>(word >> 24);
        written += 4;
    }
    result.response_bytes = written;
    return result;
}

} // namespace dspico_shim
