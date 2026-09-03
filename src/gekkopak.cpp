#include "gekkopak/gekkopak.h"

#include <iomanip>
#include <new>
#include <sstream>
#include <stdexcept>

namespace gekkopak {
namespace {

constexpr double kBytesPerMiB = 1024.0 * 1024.0;
constexpr double kUsPerSecond = 1'000'000.0;

double transfer_time_us(std::uint64_t bytes, double mib_per_second) {
    if (bytes == 0) {
        return 0.0;
    }
    if (mib_per_second <= 0.0) {
        throw std::invalid_argument("GekkoPAK bandwidth must be greater than zero");
    }
    return (static_cast<double>(bytes) / (mib_per_second * kBytesPerMiB)) * kUsPerSecond;
}

} // namespace

Simulator::Simulator(DeviceConfig config) : config_(config) {
    if (config_.tx_mib_per_second <= 0.0 || config_.rx_mib_per_second <= 0.0) {
        throw std::invalid_argument("GekkoPAK TX/RX bandwidth must be greater than zero");
    }
    if (config_.accelerator_ops_per_second <= 0.0) {
        throw std::invalid_argument("GekkoPAK accelerator throughput must be greater than zero");
    }
}

const DeviceConfig& Simulator::config() const {
    return config_;
}

void Simulator::set_config(const DeviceConfig& config) {
    if (config.tx_mib_per_second <= 0.0 || config.rx_mib_per_second <= 0.0) {
        throw std::invalid_argument("GekkoPAK TX/RX bandwidth must be greater than zero");
    }
    if (config.accelerator_ops_per_second <= 0.0) {
        throw std::invalid_argument("GekkoPAK accelerator throughput must be greater than zero");
    }
    if (config.local_memory_bytes < allocated_bytes_) {
        throw std::invalid_argument("new local-memory size is smaller than current allocations");
    }
    config_ = config;
}

JobResult Simulator::estimate(const JobDescriptor& job) const {
    JobResult result;
    result.command_us = config_.command_latency_us;
    result.tx_us = transfer_time_us(job.tx_bytes, config_.tx_mib_per_second);
    result.rx_us = transfer_time_us(job.rx_bytes, config_.rx_mib_per_second);
    result.compute_us =
        (static_cast<double>(job.estimated_operations) / config_.accelerator_ops_per_second) *
        kUsPerSecond;
    result.total_us = result.command_us + result.tx_us + result.compute_us + result.rx_us;
    result.software_time_us = job.software_time_us;
    return result;
}

BufferHandle Simulator::allocate(std::uint64_t bytes) {
    if (!has_capability(config_.capabilities, Capability::LocalMemory)) {
        throw std::runtime_error("device does not expose accelerator-local memory");
    }
    if (bytes == 0) {
        throw std::invalid_argument("cannot allocate a zero-byte GekkoPAK buffer");
    }
    if (bytes > free_bytes()) {
        throw std::bad_alloc();
    }

    const BufferHandle handle = next_handle_++;
    allocations_.emplace(handle, bytes);
    allocated_bytes_ += bytes;
    return handle;
}

void Simulator::release(BufferHandle handle) {
    const auto it = allocations_.find(handle);
    if (it == allocations_.end()) {
        throw std::invalid_argument("unknown GekkoPAK buffer handle");
    }
    allocated_bytes_ -= it->second;
    allocations_.erase(it);
}

std::uint64_t Simulator::allocated_bytes() const {
    return allocated_bytes_;
}

std::uint64_t Simulator::free_bytes() const {
    return config_.local_memory_bytes - allocated_bytes_;
}

std::string Simulator::describe() const {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2)
        << "Virtual GekkoPAK: TX " << config_.tx_mib_per_second << " MiB/s, RX "
        << config_.rx_mib_per_second << " MiB/s, command " << config_.command_latency_us
        << " us, local RAM " << (static_cast<double>(config_.local_memory_bytes) / kBytesPerMiB)
        << " MiB, accelerator " << (config_.accelerator_ops_per_second / 1'000'000.0)
        << " Mops/s";
    return out.str();
}

const char* to_string(JobType type) {
    switch (type) {
    case JobType::Ping:
        return "ping";
    case JobType::DspFrame:
        return "dsp-frame";
    case JobType::PairedSingleBatch:
        return "paired-single";
    case JobType::TextureConvert:
        return "texture-convert";
    case JobType::Decompress:
        return "decompress";
    case JobType::Custom:
        return "custom";
    }
    return "unknown";
}

} // namespace gekkopak
