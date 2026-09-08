// GekkoPAK shared device core.
//
// The protocol state machine, allocator, job table and completion queue that
// every GekkoPAK transport presents. The host model, the Azahar core device and
// the DSpico RP2040 firmware all drive this same class; they differ only in how
// bytes reach it.
//
// Freestanding by construction so the RP2040 firmware can use it:
//   - no dynamic allocation (fixed tables, caller-supplied memory pool)
//   - no exceptions, no RTTI, no iostream
//   - no <vector>/<unordered_map>/<deque>
//   - no floating point (the RP2040 has no FPU, and a conformance reference
//     must not depend on double rounding)
//
// Adapters own the bus and nothing else. They convert their native
// representation into (opcode, index, word) plus a data-phase buffer, call the
// matching method here, and convert the response back.

#ifndef GEKKOPAK_DEVICE_H
#define GEKKOPAK_DEVICE_H

// Sibling include, not a rooted one: this header set is copied into foreign
// source trees (Azahar, DSpico) whose include roots differ.
#include "protocol.h"

namespace gekkopak {

using protocol::u16;
using protocol::u32;
using protocol::u64;
using protocol::u8;

class Device {
public:
    // Fixed table sizes. Chosen to match the RP2040 firmware, which is the
    // tightest target; the emulator has no reason to want more.
    static constexpr std::size_t kMaxAllocations = 16;
    static constexpr std::size_t kMaxJobs = 16;
    static constexpr std::size_t kCompletionQueueDepth = 16;
    static constexpr std::size_t kPayloadBytes = 1024;

    struct Config {
        // Device-local memory. The core never allocates; the adapter supplies
        // the backing store and its size.
        u8* pool = nullptr;
        u32 pool_bytes = 0;
        // What HELLO/GET_CAPS advertises. Defaults to pool_bytes. The emulator
        // reports its 32 MiB target while the RP2040 reports its real 64 KiB,
        // which is a device capability difference, not a protocol difference.
        u32 reported_local_bytes = 0;
        u32 capabilities = protocol::kCaps;
        protocol::TimingModel timing{};
    };

    Device() = default;

    // Clears all state. Must be called before any command is dispatched.
    void Reset(const Config& config);

    const Config& config() const { return config_; }

    // -----------------------------------------------------------------------
    // Wire plane. One method per opcode; each is one complete NTR transaction
    // from the core's point of view.
    // -----------------------------------------------------------------------

    // F0
    void WriteReg(u8 index, u32 value);

    // F2. Handles the kEventCompletionDepth pseudo-register as well as the
    // ordinary staging registers.
    u32 ReadReg(u8 index) const;

    // F3
    void WritePayloadWord(u8 index, u32 value);

    // F1
    void Exec(u8 command, u32 sequence);

    // F4. `data` points at the 512-byte data phase the adapter has already
    // received; `word` is the command's value word. Returns the resulting
    // status, which is also left in the RESULT staging register.
    u32 WriteBlock(u8 selector, u32 word, const u8* data, std::size_t data_bytes);

    // F5. Fills `out` with up to `out_bytes` of the selected queue and returns
    // the number of meaningful bytes written. `out` is always fully zeroed
    // first, so an adapter can hand the whole buffer to the bus. Status is left
    // in the RESULT staging register.
    std::size_t ReadBlock(u8 selector, u32 word, u8* out, std::size_t out_bytes);

    // Fills `out` exactly as ReadBlock would but removes nothing from the
    // queue, so a transport can stage a block before it arms a transfer. The
    // cartridge has to have the bytes ready before the console starts clocking
    // them; see prototype/dspico-v1/overlay/src/gekkopakNtr.cpp.
    std::size_t PeekBlock(u8 selector, u32 word, u8* out, std::size_t out_bytes) const;

    // Discards `count` completion records, after a staged block has been sent.
    void DropCompletions(u32 count);

    // Applies ReadBlock's validation and leaves the outcome in RESULT, without
    // reading or consuming anything. A transport that armed its data phase
    // before it could validate the command uses this to report the verdict
    // afterwards.
    u32 ValidateReadBlock(u8 selector, u32 word);

    // -----------------------------------------------------------------------
    // Introspection, for adapters and tests
    // -----------------------------------------------------------------------

    bool completed() const { return completed_; }
    u32 completion_depth() const { return completion_count_; }
    u64 transfers() const { return transfers_; }
    void count_transfer() { ++transfers_; }

    // Direct pool access, for adapters that expose device memory for debug.
    const u8* pool() const { return config_.pool; }

private:
    struct Allocation {
        u32 offset;
        u32 size;
        u32 uploaded;
        bool live;
    };

    struct Job {
        u32 allocation;
        u32 modeled_us;
        u32 speedup_x1000;
        u32 checksum;
        u32 software_us;
        u8 polls_remaining;
        bool ready;
        bool live;
        u32 handle;
    };

    // Allocator: first fit over the flat pool, handle == slot + 1. A freed
    // handle may therefore be reissued by a later ALLOC; the conformance
    // vectors pin that behaviour.
    u32 Allocate(u32 bytes);
    Allocation* AllocationForHandle(u32 handle);
    const Allocation* AllocationForHandle(u32 handle) const;
    Job* JobForHandle(u32 handle);

    bool PushCompletion(const protocol::CompletionRecordV1& record);
    bool PopCompletion(protocol::CompletionRecordV1* out);

    // Integer timing model, in nanoseconds. See docs/CONFORMANCE_ARCHITECTURE.md
    // for why this is not floating point.
    u32 TransferNs(u32 bytes) const;
    u32 ComputeNs(u32 operations) const;
    u32 LegacyModelUs(u32 tx_bytes, u32 rx_bytes, u32 operations) const;
    u32 BlockTransportNs() const;
    static u32 NsToUs(u32 ns) { return (ns + 500u) / 1000u; }

    u32 ProcessDescriptorBlock(const u8* data, std::size_t len);

    Config config_{};
    u32 stage_[protocol::kStageRegisterCount]{};
    u8 payload_[kPayloadBytes]{};
    Allocation allocations_[kMaxAllocations]{};
    Job jobs_[kMaxJobs]{};
    protocol::CompletionRecordV1 completions_[kCompletionQueueDepth]{};
    u32 completion_read_ = 0;
    u32 completion_write_ = 0;
    u32 completion_count_ = 0;
    u32 next_job_ = 1;
    u64 transfers_ = 0;
    bool completed_ = false;
};

} // namespace gekkopak

#endif // GEKKOPAK_DEVICE_H
