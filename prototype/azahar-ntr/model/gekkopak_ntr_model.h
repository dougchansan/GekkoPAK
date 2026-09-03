#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gekkopak::ntr {

constexpr std::uint32_t kPhysicalBase = 0x10164000u;
constexpr std::uint32_t kVirtualBase = 0x1EC64000u;
constexpr std::size_t kRegisterPageSize = 0x1000;

constexpr std::size_t kRegRomCnt = 0x04;
constexpr std::size_t kRegCommand = 0x08;
constexpr std::size_t kRegFifo = 0x1C;

constexpr std::uint32_t kCardActivate = 1u << 31;
constexpr std::uint32_t kCardResetHigh = 1u << 29;
constexpr std::uint32_t kCardDataReady = 1u << 23;
constexpr std::uint32_t kCardBlock4 = 7u << 24;

constexpr std::uint8_t kWireWriteReg = 0xF0;
constexpr std::uint8_t kWireExec = 0xF1;
constexpr std::uint8_t kWireReadReg = 0xF2;
constexpr std::uint8_t kWireWritePayloadWord = 0xF3;
constexpr std::uint8_t kMagic0 = 0x47;
constexpr std::uint8_t kMagic1 = 0x4B;

constexpr std::uint32_t kProtocolVersion = 0x00010000u;
constexpr std::uint32_t kCaps = 0x0000000Fu;
constexpr std::uint32_t kLocalMemoryBytes = 32u * 1024u * 1024u;

enum Register : std::uint8_t {
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

enum HighCommand : std::uint8_t {
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

enum Error : std::uint32_t {
    Ok = 0,
    BadCommand = 1,
    BadHandle = 2,
    NoMemory = 3,
    NotReady = 4,
};

class Device {
public:
    Device();

    std::uint8_t* registers() { return regs_.data(); }
    const std::uint8_t* registers() const { return regs_.data(); }
    std::size_t register_size() const { return regs_.size(); }

    void Tick();

    bool completed() const { return completed_; }
    std::uint64_t transfers() const { return transfers_; }

private:
    struct Allocation {
        std::vector<std::uint8_t> data;
        std::size_t uploaded = 0;
    };
    struct Job {
        std::uint32_t allocation = 0;
        std::uint32_t kernel = 0;
        std::uint32_t operations = 0;
        std::uint32_t software_us = 0;
        std::uint32_t modeled_us = 0;
        std::uint32_t speedup_x1000 = 0;
        std::uint32_t checksum = 0;
        int polls_remaining = 2;
        bool ready = false;
    };

    std::array<std::uint8_t, kRegisterPageSize> regs_{};
    std::array<std::uint32_t, 16> stage_{};
    std::array<std::uint8_t, 1024> payload_{};
    std::unordered_map<std::uint32_t, Allocation> allocations_;
    std::unordered_map<std::uint32_t, Job> jobs_;
    std::uint32_t next_alloc_ = 1;
    std::uint32_t next_job_ = 1;
    std::uint64_t allocated_bytes_ = 0;
    std::uint64_t transfers_ = 0;
    bool completed_ = false;

    void ProcessWireCommand(const std::uint8_t cmd[8]);
    void ExecuteHighCommand(std::uint8_t command, std::uint32_t seq);
    std::uint32_t ModelJobUs(std::uint32_t tx_bytes, std::uint32_t rx_bytes,
                             std::uint32_t operations) const;
    static std::uint32_t Fnv1a(const std::uint8_t* data, std::size_t size);

    std::uint32_t Load32(std::size_t off) const;
    void Store32(std::size_t off, std::uint32_t value);
};

} // namespace gekkopak::ntr
