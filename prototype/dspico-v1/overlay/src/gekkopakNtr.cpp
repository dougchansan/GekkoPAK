#include "gekkopakNtr.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "ntrCardRomGameNoScramble.h"

namespace {

constexpr u32 kProtocolVersion = 0x00010000u;
constexpr u32 kBaseCaps = 0x0000000Fu;
constexpr u32 kCapBlockTransport = 1u << 4;
constexpr u32 kCapabilities = kBaseCaps | kCapBlockTransport;

#ifndef GEKKOPAK_LOCAL_BYTES
#define GEKKOPAK_LOCAL_BYTES (64u * 1024u)
#endif
constexpr u32 kLocalBytes = GEKKOPAK_LOCAL_BYTES;
constexpr u32 kModeledBandwidthBps = 6u * 1024u * 1024u;
constexpr u32 kCommandLatencyUs = 25u;
constexpr u32 kKernelOpsPerSecond = 100000000u;

constexpr u32 kCommandDiscriminator = 0x00474B00u;
constexpr u32 kCommandLowMask = 0xFFFFFF00u;
constexpr u8 kEventCompletionDepth = 0xFE;
constexpr std::size_t kBlockBytes = 512;
constexpr std::size_t kDescriptorBytes = 64;
constexpr std::size_t kCompletionBytes = 64;
constexpr std::size_t kMaxBatch = kBlockBytes / kDescriptorBytes;
constexpr std::size_t kMaxAllocations = 16;
constexpr std::size_t kMaxJobs = 16;
constexpr std::size_t kCompletionQueueDepth = 16;
constexpr u32 kDescriptorMagic = 0x31444B47u; // GKD1, little-endian payload ABI
constexpr u32 kCompletionMagic = 0x31434B47u; // GKC1
constexpr u16 kBlockVersion = 1;
constexpr u16 kSubmitOpcode = 1;
constexpr u32 kInlineInput = 1u << 0;

constexpr u8 kWireWriteReg = 0xF0;
constexpr u8 kWireExec = 0xF1;
constexpr u8 kWireReadReg = 0xF2;
constexpr u8 kWirePayloadWord = 0xF3;
constexpr u8 kWireWriteBlock = 0xF4;
constexpr u8 kWireReadBlock = 0xF5;

enum StageRegister : u8 {
    Arg0 = 0,
    Arg1 = 1,
    Arg2 = 2,
    Arg3 = 3,
    PayloadLen = 4,
    Result = 5,
    Out0 = 6,
    Out1 = 7,
    Out2 = 8,
    Out3 = 9,
};

enum HighCommand : u8 {
    Hello = 1,
    GetCaps = 2,
    Alloc = 3,
    Upload = 4,
    Submit = 5,
    Poll = 6,
    Collect = 7,
    Free = 8,
    Complete = 9,
};

enum Error : u32 {
    Ok = 0,
    BadCommand = 1,
    BadHandle = 2,
    NoMemory = 3,
    NotReady = 4,
    BadDescriptor = 5,
    BadBlock = 6,
    QueueFull = 7,
};

struct JobDescriptorV1 {
    u32 magic;
    u16 version;
    u16 opcode;
    u32 sequence;
    u32 flags;
    u32 input_handle;
    u32 input_offset;
    u32 input_length;
    u32 output_handle;
    u32 output_offset;
    u32 output_length;
    u32 kernel_id;
    u32 work_units;
    u32 arg0;
    u32 arg1;
    u32 arg2;
    u32 arg3;
};
static_assert(sizeof(JobDescriptorV1) == kDescriptorBytes);

struct CompletionRecordV1 {
    u32 magic;
    u16 version;
    u16 status;
    u32 sequence;
    u32 job_handle;
    u32 modeled_us;
    u32 speedup_x1000;
    u32 checksum;
    u32 software_us;
    u32 output_length;
    u32 transport_us_x1000;
    u32 batch_size;
    u32 reserved[5];
};
static_assert(sizeof(CompletionRecordV1) == kCompletionBytes);

struct Allocation {
    u32 offset;
    u32 size;
    u32 uploaded;
    bool live;
};

struct Job {
    u32 handle;
    u32 allocation;
    u32 modeled_us;
    u32 speedup_x1000;
    u32 checksum;
    u32 software_us;
    u8 polls_remaining;
    bool live;
};

alignas(4) std::array<u8, kLocalBytes> sLocal{};
alignas(4) std::array<u8, 1024> sLegacyPayload{};
alignas(4) std::array<u8, kBlockBytes> sBlockTx{};
alignas(4) std::array<u8, kBlockBytes> sBlockRx{};
std::array<u32, 16> sStage{};
std::array<Allocation, kMaxAllocations> sAllocations{};
std::array<Job, kMaxJobs> sJobs{};
std::array<CompletionRecordV1, kCompletionQueueDepth> sCompletions{};
u32 sNextJob = 1;

// F4 instrumentation.
//
// The host has run out of things it can distinguish. From the DS side an F4
// that never arrives, one that arrives but whose payload is not captured, and
// one whose descriptor is rejected are all identical: no completion appears and
// the transfer reports no error. These counters separate them, and are read
// back through F2 at indices 0xF0-0xF3.
u32 sF4Enter;     // cmd1 handler entered at all
u32 sF4Accepted;  // passed the opcode/index/length checks and began a read
u32 sF4Complete;  // payload fully received, completion callback fired
u32 sF4Parsed;    // descriptors accepted by processDescriptorBatch
u32 sCompletionRead = 0;
u32 sCompletionWrite = 0;
u32 sCompletionCount = 0;

bool commandMatches(const ntr_rom_emu_t* romEmu, u8 opcode) {
    const u32 expected = (static_cast<u32>(opcode) << 24) | kCommandDiscriminator;
    return (romEmu->cmd0 & kCommandLowMask) == expected;
}

u8 commandIndex(const ntr_rom_emu_t* romEmu) {
    return static_cast<u8>(romEmu->cmd0 & 0xFFu);
}

u32 fnv1a(const u8* data, std::size_t size) {
    u32 hash = 2166136261u;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

u32 transferUsRounded(u32 bytes) {
    const u64 scaled = static_cast<u64>(bytes) * 1000000ull;
    return static_cast<u32>((scaled + kModeledBandwidthBps / 2) / kModeledBandwidthBps);
}

u32 legacyModelUs(u32 txBytes, u32 rxBytes, u32 operations) {
    const u32 compute = static_cast<u32>((static_cast<u64>(operations) * 1000000ull +
                                          kKernelOpsPerSecond / 2) /
                                         kKernelOpsPerSecond);
    return kCommandLatencyUs + transferUsRounded(txBytes + rxBytes) + compute;
}

u32 v1TransportUsX1000() {
    constexpr u32 bytes = 512 + 4 + 512;
    const u64 byteUsX1000 =
        (static_cast<u64>(bytes) * 1000000000ull + kModeledBandwidthBps / 2) /
        kModeledBandwidthBps;
    return 3u * kCommandLatencyUs * 1000u + static_cast<u32>(byteUsX1000);
}

Allocation* allocationForHandle(u32 handle) {
    if (handle == 0 || handle > sAllocations.size())
        return nullptr;
    Allocation& allocation = sAllocations[handle - 1];
    return allocation.live ? &allocation : nullptr;
}

u32 allocate(u32 bytes) {
    bytes = (bytes + 3u) & ~3u;
    if (bytes == 0 || bytes > kLocalBytes)
        return 0;

    std::size_t slot = sAllocations.size();
    for (std::size_t i = 0; i < sAllocations.size(); ++i) {
        if (!sAllocations[i].live) {
            slot = i;
            break;
        }
    }
    if (slot == sAllocations.size())
        return 0;

    u32 candidate = 0;
    while (candidate + bytes <= kLocalBytes) {
        bool overlap = false;
        u32 bump = candidate;
        for (const Allocation& allocation : sAllocations) {
            if (!allocation.live)
                continue;
            const u32 end = candidate + bytes;
            const u32 allocationEnd = allocation.offset + allocation.size;
            if (end <= allocation.offset || candidate >= allocationEnd)
                continue;
            overlap = true;
            bump = std::max(bump, allocationEnd);
        }
        if (!overlap) {
            sAllocations[slot] = Allocation{candidate, bytes, 0, true};
            return static_cast<u32>(slot + 1);
        }
        candidate = (bump + 3u) & ~3u;
    }
    return 0;
}

Job* jobForHandle(u32 handle) {
    for (Job& job : sJobs) {
        if (job.live && job.handle == handle)
            return &job;
    }
    return nullptr;
}

Job* createJob() {
    for (Job& job : sJobs) {
        if (!job.live) {
            job = Job{};
            job.handle = sNextJob++;
            job.live = true;
            return &job;
        }
    }
    return nullptr;
}

bool pushCompletion(const CompletionRecordV1& completion) {
    if (sCompletionCount == sCompletions.size())
        return false;
    sCompletions[sCompletionWrite] = completion;
    sCompletionWrite = (sCompletionWrite + 1) % sCompletions.size();
    ++sCompletionCount;
    return true;
}

bool popCompletion(CompletionRecordV1* completion) {
    if (sCompletionCount == 0)
        return false;
    *completion = sCompletions[sCompletionRead];
    sCompletionRead = (sCompletionRead + 1) % sCompletions.size();
    --sCompletionCount;
    return true;
}

void executeHighCommand(u8 command) {
    sStage[Result] = Ok;
    sStage[Out0] = sStage[Out1] = sStage[Out2] = sStage[Out3] = 0;

    switch (command) {
    case Hello:
        sStage[Out0] = kProtocolVersion;
        sStage[Out1] = kCapabilities;
        sStage[Out2] = kLocalBytes;
        sStage[Out3] = 2;
        break;
    case GetCaps:
        sStage[Out0] = kCapabilities;
        sStage[Out1] = kLocalBytes;
        // Until physical characterization, these remain compatibility model values.
        sStage[Out2] = kModeledBandwidthBps;
        sStage[Out3] = kCommandLatencyUs;
        break;
    case Alloc: {
        const u32 handle = allocate(sStage[Arg0]);
        if (handle == 0) {
            sStage[Result] = NoMemory;
            break;
        }
        sStage[Out0] = handle;
        sStage[Out1] = allocationForHandle(handle)->size;
        break;
    }
    case Upload: {
        Allocation* allocation = allocationForHandle(sStage[Arg0]);
        const u32 offset = sStage[Arg1];
        const u32 len = sStage[PayloadLen];
        if (!allocation || len > sLegacyPayload.size() || offset > allocation->size ||
            len > allocation->size - offset) {
            sStage[Result] = BadHandle;
            break;
        }
        std::copy_n(sLegacyPayload.begin(), len, sLocal.begin() + allocation->offset + offset);
        allocation->uploaded = std::max(allocation->uploaded, offset + len);
        sStage[Out0] = len;
        sStage[Out1] = fnv1a(sLocal.data() + allocation->offset, allocation->uploaded);
        break;
    }
    case Submit: {
        Allocation* allocation = allocationForHandle(sStage[Arg0]);
        Job* job = createJob();
        if (!allocation) {
            sStage[Result] = BadHandle;
            if (job)
                job->live = false;
            break;
        }
        if (!job) {
            sStage[Result] = NoMemory;
            break;
        }
        job->allocation = sStage[Arg0];
        job->software_us = sStage[Arg3];
        job->checksum = fnv1a(sLocal.data() + allocation->offset, allocation->uploaded);
        job->modeled_us = legacyModelUs(allocation->uploaded, 16, sStage[Arg2]);
        job->speedup_x1000 = job->modeled_us
                                 ? static_cast<u32>((static_cast<u64>(job->software_us) * 1000u) /
                                                    job->modeled_us)
                                 : 0;
        job->polls_remaining = 2;
        sStage[Out0] = job->handle;
        sStage[Out1] = job->modeled_us;
        break;
    }
    case Poll: {
        Job* job = jobForHandle(sStage[Arg0]);
        if (!job) {
            sStage[Result] = BadHandle;
            break;
        }
        if (job->polls_remaining)
            --job->polls_remaining;
        sStage[Out0] = job->polls_remaining == 0 ? 1u : 0u;
        sStage[Out1] = job->polls_remaining;
        break;
    }
    case Collect: {
        Job* job = jobForHandle(sStage[Arg0]);
        if (!job) {
            sStage[Result] = BadHandle;
            break;
        }
        if (job->polls_remaining != 0) {
            sStage[Result] = NotReady;
            break;
        }
        sStage[Out0] = job->modeled_us;
        sStage[Out1] = job->speedup_x1000;
        sStage[Out2] = job->checksum;
        sStage[Out3] = job->software_us;
        break;
    }
    case Free: {
        Allocation* allocation = allocationForHandle(sStage[Arg0]);
        if (!allocation) {
            sStage[Result] = BadHandle;
            break;
        }
        allocation->live = false;
        allocation->uploaded = 0;
        break;
    }
    case Complete:
        sStage[Out0] = 0x53534150u;
        break;
    default:
        sStage[Result] = BadCommand;
        break;
    }
}

void processDescriptorBatch(u32 meaningfulBytes) {
    std::size_t cursor = 0;
    std::array<JobDescriptorV1, kMaxBatch> descriptors{};
    std::array<const u8*, kMaxBatch> inlineInputs{};
    std::size_t count = 0;

    while (cursor + sizeof(JobDescriptorV1) <= meaningfulBytes && count < kMaxBatch) {
        JobDescriptorV1 descriptor{};
        std::memcpy(&descriptor, sBlockTx.data() + cursor, sizeof(descriptor));
        if (descriptor.magic == 0)
            break;
        if (descriptor.magic != kDescriptorMagic || descriptor.version != kBlockVersion ||
            descriptor.opcode != kSubmitOpcode)
            return;
        cursor += sizeof(descriptor);
        const std::size_t inlineBytes =
            (descriptor.flags & kInlineInput) ? descriptor.input_length : 0;
        if (inlineBytes > meaningfulBytes - cursor)
            return;
        descriptors[count] = descriptor;
        inlineInputs[count] = inlineBytes ? sBlockTx.data() + cursor : nullptr;
        cursor += inlineBytes;
        ++count;
    }
    if (count == 0)
        return;
    sF4Parsed += static_cast<u32>(count);

    const u32 batchTransportX1000 = v1TransportUsX1000();
    const u32 transportShareX1000 = (batchTransportX1000 + count / 2) / count;
    const u32 transportShareUs = (transportShareX1000 + 500u) / 1000u;

    for (std::size_t i = 0; i < count; ++i) {
        const JobDescriptorV1& descriptor = descriptors[i];
        CompletionRecordV1 completion{};
        completion.magic = kCompletionMagic;
        completion.version = kBlockVersion;
        completion.status = Ok;
        completion.sequence = descriptor.sequence;
        completion.software_us = descriptor.arg0;
        completion.output_length = descriptor.output_length;
        completion.transport_us_x1000 = transportShareX1000;
        completion.batch_size = static_cast<u32>(count);

        Allocation* input = allocationForHandle(descriptor.input_handle);
        if (!input || descriptor.input_offset > input->size ||
            descriptor.input_length > input->size - descriptor.input_offset) {
            completion.status = BadHandle;
            (void)pushCompletion(completion);
            continue;
        }

        if (inlineInputs[i]) {
            std::copy_n(inlineInputs[i], descriptor.input_length,
                        sLocal.begin() + input->offset + descriptor.input_offset);
            input->uploaded = std::max(input->uploaded,
                                       descriptor.input_offset + descriptor.input_length);
        } else if (input->uploaded < descriptor.input_offset + descriptor.input_length) {
            completion.status = BadDescriptor;
            (void)pushCompletion(completion);
            continue;
        }

        Job* job = createJob();
        if (!job) {
            completion.status = NoMemory;
            (void)pushCompletion(completion);
            continue;
        }
        job->allocation = descriptor.input_handle;
        job->software_us = descriptor.arg0;
        job->checksum = fnv1a(sLocal.data() + input->offset + descriptor.input_offset,
                              descriptor.input_length);
        const u32 computeUs = static_cast<u32>(
            (static_cast<u64>(descriptor.work_units) * 1000000ull + kKernelOpsPerSecond / 2) /
            kKernelOpsPerSecond);
        job->modeled_us = computeUs + transportShareUs;
        job->speedup_x1000 = job->modeled_us
                                 ? static_cast<u32>((static_cast<u64>(job->software_us) * 1000u) /
                                                    job->modeled_us)
                                 : 0;
        job->polls_remaining = 0;

        completion.job_handle = job->handle;
        completion.modeled_us = job->modeled_us;
        completion.speedup_x1000 = job->speedup_x1000;
        completion.checksum = job->checksum;
        if (!pushCompletion(completion)) {
            job->live = false;
            break;
        }
    }
}

void blockWriteComplete(ntr_rom_emu_t* romEmu) {
    ++sF4Complete;
    const u32 meaningfulBytes = romEmu->cmd1 & 0xFFFFu;
    if (meaningfulBytes == 0 || meaningfulBytes > kBlockBytes)
        return;
    processDescriptorBatch(meaningfulBytes);
}

void finishCmd0(ntr_rom_emu_t* romEmu) {
    ntrc_finishGameNoScrambleCmd0(romEmu);
}

} // namespace

extern "C" void gekkopak_ntr_reset(void) {
    sStage.fill(0);
    sLegacyPayload.fill(0);
    sBlockTx.fill(0);
    sBlockRx.fill(0);
    sAllocations.fill(Allocation{});
    sJobs.fill(Job{});
    sCompletions.fill(CompletionRecordV1{});
    sNextJob = 1;
    sCompletionRead = sCompletionWrite = sCompletionCount = 0;
    sF4Enter = sF4Accepted = sF4Complete = sF4Parsed = 0;
}

extern "C" void ntrc_gekkopakWriteRegCmd0(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}
extern "C" void ntrc_gekkopakWriteRegCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio) {
    ntrc_noPayload(pio);
    if (commandMatches(romEmu, kWireWriteReg) && commandIndex(romEmu) < sStage.size())
        sStage[commandIndex(romEmu)] = word;
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

extern "C" void ntrc_gekkopakExecCmd0(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}
extern "C" void ntrc_gekkopakExecCmd1(ntr_rom_emu_t* romEmu, u32, pio_hw_t* pio) {
    ntrc_noPayload(pio);
    if (commandMatches(romEmu, kWireExec))
        executeHighCommand(commandIndex(romEmu));
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

extern "C" void ntrc_gekkopakReadRegCmd0(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}
extern "C" void ntrc_gekkopakReadRegCmd1(ntr_rom_emu_t* romEmu, u32, pio_hw_t* pio) {
    u32 value = 0;
    if (commandMatches(romEmu, kWireReadReg)) {
        const u8 index = commandIndex(romEmu);
        if (index == kEventCompletionDepth)
            value = sCompletionCount;
        else if (index == 0xF0)
            value = sF4Enter;
        else if (index == 0xF1)
            value = sF4Accepted;
        else if (index == 0xF2)
            value = sF4Complete;
        else if (index == 0xF3)
            value = sF4Parsed;
        else if (index < sStage.size())
            value = sStage[index];
    }
    ntrc_beginWrite(pio, 4);
    ntrc_writeWord(pio, value);
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

extern "C" void ntrc_gekkopakPayloadWordCmd0(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}
extern "C" void ntrc_gekkopakPayloadWordCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio) {
    ntrc_noPayload(pio);
    if (commandMatches(romEmu, kWirePayloadWord)) {
        const std::size_t offset = static_cast<std::size_t>(commandIndex(romEmu)) * 4;
        if (offset + 4 <= sLegacyPayload.size())
            std::memcpy(sLegacyPayload.data() + offset, &word, sizeof(word));
    }
    ntrc_finishGameNoScrambleCmd1(romEmu);
}

extern "C" void ntrc_gekkopakWriteBlockCmd0(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}
extern "C" void ntrc_gekkopakWriteBlockCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio) {
    ++sF4Enter;
    if (!commandMatches(romEmu, kWireWriteBlock) || commandIndex(romEmu) != 0 ||
        (word & 0xFFFFu) == 0 || (word & 0xFFFFu) > kBlockBytes) {
        ntrc_noPayload(pio);
        ntrc_finishGameNoScrambleCmd1(romEmu);
        return;
    }
    ++sF4Accepted;
    ntrc_beginRead(pio, kBlockBytes);
    ntrc_finishGameNoScrambleCmd1WithReadPayload(
        romEmu, reinterpret_cast<u32*>(sBlockTx.data()), kBlockBytes, blockWriteComplete);
}

extern "C" void ntrc_gekkopakReadBlockCmd0(ntr_rom_emu_t* romEmu, u32, pio_hw_t*) {
    finishCmd0(romEmu);
}
extern "C" void ntrc_gekkopakReadBlockCmd1(ntr_rom_emu_t* romEmu, u32 word, pio_hw_t* pio) {
    sBlockRx.fill(0);
    if (commandMatches(romEmu, kWireReadBlock) && commandIndex(romEmu) == 1) {
        const u32 offset = word & 0xFFFFu;
        const u32 meaningfulBytes = word >> 16;
        if (offset == 0 && meaningfulBytes != 0 && meaningfulBytes <= kBlockBytes) {
            std::size_t written = 0;
            CompletionRecordV1 completion{};
            while (written + sizeof(completion) <= meaningfulBytes && popCompletion(&completion)) {
                std::memcpy(sBlockRx.data() + written, &completion, sizeof(completion));
                written += sizeof(completion);
            }
        }
    }
    ntrc_beginWrite(pio, kBlockBytes);
    ntrc_dmaToBus(sBlockRx.data(), kBlockBytes);
    ntrc_finishGameNoScrambleCmd1(romEmu);
}
