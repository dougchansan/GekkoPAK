// Register-page NTR transport, shared by the host model and the Azahar core
// device.
//
// Both emulate the same thing: a 4 KiB NTRCARD register page in which the guest
// stages an 8-byte command, raises the ROMCNT activate bit, and moves the data
// phase through the 32-bit FIFO a word at a time. Keeping that decode in one
// header is what makes "the host model and the Azahar device model behave
// identically" a property of the code rather than a claim about two copies.
//
// Freestanding, like the rest of the core: the caller owns the page, whether it
// is a std::array or an Azahar BackingMem.
//
// ---------------------------------------------------------------------------
// Fidelity, and the one remaining emulator-ism
// ---------------------------------------------------------------------------
//
// The data phase is modelled the way the bus actually works: the guest declares
// a transfer length in ROMCNT's block-size field, the transfer stays active
// until every word has crossed, and each word crosses through the FIFO
// register. A guest that programs the wrong block size for its opcode is now a
// fault rather than something nothing notices.
//
// What is NOT hardware is the per-word acknowledgement. On silicon, reading the
// FIFO clears DATA_READY by itself. Azahar maps this page as ordinary backing
// memory -- it has no MMIO page type at all, Citra's MMIORegion having been
// removed -- so the device cannot see an individual read or write and cannot
// auto-clear anything. The guest therefore clears DATA_READY itself to say "I
// have taken that word" (or "I have supplied that word"), and the device
// advances on the next service tick. That is the same shape as the CARD_START
// handshake the command phase already uses.
//
// The cost is one service tick per word, so 128 ticks per 512-byte block. The
// device is serviced once per CPU slice and a frame is many slices, so a full
// block transfer is comfortably inside a frame.

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

inline constexpr std::uint32_t kCardActivate = 1u << 31;
inline constexpr std::uint32_t kCardResetHigh = 1u << 29;
inline constexpr std::uint32_t kCardDataReady = 1u << 23;

// ROMCNT data-block size, bits 26:24. The encoding is the hardware's: 0 means
// no data phase, 7 means exactly four bytes, and 1 means 512. There is no
// encoding for 16, 32, 64, 128 or 256 bytes, which is why a small GekkoPAK
// payload is either a run of four-byte F3 transactions or one 512-byte F4 whose
// meaningful region is short.
inline constexpr std::uint32_t kCardBlockMask = 7u << 24;
inline constexpr std::uint32_t kCardBlockNone = 0u << 24;
inline constexpr std::uint32_t kCardBlock512 = 1u << 24;
inline constexpr std::uint32_t kCardBlock4 = 7u << 24;

inline std::uint32_t Load32(const std::uint8_t* regs, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, regs + offset, sizeof(value));
    return value;
}

inline void Store32(std::uint8_t* regs, std::size_t offset, std::uint32_t value) {
    std::memcpy(regs + offset, &value, sizeof(value));
}

// How many bytes each opcode's data phase carries, and in which direction.
enum class PhaseDirection : std::uint8_t {
    None,
    CartToConsole, // F2, F5
    ConsoleToCart, // F4
};

struct ExpectedPhase {
    PhaseDirection direction = PhaseDirection::None;
    std::uint16_t bytes = 0;
    std::uint32_t block_field = kCardBlockNone;
};

inline ExpectedPhase PhaseForOpcode(std::uint8_t opcode) {
    switch (opcode) {
    case protocol::kWireReadReg:
        return {PhaseDirection::CartToConsole, 4, kCardBlock4};
    case protocol::kWireReadBlock:
        return {PhaseDirection::CartToConsole,
                static_cast<std::uint16_t>(protocol::kBlockBytes), kCardBlock512};
    case protocol::kWireWriteBlock:
        return {PhaseDirection::ConsoleToCart,
                static_cast<std::uint16_t>(protocol::kBlockBytes), kCardBlock512};
    default:
        return {PhaseDirection::None, 0, kCardBlockNone};
    }
}

class RegisterTransport {
public:
    enum class Fault : std::uint8_t {
        None = 0,
        // ROMCNT's block-size field did not match the data phase the opcode
        // requires. On hardware this desynchronises the bus; here it is
        // reported and the command is refused.
        BlockSizeMismatch,
    };

    void Reset(std::uint8_t* regs) {
        direction_ = PhaseDirection::None;
        word_index_ = 0;
        word_count_ = 0;
        index_ = 0;
        command_word_ = 0;
        fault_ = Fault::None;
        fault_count_ = 0;
        std::memset(buffer_, 0, sizeof(buffer_));
        if (regs != nullptr) {
            Store32(regs, kRegRomCnt, kCardResetHigh);
        }
    }

    bool transfer_active() const { return direction_ != PhaseDirection::None; }
    Fault last_fault() const { return fault_; }
    std::uint32_t fault_count() const { return fault_count_; }

    // Services the page for one slice. Returns true when something was done --
    // a command was accepted, or a data-phase word crossed.
    bool Tick(Device& device, std::uint8_t* regs) {
        if (direction_ != PhaseDirection::None) {
            return ServiceDataPhase(device, regs);
        }
        return StartCommand(device, regs);
    }

private:
    bool StartCommand(Device& device, std::uint8_t* regs) {
        const std::uint32_t romcnt = Load32(regs, kRegRomCnt);
        if ((romcnt & kCardActivate) == 0) {
            return false;
        }

        std::uint8_t bytes[protocol::kCommandBytes];
        std::memcpy(bytes, regs + kRegCommand, sizeof(bytes));
        const protocol::WireCommand cmd = protocol::DecodeCommand(bytes);

        // One NTR transaction, however many words its data phase carries.
        device.count_transfer();

        if (!cmd.valid) {
            // Not GekkoPAK traffic. A real cartridge would let another command
            // family claim it; here it is simply dropped.
            Store32(regs, kRegRomCnt, romcnt & ~(kCardActivate | kCardDataReady));
            return true;
        }

        const ExpectedPhase phase = PhaseForOpcode(cmd.opcode);
        if ((romcnt & kCardBlockMask) != phase.block_field) {
            fault_ = Fault::BlockSizeMismatch;
            ++fault_count_;
            Store32(regs, kRegRomCnt, romcnt & ~(kCardActivate | kCardDataReady));
            return true;
        }

        index_ = cmd.index;
        command_word_ = cmd.word;

        if (phase.direction == PhaseDirection::None) {
            switch (cmd.opcode) {
            case protocol::kWireWriteReg:
                device.WriteReg(cmd.index, cmd.word);
                break;
            case protocol::kWireExec:
                device.Exec(cmd.index, cmd.word);
                break;
            case protocol::kWireWritePayloadWord:
                device.WritePayloadWord(cmd.index, cmd.word);
                break;
            default:
                break;
            }
            Store32(regs, kRegRomCnt, romcnt & ~(kCardActivate | kCardDataReady));
            return true;
        }

        // Open a data phase. CARD_START stays set until the last word crosses.
        word_index_ = 0;
        word_count_ = static_cast<std::uint16_t>(phase.bytes / 4u);
        direction_ = phase.direction;

        if (direction_ == PhaseDirection::CartToConsole) {
            std::memset(buffer_, 0, sizeof(buffer_));
            if (cmd.opcode == protocol::kWireReadReg) {
                const std::uint32_t value = device.ReadReg(cmd.index);
                std::memcpy(buffer_, &value, sizeof(value));
            } else {
                device.ReadBlock(cmd.index, cmd.word, buffer_, phase.bytes);
            }
            PresentWord(regs, 0);
        }

        // CARD_START stays set for the duration; DATA_READY says a word is
        // available (outbound) or expected (inbound).
        Store32(regs, kRegRomCnt, romcnt | kCardDataReady);
        return true;
    }

    bool ServiceDataPhase(Device& device, std::uint8_t* regs) {
        const std::uint32_t romcnt = Load32(regs, kRegRomCnt);
        // DATA_READY still set means the guest has not taken (or supplied) the
        // current word yet. See the emulator-ism note at the top of this file.
        if ((romcnt & kCardDataReady) != 0) {
            return false;
        }

        if (direction_ == PhaseDirection::ConsoleToCart) {
            const std::uint32_t value = Load32(regs, kRegFifo);
            std::memcpy(buffer_ + static_cast<std::size_t>(word_index_) * 4u, &value,
                        sizeof(value));
        }

        ++word_index_;
        if (word_index_ < word_count_) {
            if (direction_ == PhaseDirection::CartToConsole) {
                PresentWord(regs, word_index_);
            }
            Store32(regs, kRegRomCnt, romcnt | kCardDataReady);
            return true;
        }

        // Last word has crossed. Deliver a console-to-cartridge payload now that
        // all of it has arrived, then close the transfer.
        if (direction_ == PhaseDirection::ConsoleToCart) {
            device.WriteBlock(index_, command_word_, buffer_,
                              static_cast<std::size_t>(word_count_) * 4u);
        }
        direction_ = PhaseDirection::None;
        Store32(regs, kRegRomCnt, romcnt & ~(kCardActivate | kCardDataReady));
        return true;
    }

    void PresentWord(std::uint8_t* regs, std::uint16_t word) {
        std::uint32_t value = 0;
        std::memcpy(&value, buffer_ + static_cast<std::size_t>(word) * 4u, sizeof(value));
        Store32(regs, kRegFifo, value);
    }

    PhaseDirection direction_ = PhaseDirection::None;
    std::uint16_t word_index_ = 0;
    std::uint16_t word_count_ = 0;
    std::uint8_t index_ = 0;
    std::uint32_t command_word_ = 0;
    std::uint8_t buffer_[protocol::kBlockBytes]{};
    Fault fault_ = Fault::None;
    std::uint32_t fault_count_ = 0;
};

} // namespace ntr_transport
} // namespace gekkopak

#endif // GEKKOPAK_NTR_REGISTER_TRANSPORT_H
