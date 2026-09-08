// Register-page NTR transport, shared by the host model and the Azahar core
// device.
//
// Both emulate the same thing: a 4 KiB NTRCARD register page in which the guest
// stages an 8-byte command, raises the ROMCNT activate bit, and reads the
// response back out of the data FIFO. Keeping that decode in one header is what
// makes "the host model and the Azahar device model behave identically" a
// property of the code rather than a claim about two copies.
//
// Freestanding, like the rest of the core: the caller owns the page, whether it
// is a std::array or an Azahar BackingMem.

#ifndef GEKKOPAK_NTR_REGISTER_TRANSPORT_H
#define GEKKOPAK_NTR_REGISTER_TRANSPORT_H

#include <cstring>

#include "device.h"
#include "protocol.h"

namespace gekkopak {
namespace ntr_transport {

// NTRCARD register page, as mapped for GekkoPAK development.
inline constexpr std::uint32_t kPhysicalBase = 0x10164000u;
inline constexpr std::uint32_t kVirtualBase = 0x1EC64000u;
inline constexpr std::size_t kRegisterPageSize = 0x1000;

inline constexpr std::size_t kRegRomCnt = 0x04;
inline constexpr std::size_t kRegCommand = 0x08;
inline constexpr std::size_t kRegFifo = 0x1C;

// Block staging windows.
//
// These are an emulator convenience and not hardware: on a real cartridge the
// 512-byte F4/F5 payload crosses the bus as an NTR data phase, driven by the
// block-size field of ROMCNT. Modelling that faithfully would mean emulating
// the FIFO word by word; carrying the payload in the register page keeps the
// protocol semantics exact while leaving the bus transfer unmodelled. See
// docs/CONFORMANCE_RESULTS.md.
inline constexpr std::size_t kRegBlockTx = 0x100;
inline constexpr std::size_t kRegBlockRx = 0x300;

inline constexpr std::uint32_t kCardActivate = 1u << 31;
inline constexpr std::uint32_t kCardResetHigh = 1u << 29;
inline constexpr std::uint32_t kCardDataReady = 1u << 23;
inline constexpr std::uint32_t kCardBlock4 = 7u << 24;

inline std::uint32_t Load32(const std::uint8_t* regs, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, regs + offset, sizeof(value));
    return value;
}

inline void Store32(std::uint8_t* regs, std::size_t offset, std::uint32_t value) {
    std::memcpy(regs + offset, &value, sizeof(value));
}

// Consumes one staged command if the activate bit is set. Returns true when a
// command was serviced, whether or not it was a valid GekkoPAK command.
inline bool Tick(Device& device, std::uint8_t* regs) {
    const std::uint32_t romcnt = Load32(regs, kRegRomCnt);
    if ((romcnt & kCardActivate) == 0) {
        return false;
    }

    std::uint8_t bytes[protocol::kCommandBytes];
    std::memcpy(bytes, regs + kRegCommand, sizeof(bytes));
    Store32(regs, kRegRomCnt, romcnt & ~(kCardActivate | kCardDataReady));

    device.count_transfer();
    const protocol::WireCommand cmd = protocol::DecodeCommand(bytes);
    if (!cmd.valid) {
        // Not GekkoPAK traffic. A real cartridge would let another command
        // family claim it; here it is simply dropped.
        return true;
    }

    switch (cmd.opcode) {
    case protocol::kWireWriteReg:
        device.WriteReg(cmd.index, cmd.word);
        break;
    case protocol::kWireExec:
        device.Exec(cmd.index, cmd.word);
        break;
    case protocol::kWireReadReg:
        Store32(regs, kRegFifo, device.ReadReg(cmd.index));
        Store32(regs, kRegRomCnt, Load32(regs, kRegRomCnt) | kCardDataReady);
        break;
    case protocol::kWireWritePayloadWord:
        device.WritePayloadWord(cmd.index, cmd.word);
        break;
    case protocol::kWireWriteBlock:
        device.WriteBlock(cmd.index, cmd.word, regs + kRegBlockTx, protocol::kBlockBytes);
        break;
    case protocol::kWireReadBlock:
        device.ReadBlock(cmd.index, cmd.word, regs + kRegBlockRx, protocol::kBlockBytes);
        break;
    default:
        break;
    }
    return true;
}

} // namespace ntr_transport
} // namespace gekkopak

#endif // GEKKOPAK_NTR_REGISTER_TRANSPORT_H
