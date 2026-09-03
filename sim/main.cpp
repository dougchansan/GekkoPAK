#include "gekkopak/gekkopak.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using gekkopak::DeviceConfig;
using gekkopak::JobDescriptor;
using gekkopak::JobResult;
using gekkopak::JobType;
using gekkopak::Simulator;

void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n\n"
        << "Options:\n"
        << "  --tx <MiB/s>          Simulated host-to-cart bandwidth\n"
        << "  --rx <MiB/s>          Simulated cart-to-host bandwidth\n"
        << "  --latency-us <us>     Command/setup latency\n"
        << "  --ops-mops <Mops/s>   Accelerator throughput\n"
        << "  --memory-mib <MiB>    Accelerator-local memory\n"
        << "  --sweep               Run payload-size crossover sweep\n"
        << "  --help                Show this message\n";
}

double parse_number(const char* text, const char* option) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || value <= 0.0) {
        throw std::invalid_argument(std::string("invalid value for ") + option);
    }
    return value;
}

void print_job(const JobDescriptor& job, const JobResult& result) {
    std::cout << std::left << std::setw(24)
              << (job.label ? job.label : gekkopak::to_string(job.type))
              << std::right << std::fixed << std::setprecision(1)
              << std::setw(10) << result.tx_us
              << std::setw(11) << result.compute_us
              << std::setw(10) << result.rx_us
              << std::setw(11) << result.total_us
              << std::setw(11) << result.software_time_us
              << std::setw(9) << (result.offload_wins() ? "YES" : "NO");

    if (result.software_time_us > 0.0) {
        std::cout << std::setw(9) << std::setprecision(2) << result.speedup() << "x";
    }
    std::cout << '\n';
}

void run_examples(const Simulator& sim) {
    const std::vector<JobDescriptor> jobs = {
        {JobType::DspFrame, 256, 128, 250'000, 3500.0, "DSP frame mailbox"},
        {JobType::PairedSingleBatch, 1024, 256, 500'000, 5000.0, "paired-single batch"},
        {JobType::TextureConvert, 64 * 1024, 64 * 1024, 300'000, 3000.0,
         "64 KiB texture roundtrip"},
        {JobType::Custom, 128, 128, 750'000, 6000.0, "resident-data hot kernel"},
    };

    std::cout << "\nRepresentative jobs\n";
    std::cout << std::left << std::setw(24) << "job"
              << std::right << std::setw(10) << "TX us"
              << std::setw(11) << "compute"
              << std::setw(10) << "RX us"
              << std::setw(11) << "total us"
              << std::setw(11) << "ARM11 us"
              << std::setw(9) << "wins?"
              << std::setw(10) << "speedup" << '\n';

    for (const auto& job : jobs) {
        print_job(job, sim.estimate(job));
    }
}

void run_sweep(const Simulator& sim) {
    std::cout << "\nCrossover sweep: 500k operations, 5000 us software baseline\n";
    std::cout << std::left << std::setw(14) << "TX=RX bytes"
              << std::right << std::setw(14) << "offload us"
              << std::setw(12) << "speedup"
              << std::setw(10) << "wins?" << '\n';

    for (std::uint64_t bytes = 64; bytes <= 256 * 1024; bytes *= 2) {
        const JobDescriptor job{
            JobType::Custom,
            bytes,
            bytes,
            500'000,
            5000.0,
            "sweep",
        };
        const auto result = sim.estimate(job);
        std::cout << std::left << std::setw(14) << bytes
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(14) << result.total_us
                  << std::setw(11) << std::setprecision(2) << result.speedup() << "x"
                  << std::setw(10) << (result.offload_wins() ? "YES" : "NO") << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        DeviceConfig config;
        bool sweep = false;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                print_usage(argv[0]);
                return 0;
            }
            if (arg == "--sweep") {
                sweep = true;
                continue;
            }
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value after " + arg);
            }

            if (arg == "--tx") {
                config.tx_mib_per_second = parse_number(argv[++i], "--tx");
            } else if (arg == "--rx") {
                config.rx_mib_per_second = parse_number(argv[++i], "--rx");
            } else if (arg == "--latency-us") {
                config.command_latency_us = parse_number(argv[++i], "--latency-us");
            } else if (arg == "--ops-mops") {
                config.accelerator_ops_per_second =
                    parse_number(argv[++i], "--ops-mops") * 1'000'000.0;
            } else if (arg == "--memory-mib") {
                config.local_memory_bytes = static_cast<std::uint64_t>(
                    parse_number(argv[++i], "--memory-mib") * 1024.0 * 1024.0);
            } else {
                throw std::invalid_argument("unknown option: " + arg);
            }
        }

        Simulator sim(config);
        std::cout << sim.describe() << '\n';

        const auto resident = sim.allocate(8ull * 1024ull * 1024ull);
        std::cout << "Allocated persistent test buffer handle " << resident
                  << " (8 MiB); free local RAM: "
                  << (sim.free_bytes() / (1024ull * 1024ull)) << " MiB\n";

        run_examples(sim);
        if (sweep) {
            run_sweep(sim);
        }

        sim.release(resident);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "gekkopak_sim: " << e.what() << '\n';
        return 1;
    }
}
