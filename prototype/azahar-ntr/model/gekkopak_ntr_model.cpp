#include "gekkopak_ntr_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gekkopak::ntr {
namespace {
constexpr double kBytesPerSecond = 6.0 * 1024.0 * 1024.0;
constexpr double kCommandLatencyUs = 25.0;
constexpr double kKernelOpsPerSecond = 100'000'000.0;
} // namespace

Device::Device() {
    Store32(kRegRomCnt, kCardResetHigh);
}

std::uint32_t Device::Load32(std::size_t off) const {
    std::uint32_t v = 0;
    std::memcpy(&v, regs_.data() + off, sizeof(v));
    return v;
}

void Device::Store32(std::size_t off, std::uint32_t value) {
    std::memcpy(regs_.data() + off, &value, sizeof(value));
}

void Device::Tick() {
    const std::uint32_t romcnt = Load32(kRegRomCnt);
    if ((romcnt & kCardActivate) == 0)
        return;

    std::uint8_t cmd[8]{};
    std::memcpy(cmd, regs_.data() + kRegCommand, sizeof(cmd));

    Store32(kRegRomCnt, romcnt & ~(kCardActivate | kCardDataReady));
    ProcessWireCommand(cmd);
    ++transfers_;
}

void Device::ProcessWireCommand(const std::uint8_t cmd[8]) {
    if (cmd[1] != kMagic0 || cmd[2] != kMagic1)
        return;

    std::uint32_t word = 0;
    std::memcpy(&word, cmd + 4, sizeof(word));

    switch (cmd[0]) {
    case kWireWriteReg:
        if (cmd[3] < stage_.size())
            stage_[cmd[3]] = word;
        break;
    case kWireExec:
        ExecuteHighCommand(cmd[3], word);
        break;
    case kWireReadReg:
        if (cmd[3] < stage_.size()) {
            Store32(kRegFifo, stage_[cmd[3]]);
            Store32(kRegRomCnt, Load32(kRegRomCnt) | kCardDataReady);
        }
        break;
    case kWireWritePayloadWord: {
        const std::size_t offset = static_cast<std::size_t>(cmd[3]) * 4;
        if (offset + 4 <= payload_.size())
            std::memcpy(payload_.data() + offset, &word, sizeof(word));
        break;
    }
    default:
        break;
    }
}

std::uint32_t Device::Fnv1a(const std::uint8_t* data, std::size_t size) {
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

std::uint32_t Device::ModelJobUs(std::uint32_t tx_bytes, std::uint32_t rx_bytes,
                                 std::uint32_t operations) const {
    const double tx = (static_cast<double>(tx_bytes) / kBytesPerSecond) * 1'000'000.0;
    const double rx = (static_cast<double>(rx_bytes) / kBytesPerSecond) * 1'000'000.0;
    const double compute = (static_cast<double>(operations) / kKernelOpsPerSecond) * 1'000'000.0;
    return static_cast<std::uint32_t>(kCommandLatencyUs + tx + rx + compute + 0.5);
}

double Device::V1BatchTransportUs() {
    // One 512-byte F4 write, one four-byte event read, and one 512-byte F5 read.
    constexpr double bytes = static_cast<double>(kBlockBytes + 4 + kBlockBytes);
    return 3.0 * kCommandLatencyUs + (bytes / kBytesPerSecond) * 1'000'000.0;
}

bool Device::WriteBlock(BlockSelector selector, const std::uint8_t* data, std::size_t len,
                        std::uint16_t /*wire_flags*/) {
    ++transfers_;
    if (selector != BlockSelector::Submission || data == nullptr || len == 0 || len > kBlockBytes)
        return false;

    std::vector<JobDescriptorV1> descriptors;
    std::vector<const std::uint8_t*> inline_inputs;
    std::size_t cursor = 0;

    while (cursor + sizeof(JobDescriptorV1) <= len &&
           descriptors.size() < kMaxDescriptorsPerBlock) {
        JobDescriptorV1 desc{};
        std::memcpy(&desc, data + cursor, sizeof(desc));
        if (desc.magic == 0)
            break;
        if (desc.magic != kDescriptorMagic || desc.version != kBlockProtocolVersion ||
            desc.opcode != kDescriptorOpcodeSubmit)
            return false;

        cursor += sizeof(desc);
        const bool inline_input = (desc.flags & kDescriptorFlagInlineInput) != 0;
        const std::size_t inline_len = inline_input ? desc.input_length : 0;
        if (inline_len > len - cursor)
            return false;

        descriptors.push_back(desc);
        inline_inputs.push_back(inline_input ? data + cursor : nullptr);
        cursor += inline_len;
    }

    if (descriptors.empty())
        return false;

    ProcessDescriptorBatch(descriptors, inline_inputs);
    return true;
}

void Device::ProcessDescriptorBatch(const std::vector<JobDescriptorV1>& descriptors,
                                    const std::vector<const std::uint8_t*>& inline_inputs) {
    const std::uint32_t batch_size = static_cast<std::uint32_t>(descriptors.size());
    const double transport_share_us = V1BatchTransportUs() / static_cast<double>(batch_size);
    const std::uint32_t transport_us_x1000 =
        static_cast<std::uint32_t>(std::llround(transport_share_us * 1000.0));

    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        const auto& desc = descriptors[i];
        CompletionRecordV1 completion{};
        completion.sequence = desc.sequence;
        completion.software_us = desc.arg0;
        completion.output_length = desc.output_length;
        completion.transport_us_x1000 = transport_us_x1000;
        completion.batch_size = batch_size;

        auto input_it = allocations_.find(desc.input_handle);
        if (input_it == allocations_.end() || desc.input_offset > input_it->second.data.size() ||
            desc.input_length > input_it->second.data.size() - desc.input_offset) {
            completion.status = BadHandle;
            completions_.push_back(completion);
            continue;
        }

        auto& allocation = input_it->second;
        if (inline_inputs[i] != nullptr) {
            std::copy_n(inline_inputs[i], desc.input_length,
                        allocation.data.begin() + desc.input_offset);
            allocation.uploaded = std::max(
                allocation.uploaded,
                static_cast<std::size_t>(desc.input_offset + desc.input_length));
        } else if (allocation.uploaded <
                   static_cast<std::size_t>(desc.input_offset + desc.input_length)) {
            completion.status = BadDescriptor;
            completions_.push_back(completion);
            continue;
        }

        if (desc.output_handle != 0) {
            auto output_it = allocations_.find(desc.output_handle);
            if (output_it == allocations_.end() ||
                desc.output_offset > output_it->second.data.size() ||
                desc.output_length > output_it->second.data.size() - desc.output_offset) {
                completion.status = BadHandle;
                completions_.push_back(completion);
                continue;
            }
        }

        const std::uint32_t handle = next_job_++;
        Job job;
        job.allocation = desc.input_handle;
        job.kernel = desc.kernel_id;
        job.operations = desc.work_units;
        job.software_us = desc.arg0;
        job.checksum = Fnv1a(allocation.data.data() + desc.input_offset, desc.input_length);

        const double compute_us =
            (static_cast<double>(job.operations) / kKernelOpsPerSecond) * 1'000'000.0;
        job.modeled_us =
            static_cast<std::uint32_t>(std::llround(compute_us + transport_share_us));
        job.speedup_x1000 = job.modeled_us
                                ? static_cast<std::uint32_t>(
                                      (static_cast<std::uint64_t>(job.software_us) * 1000u) /
                                      job.modeled_us)
                                : 0;
        job.ready = true;
        job.polls_remaining = 0;
        jobs_.emplace(handle, job);

        completion.job_handle = handle;
        completion.modeled_us = job.modeled_us;
        completion.speedup_x1000 = job.speedup_x1000;
        completion.checksum = job.checksum;
        completions_.push_back(completion);
    }
}

std::uint32_t Device::ReadEvent() {
    ++transfers_;
    return static_cast<std::uint32_t>(std::min<std::size_t>(completions_.size(), 0xFFFFu));
}

std::size_t Device::ReadBlock(BlockSelector selector, std::uint16_t offset, std::uint8_t* out,
                              std::size_t len) {
    ++transfers_;
    if (selector != BlockSelector::Completion || offset != 0 || out == nullptr || len == 0 ||
        len > kBlockBytes)
        return 0;

    std::memset(out, 0, len);
    std::size_t written = 0;
    while (!completions_.empty() && written + sizeof(CompletionRecordV1) <= len) {
        std::memcpy(out + written, &completions_.front(), sizeof(CompletionRecordV1));
        completions_.pop_front();
        written += sizeof(CompletionRecordV1);
    }
    return written;
}

void Device::ExecuteHighCommand(std::uint8_t command, std::uint32_t /*seq*/) {
    stage_[Result] = Ok;
    stage_[Out0] = stage_[Out1] = stage_[Out2] = stage_[Out3] = 0;

    switch (command) {
    case Hello:
        stage_[Out0] = kProtocolVersion;
        stage_[Out1] = kCaps;
        stage_[Out2] = kLocalMemoryBytes;
        stage_[Out3] = 2;
        break;
    case GetCaps:
        stage_[Out0] = kCaps;
        stage_[Out1] = kLocalMemoryBytes;
        stage_[Out2] = 6u * 1024u * 1024u;
        stage_[Out3] = 25;
        break;
    case Alloc: {
        const std::uint32_t bytes = stage_[Arg0];
        if (bytes == 0 || bytes > kLocalMemoryBytes || allocated_bytes_ + bytes > kLocalMemoryBytes) {
            stage_[Result] = NoMemory;
            break;
        }
        const std::uint32_t handle = next_alloc_++;
        allocations_.emplace(handle, Allocation{std::vector<std::uint8_t>(bytes), 0});
        allocated_bytes_ += bytes;
        stage_[Out0] = handle;
        stage_[Out1] = bytes;
        break;
    }
    case Upload: {
        auto it = allocations_.find(stage_[Arg0]);
        const std::uint32_t offset = stage_[Arg1];
        const std::uint32_t len = stage_[PayloadLen];
        if (it == allocations_.end() || len > payload_.size() || offset > it->second.data.size() ||
            len > it->second.data.size() - offset) {
            stage_[Result] = BadHandle;
            break;
        }
        std::copy_n(payload_.begin(), len, it->second.data.begin() + offset);
        it->second.uploaded = std::max(it->second.uploaded, static_cast<std::size_t>(offset + len));
        stage_[Out0] = len;
        stage_[Out1] = Fnv1a(it->second.data.data(), it->second.uploaded);
        break;
    }
    case Submit: {
        auto it = allocations_.find(stage_[Arg0]);
        if (it == allocations_.end()) {
            stage_[Result] = BadHandle;
            break;
        }
        const std::uint32_t handle = next_job_++;
        Job job;
        job.allocation = stage_[Arg0];
        job.kernel = stage_[Arg1];
        job.operations = stage_[Arg2];
        job.software_us = stage_[Arg3];
        job.checksum = Fnv1a(it->second.data.data(), it->second.uploaded);
        job.modeled_us = ModelJobUs(static_cast<std::uint32_t>(it->second.uploaded), 16,
                                    job.operations);
        job.speedup_x1000 = job.modeled_us
                                ? static_cast<std::uint32_t>(
                                      (static_cast<std::uint64_t>(job.software_us) * 1000u) /
                                      job.modeled_us)
                                : 0;
        jobs_.emplace(handle, job);
        stage_[Out0] = handle;
        stage_[Out1] = job.modeled_us;
        break;
    }
    case Poll: {
        auto it = jobs_.find(stage_[Arg0]);
        if (it == jobs_.end()) {
            stage_[Result] = BadHandle;
            break;
        }
        if (!it->second.ready) {
            if (it->second.polls_remaining > 0)
                --it->second.polls_remaining;
            if (it->second.polls_remaining == 0)
                it->second.ready = true;
        }
        stage_[Out0] = it->second.ready ? 1u : 0u;
        stage_[Out1] = static_cast<std::uint32_t>(it->second.polls_remaining);
        break;
    }
    case Collect: {
        auto it = jobs_.find(stage_[Arg0]);
        if (it == jobs_.end()) {
            stage_[Result] = BadHandle;
            break;
        }
        if (!it->second.ready) {
            stage_[Result] = NotReady;
            break;
        }
        stage_[Out0] = it->second.modeled_us;
        stage_[Out1] = it->second.speedup_x1000;
        stage_[Out2] = it->second.checksum;
        stage_[Out3] = it->second.software_us;
        break;
    }
    case Free: {
        auto it = allocations_.find(stage_[Arg0]);
        if (it == allocations_.end()) {
            stage_[Result] = BadHandle;
            break;
        }
        allocated_bytes_ -= it->second.data.size();
        allocations_.erase(it);
        break;
    }
    case Complete:
        completed_ = true;
        stage_[Out0] = 0x53534150u;
        break;
    default:
        stage_[Result] = BadCommand;
        break;
    }
}

} // namespace gekkopak::ntr
