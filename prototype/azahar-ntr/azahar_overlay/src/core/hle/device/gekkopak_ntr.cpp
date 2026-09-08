#include "core/hle/device/gekkopak_ntr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>
#include "common/logging/log.h"
#include "common/memory_ref.h"

namespace GekkoPakNtr {
namespace {

constexpr u32 RegRomCnt = 0x04;
constexpr u32 RegCommand = 0x08;
constexpr u32 RegFifo = 0x1C;
constexpr u32 RegBlockTx = 0x100;
constexpr u32 RegBlockRx = 0x300;
constexpr u32 CardActivate = 1u << 31;
constexpr u32 CardDataReady = 1u << 23;

constexpr u8 WireWriteReg = 0xF0;
constexpr u8 WireExec = 0xF1;
constexpr u8 WireReadReg = 0xF2;
constexpr u8 WireWritePayloadWord = 0xF3;
constexpr u8 WireWriteBlock = 0xF4;
constexpr u8 WireReadBlock = 0xF5;
constexpr u8 EventCompletionDepth = 0xFE;
constexpr u8 Magic0 = 0x47;
constexpr u8 Magic1 = 0x4B;

constexpr u32 ProtocolVersion = 0x00010000u;
constexpr u32 BaseCapabilities = 0x0000000Fu;
constexpr u32 CapBlockTransport = 1u << 4;
constexpr u32 Capabilities = BaseCapabilities | CapBlockTransport;
constexpr u32 LocalMemoryBytes = 32u * 1024u * 1024u;
constexpr std::size_t BlockBytes = 512;
constexpr std::size_t DescriptorBytes = 64;
constexpr std::size_t CompletionBytes = 64;
constexpr std::size_t MaxDescriptorsPerBlock = BlockBytes / DescriptorBytes;
constexpr u32 DescriptorMagic = 0x31444B47u;
constexpr u32 CompletionMagic = 0x31434B47u;
constexpr u16 BlockProtocolVersion = 1;
constexpr u16 DescriptorOpcodeSubmit = 1;
constexpr u32 DescriptorFlagInlineInput = 1u << 0;
constexpr double BytesPerSecond = 6.0 * 1024.0 * 1024.0;
constexpr double CommandLatencyUs = 25.0;
constexpr double KernelOpsPerSecond = 100'000'000.0;

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
};

struct JobDescriptorV1 {
    u32 magic{DescriptorMagic};
    u16 version{BlockProtocolVersion};
    u16 opcode{DescriptorOpcodeSubmit};
    u32 sequence{};
    u32 flags{};
    u32 input_handle{};
    u32 input_offset{};
    u32 input_length{};
    u32 output_handle{};
    u32 output_offset{};
    u32 output_length{};
    u32 kernel_id{};
    u32 work_units{};
    u32 arg0{};
    u32 arg1{};
    u32 arg2{};
    u32 arg3{};
};
static_assert(sizeof(JobDescriptorV1) == DescriptorBytes);

struct CompletionRecordV1 {
    u32 magic{CompletionMagic};
    u16 version{BlockProtocolVersion};
    u16 status{static_cast<u16>(Ok)};
    u32 sequence{};
    u32 job_handle{};
    u32 modeled_us{};
    u32 speedup_x1000{};
    u32 checksum{};
    u32 software_us{};
    u32 output_length{};
    u32 transport_us_x1000{};
    u32 batch_size{};
    u32 reserved[5]{};
};
static_assert(sizeof(CompletionRecordV1) == CompletionBytes);

struct Allocation {
    std::vector<u8> data;
    std::size_t uploaded{};
};

struct Job {
    u32 allocation{};
    u32 kernel{};
    u32 operations{};
    u32 software_us{};
    u32 modeled_us{};
    u32 speedup_x1000{};
    u32 checksum{};
    int polls_remaining{2};
    bool ready{};
};

struct State {
    std::shared_ptr<BufferMem> regs = std::make_shared<BufferMem>(RegisterPageSize);
    std::array<u32, 16> stage{};
    std::array<u8, 1024> payload{};
    std::unordered_map<u32, Allocation> allocations;
    std::unordered_map<u32, Job> jobs;
    std::deque<CompletionRecordV1> completions;
    u32 next_alloc{1};
    u32 next_job{1};
    u64 allocated_bytes{};
    u64 transfers{};
    bool completed{};
};

State& GetState() {
    static State state;
    return state;
}

u32 Load32(std::size_t offset) {
    u32 value{};
    auto& vec = GetState().regs->Vector();
    std::memcpy(&value, vec.data() + offset, sizeof(value));
    return value;
}

void Store32(std::size_t offset, u32 value) {
    auto& vec = GetState().regs->Vector();
    std::memcpy(vec.data() + offset, &value, sizeof(value));
}

u32 Fnv1a(const u8* data, std::size_t size) {
    u32 hash = 2166136261u;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

u32 ModelJobUs(u32 tx_bytes, u32 rx_bytes, u32 operations) {
    const double tx = (static_cast<double>(tx_bytes) / BytesPerSecond) * 1'000'000.0;
    const double rx = (static_cast<double>(rx_bytes) / BytesPerSecond) * 1'000'000.0;
    const double compute = (static_cast<double>(operations) / KernelOpsPerSecond) * 1'000'000.0;
    return static_cast<u32>(CommandLatencyUs + tx + rx + compute + 0.5);
}

double V1BatchTransportUs() {
    constexpr double bytes = static_cast<double>(BlockBytes + 4 + BlockBytes);
    return 3.0 * CommandLatencyUs + (bytes / BytesPerSecond) * 1'000'000.0;
}

void QueueDescriptorBatch(const std::vector<JobDescriptorV1>& descriptors,
                          const std::vector<const u8*>& inline_inputs) {
    auto& s = GetState();
    const u32 batch_size = static_cast<u32>(descriptors.size());
    const double transport_share_us = V1BatchTransportUs() / static_cast<double>(batch_size);
    const u32 transport_us_x1000 =
        static_cast<u32>(std::llround(transport_share_us * 1000.0));

    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        const auto& desc = descriptors[i];
        CompletionRecordV1 completion{};
        completion.sequence = desc.sequence;
        completion.software_us = desc.arg0;
        completion.output_length = desc.output_length;
        completion.transport_us_x1000 = transport_us_x1000;
        completion.batch_size = batch_size;

        auto input_it = s.allocations.find(desc.input_handle);
        if (input_it == s.allocations.end() || desc.input_offset > input_it->second.data.size() ||
            desc.input_length > input_it->second.data.size() - desc.input_offset) {
            completion.status = static_cast<u16>(BadHandle);
            s.completions.push_back(completion);
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
            completion.status = static_cast<u16>(BadDescriptor);
            s.completions.push_back(completion);
            continue;
        }

        if (desc.output_handle != 0) {
            auto output_it = s.allocations.find(desc.output_handle);
            if (output_it == s.allocations.end() ||
                desc.output_offset > output_it->second.data.size() ||
                desc.output_length > output_it->second.data.size() - desc.output_offset) {
                completion.status = static_cast<u16>(BadHandle);
                s.completions.push_back(completion);
                continue;
            }
        }

        const u32 handle = s.next_job++;
        Job job;
        job.allocation = desc.input_handle;
        job.kernel = desc.kernel_id;
        job.operations = desc.work_units;
        job.software_us = desc.arg0;
        job.checksum = Fnv1a(allocation.data.data() + desc.input_offset, desc.input_length);
        const double compute_us =
            (static_cast<double>(job.operations) / KernelOpsPerSecond) * 1'000'000.0;
        job.modeled_us = static_cast<u32>(std::llround(compute_us + transport_share_us));
        job.speedup_x1000 = job.modeled_us
                                ? static_cast<u32>((static_cast<u64>(job.software_us) * 1000u) /
                                                   job.modeled_us)
                                : 0;
        job.ready = true;
        job.polls_remaining = 0;
        s.jobs.emplace(handle, job);

        completion.job_handle = handle;
        completion.modeled_us = job.modeled_us;
        completion.speedup_x1000 = job.speedup_x1000;
        completion.checksum = job.checksum;
        s.completions.push_back(completion);
    }
}

void ProcessWriteBlock(u8 selector, u32 word) {
    auto& s = GetState();
    const u32 len = word & 0xFFFFu;
    if (selector != 0 || len == 0 || len > BlockBytes) {
        s.stage[Result] = BadBlock;
        return;
    }

    auto& regs = s.regs->Vector();
    const u8* data = regs.data() + RegBlockTx;
    std::vector<JobDescriptorV1> descriptors;
    std::vector<const u8*> inline_inputs;
    std::size_t cursor = 0;

    while (cursor + sizeof(JobDescriptorV1) <= len &&
           descriptors.size() < MaxDescriptorsPerBlock) {
        JobDescriptorV1 desc{};
        std::memcpy(&desc, data + cursor, sizeof(desc));
        if (desc.magic == 0)
            break;
        if (desc.magic != DescriptorMagic || desc.version != BlockProtocolVersion ||
            desc.opcode != DescriptorOpcodeSubmit) {
            s.stage[Result] = BadDescriptor;
            return;
        }

        cursor += sizeof(desc);
        const bool inline_input = (desc.flags & DescriptorFlagInlineInput) != 0;
        const std::size_t inline_len = inline_input ? desc.input_length : 0;
        if (inline_len > len - cursor) {
            s.stage[Result] = BadBlock;
            return;
        }

        descriptors.push_back(desc);
        inline_inputs.push_back(inline_input ? data + cursor : nullptr);
        cursor += inline_len;
    }

    if (descriptors.empty()) {
        s.stage[Result] = BadDescriptor;
        return;
    }

    QueueDescriptorBatch(descriptors, inline_inputs);
    LOG_INFO(Core, "GekkoPAK NTR v1 F4 submit: batch={} completion_depth={}",
             descriptors.size(), s.completions.size());
}

void ProcessReadBlock(u8 selector, u32 word) {
    auto& s = GetState();
    const u32 offset = word & 0xFFFFu;
    const u32 len = word >> 16;
    if (selector != 1 || offset != 0 || len == 0 || len > BlockBytes) {
        s.stage[Result] = BadBlock;
        return;
    }

    auto& regs = s.regs->Vector();
    std::fill_n(regs.begin() + RegBlockRx, BlockBytes, 0);
    std::size_t written = 0;
    while (!s.completions.empty() && written + sizeof(CompletionRecordV1) <= len) {
        std::memcpy(regs.data() + RegBlockRx + written, &s.completions.front(),
                    sizeof(CompletionRecordV1));
        s.completions.pop_front();
        written += sizeof(CompletionRecordV1);
    }
    LOG_INFO(Core, "GekkoPAK NTR v1 F5 read: bytes={} remaining={}", written,
             s.completions.size());
}

void ExecuteHighCommand(u8 command, u32 seq) {
    auto& s = GetState();
    s.stage[Result] = Ok;
    s.stage[Out0] = s.stage[Out1] = s.stage[Out2] = s.stage[Out3] = 0;

    switch (command) {
    case Hello:
        s.stage[Out0] = ProtocolVersion;
        s.stage[Out1] = Capabilities;
        s.stage[Out2] = LocalMemoryBytes;
        s.stage[Out3] = 2;
        break;
    case GetCaps:
        s.stage[Out0] = Capabilities;
        s.stage[Out1] = LocalMemoryBytes;
        s.stage[Out2] = 6u * 1024u * 1024u;
        s.stage[Out3] = 25;
        break;
    case Alloc: {
        const u32 bytes = s.stage[Arg0];
        if (bytes == 0 || bytes > LocalMemoryBytes || s.allocated_bytes + bytes > LocalMemoryBytes) {
            s.stage[Result] = NoMemory;
            break;
        }
        const u32 handle = s.next_alloc++;
        s.allocations.emplace(handle, Allocation{std::vector<u8>(bytes), 0});
        s.allocated_bytes += bytes;
        s.stage[Out0] = handle;
        s.stage[Out1] = bytes;
        break;
    }
    case Upload: {
        auto it = s.allocations.find(s.stage[Arg0]);
        const u32 offset = s.stage[Arg1];
        const u32 len = s.stage[PayloadLen];
        if (it == s.allocations.end() || len > s.payload.size() || offset > it->second.data.size() ||
            len > it->second.data.size() - offset) {
            s.stage[Result] = BadHandle;
            break;
        }
        std::copy_n(s.payload.begin(), len, it->second.data.begin() + offset);
        it->second.uploaded =
            std::max(it->second.uploaded, static_cast<std::size_t>(offset + len));
        s.stage[Out0] = len;
        s.stage[Out1] = Fnv1a(it->second.data.data(), it->second.uploaded);
        break;
    }
    case Submit: {
        auto it = s.allocations.find(s.stage[Arg0]);
        if (it == s.allocations.end()) {
            s.stage[Result] = BadHandle;
            break;
        }
        const u32 handle = s.next_job++;
        Job job;
        job.allocation = s.stage[Arg0];
        job.kernel = s.stage[Arg1];
        job.operations = s.stage[Arg2];
        job.software_us = s.stage[Arg3];
        job.checksum = Fnv1a(it->second.data.data(), it->second.uploaded);
        job.modeled_us = ModelJobUs(static_cast<u32>(it->second.uploaded), 16, job.operations);
        job.speedup_x1000 = job.modeled_us
                                ? static_cast<u32>((static_cast<u64>(job.software_us) * 1000u) /
                                                   job.modeled_us)
                                : 0;
        s.jobs.emplace(handle, job);
        s.stage[Out0] = handle;
        s.stage[Out1] = job.modeled_us;
        break;
    }
    case Poll: {
        auto it = s.jobs.find(s.stage[Arg0]);
        if (it == s.jobs.end()) {
            s.stage[Result] = BadHandle;
            break;
        }
        if (!it->second.ready) {
            if (it->second.polls_remaining > 0)
                --it->second.polls_remaining;
            if (it->second.polls_remaining == 0)
                it->second.ready = true;
        }
        s.stage[Out0] = it->second.ready ? 1u : 0u;
        s.stage[Out1] = static_cast<u32>(it->second.polls_remaining);
        break;
    }
    case Collect: {
        auto it = s.jobs.find(s.stage[Arg0]);
        if (it == s.jobs.end()) {
            s.stage[Result] = BadHandle;
            break;
        }
        if (!it->second.ready) {
            s.stage[Result] = NotReady;
            break;
        }
        s.stage[Out0] = it->second.modeled_us;
        s.stage[Out1] = it->second.speedup_x1000;
        s.stage[Out2] = it->second.checksum;
        s.stage[Out3] = it->second.software_us;
        break;
    }
    case Free: {
        auto it = s.allocations.find(s.stage[Arg0]);
        if (it == s.allocations.end()) {
            s.stage[Result] = BadHandle;
            break;
        }
        s.allocated_bytes -= it->second.data.size();
        s.allocations.erase(it);
        break;
    }
    case Complete:
        s.completed = true;
        s.stage[Out0] = 0x53534150u;
        LOG_INFO(Core, "GekkoPAK NTR guest completed: seq={} transfers={}", seq, s.transfers);
        break;
    default:
        s.stage[Result] = BadCommand;
        break;
    }
}

void ProcessWireCommand(const u8 cmd[8]) {
    auto& s = GetState();
    if (cmd[1] != Magic0 || cmd[2] != Magic1)
        return;

    u32 word{};
    std::memcpy(&word, cmd + 4, sizeof(word));

    switch (cmd[0]) {
    case WireWriteReg:
        if (cmd[3] < s.stage.size())
            s.stage[cmd[3]] = word;
        break;
    case WireExec:
        ExecuteHighCommand(cmd[3], word);
        break;
    case WireReadReg:
        if (cmd[3] == EventCompletionDepth) {
            Store32(RegFifo, static_cast<u32>(std::min<std::size_t>(s.completions.size(), 0xFFFFu)));
            Store32(RegRomCnt, Load32(RegRomCnt) | CardDataReady);
        } else if (cmd[3] < s.stage.size()) {
            Store32(RegFifo, s.stage[cmd[3]]);
            Store32(RegRomCnt, Load32(RegRomCnt) | CardDataReady);
        }
        break;
    case WireWritePayloadWord: {
        const std::size_t offset = static_cast<std::size_t>(cmd[3]) * 4;
        if (offset + 4 <= s.payload.size())
            std::memcpy(s.payload.data() + offset, &word, sizeof(word));
        break;
    }
    case WireWriteBlock:
        ProcessWriteBlock(cmd[3], word);
        break;
    case WireReadBlock:
        ProcessReadBlock(cmd[3], word);
        break;
    default:
        break;
    }
}

} // namespace

std::shared_ptr<BackingMem> GetRegisterMemory() {
    return GetState().regs;
}

void Reset() {
    auto& s = GetState();
    std::fill(s.regs->Vector().begin(), s.regs->Vector().end(), 0);
    s.stage.fill(0);
    s.payload.fill(0);
    s.allocations.clear();
    s.jobs.clear();
    s.completions.clear();
    s.next_alloc = 1;
    s.next_job = 1;
    s.allocated_bytes = 0;
    s.transfers = 0;
    s.completed = false;
    LOG_INFO(Core, "GekkoPAK NTR virtual cartridge reset");
}

void Tick() {
    auto& s = GetState();
    const u32 romcnt = Load32(RegRomCnt);
    if ((romcnt & CardActivate) == 0)
        return;

    u8 cmd[8]{};
    std::memcpy(cmd, s.regs->Vector().data() + RegCommand, sizeof(cmd));

    Store32(RegRomCnt, romcnt & ~(CardActivate | CardDataReady));
    ProcessWireCommand(cmd);
    ++s.transfers;
}

} // namespace GekkoPakNtr
