#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace gekkopak {

enum class Capability : std::uint32_t {
    None          = 0,
    LocalMemory   = 1u << 0,
    DspAudio      = 1u << 1,
    PairedSingle  = 1u << 2,
    Texture       = 1u << 3,
    Decompression = 1u << 4,
};

constexpr Capability operator|(Capability a, Capability b) {
    return static_cast<Capability>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

constexpr bool has_capability(Capability mask, Capability value) {
    return (static_cast<std::uint32_t>(mask) & static_cast<std::uint32_t>(value)) != 0;
}

enum class JobType : std::uint32_t {
    Ping = 0,
    DspFrame,
    PairedSingleBatch,
    TextureConvert,
    Decompress,
    Custom,
};

using BufferHandle = std::uint32_t;

struct DeviceConfig {
    double tx_mib_per_second = 6.0;
    double rx_mib_per_second = 6.0;
    double command_latency_us = 25.0;
    std::uint64_t local_memory_bytes = 32ull * 1024ull * 1024ull;
    double accelerator_ops_per_second = 100'000'000.0;
    Capability capabilities = Capability::LocalMemory |
                              Capability::DspAudio |
                              Capability::PairedSingle |
                              Capability::Texture;
};

struct JobDescriptor {
    JobType type = JobType::Custom;
    std::uint64_t tx_bytes = 0;
    std::uint64_t rx_bytes = 0;
    std::uint64_t estimated_operations = 0;
    double software_time_us = 0.0;
    const char* label = nullptr;
};

struct JobResult {
    double command_us = 0.0;
    double tx_us = 0.0;
    double compute_us = 0.0;
    double rx_us = 0.0;
    double total_us = 0.0;
    double software_time_us = 0.0;

    bool offload_wins() const { return software_time_us > 0.0 && total_us < software_time_us; }
    double speedup() const { return total_us > 0.0 ? software_time_us / total_us : 0.0; }
};

class Simulator {
public:
    explicit Simulator(DeviceConfig config = {});

    const DeviceConfig& config() const;
    void set_config(const DeviceConfig& config);

    JobResult estimate(const JobDescriptor& job) const;

    BufferHandle allocate(std::uint64_t bytes);
    void release(BufferHandle handle);
    std::uint64_t allocated_bytes() const;
    std::uint64_t free_bytes() const;

    std::string describe() const;

private:
    DeviceConfig config_;
    std::uint64_t allocated_bytes_ = 0;
    BufferHandle next_handle_ = 1;
    std::unordered_map<BufferHandle, std::uint64_t> allocations_;
};

const char* to_string(JobType type);

} // namespace gekkopak
