#include "gekkopak_ntr_model.h"

#include <algorithm>
#include <cstring>

namespace gekkopak::ntr {

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
    const double tx = (static_cast<double>(tx_bytes) / (6.0 * 1024.0 * 1024.0)) * 1'000'000.0;
    const double rx = (static_cast<double>(rx_bytes) / (6.0 * 1024.0 * 1024.0)) * 1'000'000.0;
    const double compute = (static_cast<double>(operations) / 100'000'000.0) * 1'000'000.0;
    return static_cast<std::uint32_t>(25.0 + tx + rx + compute + 0.5);
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
