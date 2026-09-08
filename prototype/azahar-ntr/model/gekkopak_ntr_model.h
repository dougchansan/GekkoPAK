#pragma once

// Host NTRCARD transport adapter.
//
// This is one of three adapters over the shared GekkoPAK device core; the other
// two are the Azahar core device and the DSpico RP2040 firmware. It owns a
// simulated NTR register page and the ROMCNT activate/data-ready handshake, and
// nothing else: every protocol decision lives in gekkopak::Device.
//
// The names below are re-exported from gekkopak::protocol so existing model
// tests and tools keep compiling against gekkopak::ntr::.

#include "gekkopak/device.h"
#include "gekkopak/protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gekkopak::ntr {

// Re-exported so `using namespace gekkopak::ntr;` also reaches the protocol
// helpers, which the model tests and the trace tools need.
namespace protocol = ::gekkopak::protocol;

// -- NTR register page ------------------------------------------------------

constexpr std::uint32_t kPhysicalBase = 0x10164000u;
constexpr std::uint32_t kVirtualBase = 0x1EC64000u;
constexpr std::size_t kRegisterPageSize = 0x1000;

constexpr std::size_t kRegRomCnt = 0x04;
constexpr std::size_t kRegCommand = 0x08;
constexpr std::size_t kRegFifo = 0x1C;
// Block staging windows. These are an emulator convenience: on real hardware
// the 512-byte payload crosses the bus as an NTR data phase, not through the
// register page. See docs/CONFORMANCE_RESULTS.md for the fidelity gap this
// leaves.
constexpr std::size_t kRegBlockTx = 0x100;
constexpr std::size_t kRegBlockRx = 0x300;

constexpr std::uint32_t kCardActivate = 1u << 31;
constexpr std::uint32_t kCardResetHigh = 1u << 29;
constexpr std::uint32_t kCardDataReady = 1u << 23;
constexpr std::uint32_t kCardBlock4 = 7u << 24;

// -- protocol re-exports ----------------------------------------------------

constexpr std::uint8_t kWireWriteReg = protocol::kWireWriteReg;
constexpr std::uint8_t kWireExec = protocol::kWireExec;
constexpr std::uint8_t kWireReadReg = protocol::kWireReadReg;
constexpr std::uint8_t kWireWritePayloadWord = protocol::kWireWritePayloadWord;
constexpr std::uint8_t kWireWriteBlock = protocol::kWireWriteBlock;
constexpr std::uint8_t kWireReadBlock = protocol::kWireReadBlock;
constexpr std::uint8_t kMagic0 = protocol::kMagic0;
constexpr std::uint8_t kMagic1 = protocol::kMagic1;
constexpr std::uint8_t kEventCompletionDepth = protocol::kEventCompletionDepth;

constexpr std::uint32_t kProtocolVersion = protocol::kProtocolVersion;
constexpr std::uint32_t kBaseCaps = protocol::kBaseCaps;
constexpr std::uint32_t kCapBlockTransport = protocol::kCapBlockTransport;
constexpr std::uint32_t kCaps = protocol::kCaps;
constexpr std::uint32_t kLocalMemoryBytes = 32u * 1024u * 1024u;

constexpr std::size_t kBlockBytes = protocol::kBlockBytes;
constexpr std::size_t kDescriptorBytes = protocol::kDescriptorBytes;
constexpr std::size_t kCompletionBytes = protocol::kCompletionBytes;
constexpr std::size_t kMaxDescriptorsPerBlock = protocol::kMaxDescriptorsPerBlock;
constexpr std::uint32_t kDescriptorMagic = protocol::kDescriptorMagic;
constexpr std::uint32_t kCompletionMagic = protocol::kCompletionMagic;
constexpr std::uint16_t kBlockProtocolVersion = protocol::kBlockProtocolVersion;
constexpr std::uint16_t kDescriptorOpcodeSubmit = protocol::kDescriptorOpcodeSubmit;
constexpr std::uint32_t kDescriptorFlagInlineInput = protocol::kDescriptorFlagInlineInput;

using Register = protocol::StageRegister;
using protocol::kArg0;
using protocol::kArg1;
using protocol::kArg2;
using protocol::kArg3;
using protocol::kOut0;
using protocol::kOut1;
using protocol::kOut2;
using protocol::kOut3;
using protocol::kPayloadLen;
using protocol::kResult;

// Legacy spellings, kept so existing tests read the same.
constexpr Register Arg0 = protocol::kArg0;
constexpr Register Arg1 = protocol::kArg1;
constexpr Register Arg2 = protocol::kArg2;
constexpr Register Arg3 = protocol::kArg3;
constexpr Register PayloadLen = protocol::kPayloadLen;
constexpr Register Result = protocol::kResult;
constexpr Register Out0 = protocol::kOut0;
constexpr Register Out1 = protocol::kOut1;
constexpr Register Out2 = protocol::kOut2;
constexpr Register Out3 = protocol::kOut3;

using HighCommand = protocol::HighCommand;
constexpr HighCommand Hello = protocol::kHello;
constexpr HighCommand GetCaps = protocol::kGetCaps;
constexpr HighCommand Alloc = protocol::kAlloc;
constexpr HighCommand Upload = protocol::kUpload;
constexpr HighCommand Submit = protocol::kSubmit;
constexpr HighCommand Poll = protocol::kPoll;
constexpr HighCommand Collect = protocol::kCollect;
constexpr HighCommand Free = protocol::kFree;
constexpr HighCommand Complete = protocol::kComplete;

using Error = protocol::Status;
constexpr Error Ok = protocol::kOk;
constexpr Error BadCommand = protocol::kBadCommand;
constexpr Error BadHandle = protocol::kBadHandle;
constexpr Error NoMemory = protocol::kNoMemory;
constexpr Error NotReady = protocol::kNotReady;
constexpr Error BadDescriptor = protocol::kBadDescriptor;
constexpr Error BadBlock = protocol::kBadBlock;
constexpr Error QueueFull = protocol::kQueueFull;

enum class BlockSelector : std::uint8_t {
    Submission = protocol::kSelectorSubmission,
    Completion = protocol::kSelectorCompletion,
};

using JobDescriptorV1 = protocol::JobDescriptorV1;
using CompletionRecordV1 = protocol::CompletionRecordV1;

// -- adapter ----------------------------------------------------------------

class Device {
public:
    // `local_bytes` is what the device advertises and backs. It defaults to the
    // 32 MiB emulator target; the conformance runner overrides it so every
    // target answers with the same numbers.
    explicit Device(std::uint32_t local_bytes = kLocalMemoryBytes);

    std::uint8_t* registers() { return regs_.data(); }
    const std::uint8_t* registers() const { return regs_.data(); }
    std::size_t register_size() const { return regs_.size(); }

    // F0-F3 bootstrap/debug transport: consume one command from the register
    // page, driven by the ROMCNT activate bit.
    void Tick();

    // F4/F5 block transport. Each call is one complete NTR command plus its
    // data phase.
    bool WriteBlock(BlockSelector selector, const std::uint8_t* data, std::size_t len,
                    std::uint16_t wire_flags = 0);
    std::uint32_t ReadEvent();
    std::size_t ReadBlock(BlockSelector selector, std::uint16_t offset, std::uint8_t* out,
                          std::size_t len);

    bool completed() const { return core_.completed(); }
    std::uint64_t transfers() const { return core_.transfers(); }
    std::size_t completion_depth() const { return core_.completion_depth(); }

    gekkopak::Device& core() { return core_; }
    const gekkopak::Device& core() const { return core_; }

private:
    std::uint32_t Load32(std::size_t off) const;
    void Store32(std::size_t off, std::uint32_t value);

    std::array<std::uint8_t, kRegisterPageSize> regs_{};
    std::vector<std::uint8_t> pool_;
    gekkopak::Device core_;
};

} // namespace gekkopak::ntr
