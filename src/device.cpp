#include "gekkopak/device.h"

#include <cstring>

namespace gekkopak {

using namespace protocol;

namespace {

u32 AlignUp4(u32 value) {
    return (value + 3u) & ~3u;
}

u32 Max32(u32 a, u32 b) {
    return a > b ? a : b;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void Device::Reset(const Config& config) {
    config_ = config;
    if (config_.reported_local_bytes == 0) {
        config_.reported_local_bytes = config_.pool_bytes;
    }

    std::memset(stage_, 0, sizeof(stage_));
    std::memset(payload_, 0, sizeof(payload_));
    std::memset(allocations_, 0, sizeof(allocations_));
    std::memset(jobs_, 0, sizeof(jobs_));
    for (std::size_t i = 0; i < kCompletionQueueDepth; ++i) {
        completions_[i] = CompletionRecordV1{};
    }
    if (config_.pool != nullptr && config_.pool_bytes != 0) {
        std::memset(config_.pool, 0, config_.pool_bytes);
    }
    completion_read_ = 0;
    completion_write_ = 0;
    completion_count_ = 0;
    next_job_ = 1;
    transfers_ = 0;
    completed_ = false;
}

// ---------------------------------------------------------------------------
// Allocator
// ---------------------------------------------------------------------------

Device::Allocation* Device::AllocationForHandle(u32 handle) {
    if (handle == 0 || handle > kMaxAllocations) {
        return nullptr;
    }
    Allocation& allocation = allocations_[handle - 1];
    return allocation.live ? &allocation : nullptr;
}

const Device::Allocation* Device::AllocationForHandle(u32 handle) const {
    return const_cast<Device*>(this)->AllocationForHandle(handle);
}

u32 Device::Allocate(u32 bytes) {
    // Check before rounding up: AlignUp4 wraps near UINT32_MAX, and a request
    // that wrapped to a small number would be granted a buffer far smaller than
    // it asked for.
    if (bytes == 0 || bytes > config_.pool_bytes || config_.pool == nullptr) {
        return 0;
    }
    bytes = AlignUp4(bytes);
    if (bytes > config_.pool_bytes) {
        return 0; // rounding up crossed the end of the pool
    }

    std::size_t slot = kMaxAllocations;
    for (std::size_t i = 0; i < kMaxAllocations; ++i) {
        if (!allocations_[i].live) {
            slot = i;
            break;
        }
    }
    if (slot == kMaxAllocations) {
        return 0;
    }

    u32 candidate = 0;
    while (candidate + bytes <= config_.pool_bytes) {
        bool overlap = false;
        u32 bump = candidate;
        for (std::size_t i = 0; i < kMaxAllocations; ++i) {
            const Allocation& allocation = allocations_[i];
            if (!allocation.live) {
                continue;
            }
            const u32 end = candidate + bytes;
            const u32 allocation_end = allocation.offset + allocation.size;
            if (end <= allocation.offset || candidate >= allocation_end) {
                continue;
            }
            overlap = true;
            bump = Max32(bump, allocation_end);
        }
        if (!overlap) {
            allocations_[slot].offset = candidate;
            allocations_[slot].size = bytes;
            allocations_[slot].uploaded = 0;
            allocations_[slot].live = true;
            return static_cast<u32>(slot + 1);
        }
        candidate = AlignUp4(bump);
    }
    return 0;
}

Device::Job* Device::JobForHandle(u32 handle) {
    if (handle == 0) {
        return nullptr;
    }
    for (std::size_t i = 0; i < kMaxJobs; ++i) {
        if (jobs_[i].live && jobs_[i].handle == handle) {
            return &jobs_[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Completion queue
// ---------------------------------------------------------------------------

bool Device::PushCompletion(const CompletionRecordV1& record) {
    if (completion_count_ >= kCompletionQueueDepth) {
        return false;
    }
    completions_[completion_write_] = record;
    completion_write_ = (completion_write_ + 1) % kCompletionQueueDepth;
    ++completion_count_;
    return true;
}

bool Device::PopCompletion(CompletionRecordV1* out) {
    if (completion_count_ == 0) {
        return false;
    }
    *out = completions_[completion_read_];
    completion_read_ = (completion_read_ + 1) % kCompletionQueueDepth;
    --completion_count_;
    return true;
}

// ---------------------------------------------------------------------------
// Integer timing model
// ---------------------------------------------------------------------------

u32 Device::TransferNs(u32 bytes) const {
    const u32 bps = config_.timing.bus_bytes_per_second;
    if (bps == 0) {
        return 0;
    }
    return static_cast<u32>((static_cast<u64>(bytes) * 1000000000ull + bps / 2) / bps);
}

u32 Device::ComputeNs(u32 operations) const {
    const u32 hz = config_.timing.kernel_ops_per_second;
    if (hz == 0) {
        return 0;
    }
    return static_cast<u32>((static_cast<u64>(operations) * 1000000000ull + hz / 2) / hz);
}

u32 Device::LegacyModelUs(u32 tx_bytes, u32 rx_bytes, u32 operations) const {
    const u32 ns = config_.timing.command_latency_ns + TransferNs(tx_bytes) +
                   TransferNs(rx_bytes) + ComputeNs(operations);
    return NsToUs(ns);
}

u32 Device::BlockTransportNs() const {
    // One 512-byte F4 write, one four-byte F2 event read, one 512-byte F5 read.
    const u32 bytes = static_cast<u32>(kBlockBytes + 4 + kBlockBytes);
    return 3u * config_.timing.command_latency_ns + TransferNs(bytes);
}

// ---------------------------------------------------------------------------
// F0 / F2 / F3
// ---------------------------------------------------------------------------

void Device::WriteReg(u8 index, u32 value) {
    if (index < kStageRegisterCount) {
        stage_[index] = value;
    }
}

u32 Device::ReadReg(u8 index) const {
    if (index == kEventCompletionDepth) {
        return completion_count_;
    }
    if (index < kStageRegisterCount) {
        return stage_[index];
    }
    return 0;
}

void Device::WritePayloadWord(u8 index, u32 value) {
    const std::size_t offset = static_cast<std::size_t>(index) * 4u;
    if (offset + 4u <= kPayloadBytes) {
        std::memcpy(payload_ + offset, &value, sizeof(value));
    }
}

// ---------------------------------------------------------------------------
// F1
// ---------------------------------------------------------------------------

void Device::Exec(u8 command, u32 sequence) {
    (void)sequence;
    stage_[kResult] = kOk;
    stage_[kOut0] = 0;
    stage_[kOut1] = 0;
    stage_[kOut2] = 0;
    stage_[kOut3] = 0;

    switch (command) {
    case kHello:
        stage_[kOut0] = kProtocolVersion;
        stage_[kOut1] = config_.capabilities;
        stage_[kOut2] = config_.reported_local_bytes;
        stage_[kOut3] = 2; // transport generation
        break;

    case kGetCaps:
        stage_[kOut0] = config_.capabilities;
        stage_[kOut1] = config_.reported_local_bytes;
        stage_[kOut2] = config_.timing.bus_bytes_per_second;
        stage_[kOut3] = config_.timing.command_latency_ns / 1000u;
        break;

    case kAlloc: {
        const u32 handle = Allocate(stage_[kArg0]);
        if (handle == 0) {
            stage_[kResult] = kNoMemory;
            break;
        }
        stage_[kOut0] = handle;
        stage_[kOut1] = AllocationForHandle(handle)->size;
        break;
    }

    case kUpload: {
        Allocation* allocation = AllocationForHandle(stage_[kArg0]);
        const u32 offset = stage_[kArg1];
        const u32 len = stage_[kPayloadLen];
        if (allocation == nullptr || len > kPayloadBytes || offset > allocation->size ||
            len > allocation->size - offset) {
            stage_[kResult] = kBadHandle;
            break;
        }
        std::memcpy(config_.pool + allocation->offset + offset, payload_, len);
        allocation->uploaded = Max32(allocation->uploaded, offset + len);
        stage_[kOut0] = len;
        stage_[kOut1] = Fnv1a(config_.pool + allocation->offset, allocation->uploaded);
        break;
    }

    case kSubmit: {
        Allocation* allocation = AllocationForHandle(stage_[kArg0]);
        if (allocation == nullptr) {
            stage_[kResult] = kBadHandle;
            break;
        }
        std::size_t slot = kMaxJobs;
        for (std::size_t i = 0; i < kMaxJobs; ++i) {
            if (!jobs_[i].live) {
                slot = i;
                break;
            }
        }
        if (slot == kMaxJobs) {
            stage_[kResult] = kQueueFull;
            break;
        }

        Job& job = jobs_[slot];
        job.allocation = stage_[kArg0];
        job.software_us = stage_[kArg3];
        job.checksum = Fnv1a(config_.pool + allocation->offset, allocation->uploaded);
        job.modeled_us = LegacyModelUs(allocation->uploaded, 16, stage_[kArg2]);
        job.speedup_x1000 =
            job.modeled_us != 0
                ? static_cast<u32>((static_cast<u64>(job.software_us) * 1000ull) / job.modeled_us)
                : 0;
        job.polls_remaining = 2;
        job.ready = false;
        job.live = true;
        job.handle = next_job_++;

        stage_[kOut0] = job.handle;
        stage_[kOut1] = job.modeled_us;
        break;
    }

    case kPoll: {
        Job* job = JobForHandle(stage_[kArg0]);
        if (job == nullptr) {
            stage_[kResult] = kBadHandle;
            break;
        }
        if (!job->ready) {
            if (job->polls_remaining > 0) {
                --job->polls_remaining;
            }
            if (job->polls_remaining == 0) {
                job->ready = true;
            }
        }
        stage_[kOut0] = job->ready ? 1u : 0u;
        stage_[kOut1] = job->polls_remaining;
        break;
    }

    case kCollect: {
        Job* job = JobForHandle(stage_[kArg0]);
        if (job == nullptr) {
            stage_[kResult] = kBadHandle;
            break;
        }
        if (!job->ready) {
            stage_[kResult] = kNotReady;
            break;
        }
        stage_[kOut0] = job->modeled_us;
        stage_[kOut1] = job->speedup_x1000;
        stage_[kOut2] = job->checksum;
        stage_[kOut3] = job->software_us;
        break;
    }

    case kFree: {
        Allocation* allocation = AllocationForHandle(stage_[kArg0]);
        if (allocation == nullptr) {
            stage_[kResult] = kBadHandle;
            break;
        }
        allocation->live = false;
        allocation->uploaded = 0;
        break;
    }

    case kComplete:
        completed_ = true;
        stage_[kOut0] = kCompleteAck;
        break;

    default:
        stage_[kResult] = kBadCommand;
        break;
    }
}

// ---------------------------------------------------------------------------
// F4 / F5
// ---------------------------------------------------------------------------

u32 Device::ProcessDescriptorBlock(const u8* data, std::size_t len) {
    JobDescriptorV1 descriptors[kMaxDescriptorsPerBlock];
    const u8* inline_inputs[kMaxDescriptorsPerBlock];
    std::size_t count = 0;
    std::size_t cursor = 0;

    while (cursor + sizeof(JobDescriptorV1) <= len && count < kMaxDescriptorsPerBlock) {
        JobDescriptorV1 desc{};
        std::memcpy(&desc, data + cursor, sizeof(desc));
        if (desc.magic == 0) {
            break; // zero padding terminates the block
        }
        if (desc.magic != kDescriptorMagic || desc.version != kBlockProtocolVersion ||
            desc.opcode != kDescriptorOpcodeSubmit) {
            return kBadDescriptor;
        }

        cursor += sizeof(desc);
        const bool inline_input = (desc.flags & kDescriptorFlagInlineInput) != 0;
        const std::size_t inline_len = inline_input ? desc.input_length : 0;
        if (inline_len > len - cursor) {
            return kBadBlock;
        }

        descriptors[count] = desc;
        inline_inputs[count] = inline_input ? data + cursor : nullptr;
        ++count;
        cursor += inline_len;
    }

    if (count == 0) {
        return kBadDescriptor;
    }

    // A completion the queue could not hold is not a slow answer, it is a lost
    // one. Report it rather than leaving the client to conclude the job never
    // ran; on hardware that distinction is expensive to make.
    bool dropped = false;

    // The whole batch shares one block round trip, so each job is charged its
    // share of it. This is what makes batching visible in the modeled numbers.
    const u32 batch_size = static_cast<u32>(count);
    const u32 transport_ns = BlockTransportNs();
    const u32 transport_share_ns = (transport_ns + batch_size / 2u) / batch_size;

    for (std::size_t i = 0; i < count; ++i) {
        const JobDescriptorV1& desc = descriptors[i];
        CompletionRecordV1 completion{};
        completion.magic = kCompletionMagic;
        completion.version = kBlockProtocolVersion;
        completion.status = static_cast<u16>(kOk);
        completion.sequence = desc.sequence;
        completion.software_us = desc.arg0;
        completion.output_length = desc.output_length;
        completion.transport_us_x1000 = transport_share_ns;
        completion.batch_size = batch_size;

        Allocation* input = AllocationForHandle(desc.input_handle);
        if (input == nullptr || desc.input_offset > input->size ||
            desc.input_length > input->size - desc.input_offset) {
            completion.status = static_cast<u16>(kBadHandle);
            dropped |= !PushCompletion(completion);
            continue;
        }

        if (inline_inputs[i] != nullptr) {
            std::memcpy(config_.pool + input->offset + desc.input_offset, inline_inputs[i],
                        desc.input_length);
            input->uploaded = Max32(input->uploaded, desc.input_offset + desc.input_length);
        } else if (input->uploaded < desc.input_offset + desc.input_length) {
            completion.status = static_cast<u16>(kBadDescriptor);
            dropped |= !PushCompletion(completion);
            continue;
        }

        if (desc.output_handle != 0) {
            const Allocation* output = AllocationForHandle(desc.output_handle);
            if (output == nullptr || desc.output_offset > output->size ||
                desc.output_length > output->size - desc.output_offset) {
                completion.status = static_cast<u16>(kBadHandle);
                dropped |= !PushCompletion(completion);
                continue;
            }
        }

        std::size_t slot = kMaxJobs;
        for (std::size_t j = 0; j < kMaxJobs; ++j) {
            if (!jobs_[j].live) {
                slot = j;
                break;
            }
        }
        if (slot == kMaxJobs) {
            completion.status = static_cast<u16>(kQueueFull);
            dropped |= !PushCompletion(completion);
            continue;
        }

        Job& job = jobs_[slot];
        job.allocation = desc.input_handle;
        job.software_us = desc.arg0;
        job.checksum = Fnv1a(config_.pool + input->offset + desc.input_offset, desc.input_length);
        job.modeled_us = NsToUs(ComputeNs(desc.work_units) + transport_share_ns);
        job.speedup_x1000 =
            job.modeled_us != 0
                ? static_cast<u32>((static_cast<u64>(job.software_us) * 1000ull) / job.modeled_us)
                : 0;
        // Block-submitted jobs complete synchronously: the completion record is
        // the answer, so there is nothing left to poll for.
        job.polls_remaining = 0;
        job.ready = true;
        job.live = true;
        job.handle = next_job_++;

        completion.job_handle = job.handle;
        completion.modeled_us = job.modeled_us;
        completion.speedup_x1000 = job.speedup_x1000;
        completion.checksum = job.checksum;
        dropped |= !PushCompletion(completion);
    }

    return dropped ? kQueueFull : kOk;
}

u32 Device::ValidateReadBlock(u8 selector, u32 word) {
    const u32 offset = word & 0xFFFFu;
    const u32 len = word >> 16;
    const u32 status =
        (selector != kSelectorCompletion || offset != 0 || len == 0 || len > kBlockBytes)
            ? static_cast<u32>(kBadBlock)
            : static_cast<u32>(kOk);
    stage_[kResult] = status;
    return status;
}

std::size_t Device::PeekBlock(u8 selector, u32 word, u8* out, std::size_t out_bytes) const {
    const u32 offset = word & 0xFFFFu;
    const u32 len = word >> 16;
    if (out == nullptr || out_bytes == 0) {
        return 0;
    }
    std::memset(out, 0, out_bytes);
    if (selector != kSelectorCompletion || offset != 0 || len == 0 || len > kBlockBytes) {
        return 0;
    }

    const std::size_t limit = len < out_bytes ? len : out_bytes;
    std::size_t written = 0;
    u32 index = completion_read_;
    u32 remaining = completion_count_;
    while (written + sizeof(CompletionRecordV1) <= limit && remaining != 0) {
        std::memcpy(out + written, &completions_[index], sizeof(CompletionRecordV1));
        index = (index + 1) % kCompletionQueueDepth;
        --remaining;
        written += sizeof(CompletionRecordV1);
    }
    return written;
}

void Device::DropCompletions(u32 count) {
    while (count != 0 && completion_count_ != 0) {
        completion_read_ = (completion_read_ + 1) % kCompletionQueueDepth;
        --completion_count_;
        --count;
    }
}

u32 Device::WriteBlock(u8 selector, u32 word, const u8* data, std::size_t data_bytes) {
    const u32 len = word & 0xFFFFu;
    if (selector != kSelectorSubmission || data == nullptr || len == 0 || len > kBlockBytes ||
        len > data_bytes) {
        stage_[kResult] = kBadBlock;
        return kBadBlock;
    }
    const u32 status = ProcessDescriptorBlock(data, len);
    stage_[kResult] = status;
    return status;
}

std::size_t Device::ReadBlock(u8 selector, u32 word, u8* out, std::size_t out_bytes) {
    const u32 offset = word & 0xFFFFu;
    const u32 len = word >> 16;
    if (out == nullptr || out_bytes == 0) {
        stage_[kResult] = kBadBlock;
        return 0;
    }
    std::memset(out, 0, out_bytes);
    if (selector != kSelectorCompletion || offset != 0 || len == 0 || len > kBlockBytes) {
        stage_[kResult] = kBadBlock;
        return 0;
    }

    const std::size_t limit = len < out_bytes ? len : out_bytes;
    std::size_t written = 0;
    CompletionRecordV1 record{};
    while (written + sizeof(record) <= limit && PopCompletion(&record)) {
        std::memcpy(out + written, &record, sizeof(record));
        written += sizeof(record);
    }
    stage_[kResult] = kOk;
    return written;
}

} // namespace gekkopak
