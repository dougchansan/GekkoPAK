// GekkoPAK wire protocol definitions.
//
// This header is the single normative description of the GekkoPAK cartridge
// protocol. It is shared verbatim by the host model, the Azahar core device and
// the DSpico RP2040 firmware, so it must stay freestanding: <cstdint> and
// <cstddef> only, no allocation, no exceptions, no floating point.
//
// See docs/NTR_WIRE_V1.md for the prose specification and
// docs/CONFORMANCE_ARCHITECTURE.md for why this is shared rather than copied.

#ifndef GEKKOPAK_PROTOCOL_H
#define GEKKOPAK_PROTOCOL_H

#include <cstddef>
#include <cstdint>

namespace gekkopak {
namespace protocol {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---------------------------------------------------------------------------
// Wire command
// ---------------------------------------------------------------------------

// Every GekkoPAK command is eight bytes on the NTR bus:
//
//     OP 47 4B II VV VV VV VV
//
// with the value word VV big-endian, MSB first. Bytes 1 and 2 are the "GK"
// discriminator, which lets a cartridge that also uses the F0-F5 opcode range
// reject GekkoPAK traffic instead of misinterpreting it.
inline constexpr u8 kMagic0 = 0x47; // 'G'
inline constexpr u8 kMagic1 = 0x4B; // 'K'

enum WireOpcode : u8 {
    kWireWriteReg = 0xF0,
    kWireExec = 0xF1,
    kWireReadReg = 0xF2,
    kWireWritePayloadWord = 0xF3,
    kWireWriteBlock = 0xF4,
    kWireReadBlock = 0xF5,
};

inline constexpr std::size_t kCommandBytes = 8;

struct WireCommand {
    u8 opcode = 0;
    u8 index = 0;
    u32 word = 0;
    bool valid = false; // false when the "GK" discriminator did not match
};

// Decode the canonical big-endian wire form. This is the byte order DSpico's
// PIO delivers and the order docs/HARDWARE_DSPICO_V1.md confirmed on silicon.
inline WireCommand DecodeCommand(const u8 bytes[kCommandBytes]) {
    WireCommand cmd;
    cmd.opcode = bytes[0];
    cmd.index = bytes[3];
    cmd.word = (static_cast<u32>(bytes[4]) << 24) | (static_cast<u32>(bytes[5]) << 16) |
               (static_cast<u32>(bytes[6]) << 8) | static_cast<u32>(bytes[7]);
    cmd.valid = bytes[1] == kMagic0 && bytes[2] == kMagic1;
    return cmd;
}

inline void EncodeCommand(u8 opcode, u8 index, u32 word, u8 out[kCommandBytes]) {
    out[0] = opcode;
    out[1] = kMagic0;
    out[2] = kMagic1;
    out[3] = index;
    out[4] = static_cast<u8>(word >> 24);
    out[5] = static_cast<u8>(word >> 16);
    out[6] = static_cast<u8>(word >> 8);
    out[7] = static_cast<u8>(word);
}

// ---------------------------------------------------------------------------
// Staging registers (F0 write / F2 read)
// ---------------------------------------------------------------------------

enum StageRegister : u8 {
    kArg0 = 0,
    kArg1 = 1,
    kArg2 = 2,
    kArg3 = 3,
    kPayloadLen = 4,
    kResult = 5,
    kOut0 = 6,
    kOut1 = 7,
    kOut2 = 8,
    kOut3 = 9,
};

inline constexpr std::size_t kStageRegisterCount = 16;

// F2 pseudo-register: returns the completion-queue depth without spending a
// 512-byte F5 block just to learn whether anything is ready.
inline constexpr u8 kEventCompletionDepth = 0xFE;

// ---------------------------------------------------------------------------
// High-level commands (F1)
// ---------------------------------------------------------------------------

enum HighCommand : u8 {
    kHello = 1,
    kGetCaps = 2,
    kAlloc = 3,
    kUpload = 4,
    kSubmit = 5,
    kPoll = 6,
    kCollect = 7,
    kFree = 8,
    kComplete = 9,
};

enum Status : u32 {
    kOk = 0,
    kBadCommand = 1,
    kBadHandle = 2,
    kNoMemory = 3,
    kNotReady = 4,
    kBadDescriptor = 5,
    kBadBlock = 6,
    kQueueFull = 7,
};

// ---------------------------------------------------------------------------
// Device identity
// ---------------------------------------------------------------------------

inline constexpr u32 kProtocolVersion = 0x00010000u; // 1.0
inline constexpr u32 kBaseCaps = 0x0000000Fu;
inline constexpr u32 kCapBlockTransport = 1u << 4;
inline constexpr u32 kCaps = kBaseCaps | kCapBlockTransport;

// COMPLETE acknowledges with 'PASS'.
inline constexpr u32 kCompleteAck = 0x53534150u;

// ---------------------------------------------------------------------------
// Modeled transport parameters
// ---------------------------------------------------------------------------
//
// These are MODELED ASSUMPTIONS, not measurements. They are what the emulator
// and the pre-hardware firmware report so that "deterministic" means the same
// numbers on every target. Real DSpico measurements replace them; see
// docs/CONFORMANCE_RESULTS.md for which numbers are which.
struct TimingModel {
    u32 command_latency_ns = 25000;         // 25 us
    u32 bus_bytes_per_second = 6u * 1024u * 1024u; // 6 MiB/s
    u32 kernel_ops_per_second = 100000000u; // 1e8 ops/s
};

// ---------------------------------------------------------------------------
// Block transport (F4/F5)
// ---------------------------------------------------------------------------

inline constexpr std::size_t kBlockBytes = 512;
inline constexpr std::size_t kDescriptorBytes = 64;
inline constexpr std::size_t kCompletionBytes = 64;
inline constexpr std::size_t kMaxDescriptorsPerBlock = kBlockBytes / kDescriptorBytes;

// Block selectors, carried in the command's index byte.
enum BlockSelector : u8 {
    kSelectorSubmission = 0, // F4 destination: descriptor/submission queue
    kSelectorCompletion = 1, // F5 source: completion queue
};

// F4 value word: bits 15:0 are the meaningful byte count of the 512-byte data
// phase. F5 value word: bits 15:0 are the source offset, bits 31:16 the
// requested length. Note that this is the opposite of the halves given in the
// original docs/NTR_WIRE_V1.md draft; the implementations are normative and the
// document was corrected to match.
inline constexpr u32 EncodeWriteBlockWord(u16 meaningful_bytes) {
    return static_cast<u32>(meaningful_bytes);
}
inline constexpr u32 EncodeReadBlockWord(u16 offset, u16 length) {
    return (static_cast<u32>(length) << 16) | static_cast<u32>(offset);
}

inline constexpr u32 kDescriptorMagic = 0x31444B47u;  // 'GKD1' little-endian
inline constexpr u32 kCompletionMagic = 0x31434B47u;  // 'GKC1' little-endian
inline constexpr u16 kBlockProtocolVersion = 1;
inline constexpr u16 kDescriptorOpcodeSubmit = 1;
inline constexpr u32 kDescriptorFlagInlineInput = 1u << 0;

// The block payload ABI is byte-transparent in both directions on real
// hardware, so these structs are laid out little-endian and copied verbatim.
struct JobDescriptorV1 {
    u32 magic = kDescriptorMagic;
    u16 version = kBlockProtocolVersion;
    u16 opcode = kDescriptorOpcodeSubmit;
    u32 sequence = 0;
    u32 flags = 0;
    u32 input_handle = 0;
    u32 input_offset = 0;
    u32 input_length = 0;
    u32 output_handle = 0;
    u32 output_offset = 0;
    u32 output_length = 0;
    u32 kernel_id = 0;
    u32 work_units = 0;
    u32 arg0 = 0;
    u32 arg1 = 0;
    u32 arg2 = 0;
    u32 arg3 = 0;
};
static_assert(sizeof(JobDescriptorV1) == kDescriptorBytes,
              "v1 job descriptor must be exactly 64 bytes");

struct CompletionRecordV1 {
    u32 magic = kCompletionMagic;
    u16 version = kBlockProtocolVersion;
    u16 status = static_cast<u16>(kOk);
    u32 sequence = 0;
    u32 job_handle = 0;
    u32 modeled_us = 0;
    u32 speedup_x1000 = 0;
    u32 checksum = 0;
    u32 software_us = 0;
    u32 output_length = 0;
    u32 transport_us_x1000 = 0;
    u32 batch_size = 0;
    u32 reserved[5] = {};
};
static_assert(sizeof(CompletionRecordV1) == kCompletionBytes,
              "v1 completion record must be exactly 64 bytes");

// ---------------------------------------------------------------------------
// Checksum
// ---------------------------------------------------------------------------

// FNV-1a over uploaded bytes. The 16-byte reference pattern
// 44 33 22 11 88 77 66 55 DD CC BB AA 0D F0 AD 0B hashes to 0xf269b734, which
// is confirmed on silicon and is the transport's byte-order canary: a word swap
// would produce 0x899bd1de instead.
inline constexpr u32 kReferenceChecksum = 0xF269B734u;

inline u32 Fnv1a(const u8* data, std::size_t size) {
    u32 hash = 2166136261u;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

} // namespace protocol
} // namespace gekkopak

#endif // GEKKOPAK_PROTOCOL_H
