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
// is a std::array or an Azahar MMIO region.
//
// ---------------------------------------------------------------------------
// Access-driven, like the hardware
// ---------------------------------------------------------------------------
//
// This transport is driven by individual register accesses, not by a periodic
// service tick. Writing ROMCNT with the activate bit runs the command; reading
// the FIFO takes a word and advances the transfer, clearing DATA_READY by
// itself exactly as the cartridge does; writing the FIFO supplies one. There is
// no acknowledgement protocol, because there is none on silicon.
//
// That requires the emulator to see an individual register access. Azahar
// cannot do that with an ordinary memory mapping -- Citra's MMIORegion was
// removed and the page would simply be RAM -- so the GekkoPAK overlay adds an
// MMIO page type back and routes this window through it. See docs/AZAHAR_MMIO.md.
//
// A guest that programs the wrong ROMCNT block size for its opcode, or touches
// the FIFO with no transfer open, is a reported fault rather than something
// nothing notices.
//
// What is still not modelled is time. A transfer completes as fast as the guest
// can issue loads and stores; there are no card clocks, no latency settings and
// no bus contention.

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
        // reported and the command refused.
        BlockSizeMismatch,
        // The FIFO was touched with no transfer open, or in the wrong
        // direction. On hardware that reads undriven data, or is discarded.
        FifoOutOfPhase,
    };

    void Reset(std::uint8_t* regs) {
        direction_ = PhaseDirection::None;
        word_index_ = 0;
        word_count_ = 0;
        index_ = 0;
        command_word_ = 0;
        fault_ = Fault::None;
        fault_count_ = 0;
        commands_ = 0;
        std::memset(buffer_, 0, sizeof(buffer_));
        if (regs != nullptr) {
            std::memset(regs, 0, kRegisterPageSize);
            Store32(regs, kRegRomCnt, kCardResetHigh);
        }
    }

    bool transfer_active() const { return direction_ != PhaseDirection::None; }
    Fault last_fault() const { return fault_; }
    std::uint32_t fault_count() const { return fault_count_; }
    // NTR transactions accepted, so an adapter can tell a command apart from
    // the data-phase accesses that follow it.
    std::uint64_t commands() const { return commands_; }

    // One 32-bit read of the register window.
    std::uint32_t Read32(Device& device, std::uint8_t* regs, std::uint32_t offset) {
        if (offset + 4u > kRegisterPageSize) {
            return 0;
        }
        if (offset == kRegFifo) {
            return ReadFifo(device, regs);
        }
        return Load32(regs, offset);
    }

    // One 32-bit write to the register window.
    void Write32(Device& device, std::uint8_t* regs, std::uint32_t offset, std::uint32_t value) {
        if (offset + 4u > kRegisterPageSize) {
            return;
        }
        if (offset == kRegFifo) {
            WriteFifo(device, regs, value);
            return;
        }
        Store32(regs, offset, value);
        if (offset == kRegRomCnt && (value & kCardActivate) != 0 &&
            direction_ == PhaseDirection::None) {
            StartCommand(device, regs);
        }
    }

private:
    void StartCommand(Device& device, std::uint8_t* regs) {
        const std::uint32_t romcnt = Load32(regs, kRegRomCnt);

        std::uint8_t bytes[protocol::kCommandBytes];
        std::memcpy(bytes, regs + kRegCommand, sizeof(bytes));
        const protocol::WireCommand cmd = protocol::DecodeCommand(bytes);

        // One NTR transaction, however many words its data phase carries.
        device.count_transfer();
        ++commands_;

        if (!cmd.valid) {
            // Not GekkoPAK traffic. A real cartridge would let another command
            // family claim it; here it is simply dropped.
            EndTransfer(regs);
            return;
        }

        const ExpectedPhase phase = PhaseForOpcode(cmd.opcode);
        if ((romcnt & kCardBlockMask) != phase.block_field) {
            fault_ = Fault::BlockSizeMismatch;
            ++fault_count_;
            EndTransfer(regs);
            return;
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
            EndTransfer(regs);
            return;
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
        }
        Store32(regs, kRegRomCnt, Load32(regs, kRegRomCnt) | kCardDataReady);
    }

    std::uint32_t ReadFifo(Device& device, std::uint8_t* regs) {
        (void)device;
        if (direction_ != PhaseDirection::CartToConsole) {
            fault_ = Fault::FifoOutOfPhase;
            ++fault_count_;
            return 0;
        }

        std::uint32_t value = 0;
        std::memcpy(&value, buffer_ + static_cast<std::size_t>(word_index_) * 4u, sizeof(value));
        // Reading the FIFO is what advances the transfer and clears DATA_READY
        // on hardware. Being able to observe this read is the whole reason the
        // window needs an MMIO page rather than backing memory.
        ++word_index_;
        if (word_index_ >= word_count_) {
            EndTransfer(regs);
        }
        return value;
    }

    void WriteFifo(Device& device, std::uint8_t* regs, std::uint32_t value) {
        if (direction_ != PhaseDirection::ConsoleToCart) {
            fault_ = Fault::FifoOutOfPhase;
            ++fault_count_;
            return;
        }

        std::memcpy(buffer_ + static_cast<std::size_t>(word_index_) * 4u, &value, sizeof(value));
        ++word_index_;
        if (word_index_ >= word_count_) {
            // The whole block has arrived; hand it to the device, then close.
            device.WriteBlock(index_, command_word_, buffer_,
                              static_cast<std::size_t>(word_count_) * 4u);
            EndTransfer(regs);
        }
    }

    void EndTransfer(std::uint8_t* regs) {
        direction_ = PhaseDirection::None;
        Store32(regs, kRegRomCnt, Load32(regs, kRegRomCnt) & ~(kCardActivate | kCardDataReady));
    }

    PhaseDirection direction_ = PhaseDirection::None;
    std::uint16_t word_index_ = 0;
    std::uint16_t word_count_ = 0;
    std::uint8_t index_ = 0;
    std::uint32_t command_word_ = 0;
    std::uint8_t buffer_[protocol::kBlockBytes]{};
    Fault fault_ = Fault::None;
    std::uint32_t fault_count_ = 0;
    std::uint64_t commands_ = 0;
};

} // namespace ntr_transport
} // namespace gekkopak

#endif // GEKKOPAK_NTR_REGISTER_TRANSPORT_H
