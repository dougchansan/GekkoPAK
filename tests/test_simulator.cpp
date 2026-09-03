#include "gekkopak/gekkopak.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

bool nearly_equal(double a, double b, double epsilon = 0.01) {
    return std::fabs(a - b) <= epsilon;
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    try {
        gekkopak::DeviceConfig config;
        config.tx_mib_per_second = 1.0;
        config.rx_mib_per_second = 1.0;
        config.command_latency_us = 10.0;
        config.accelerator_ops_per_second = 1'000'000.0;
        config.local_memory_bytes = 1024;

        gekkopak::Simulator sim(config);

        const gekkopak::JobDescriptor job{
            gekkopak::JobType::Custom,
            1024,
            1024,
            1000,
            5000.0,
            "unit-test",
        };

        const auto result = sim.estimate(job);
        require(nearly_equal(result.command_us, 10.0), "command latency mismatch");
        require(nearly_equal(result.tx_us, 976.5625), "TX timing mismatch");
        require(nearly_equal(result.rx_us, 976.5625), "RX timing mismatch");
        require(nearly_equal(result.compute_us, 1000.0), "compute timing mismatch");
        require(nearly_equal(result.total_us, 2963.125), "total timing mismatch");
        require(result.offload_wins(), "expected offload to win");

        const auto a = sim.allocate(256);
        const auto b = sim.allocate(512);
        require(a != b, "buffer handles must be unique");
        require(sim.allocated_bytes() == 768, "allocation accounting mismatch");
        require(sim.free_bytes() == 256, "free-memory accounting mismatch");

        sim.release(a);
        require(sim.allocated_bytes() == 512, "release accounting mismatch");

        bool threw = false;
        try {
            sim.allocate(513);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        require(threw, "oversized allocation should throw std::bad_alloc");

        sim.release(b);
        require(sim.free_bytes() == 1024, "all local memory should be free");

        std::cout << "GekkoPAK simulator tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "test failure: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
