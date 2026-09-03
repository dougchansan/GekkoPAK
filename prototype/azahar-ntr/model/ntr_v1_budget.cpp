#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

constexpr double kMiB = 1024.0 * 1024.0;

struct Transport {
    double bandwidth_mib_s = 6.0;
    double command_latency_us = 25.0;
};

struct Budget {
    std::uint32_t transfers = 0;
    std::uint32_t tx_bytes = 0;
    std::uint32_t rx_bytes = 0;
    double bus_us = 0.0;
};

double bytes_us(std::uint32_t bytes, const Transport& t) {
    return static_cast<double>(bytes) / (t.bandwidth_mib_s * kMiB) * 1'000'000.0;
}

Budget bringup_steady_state(const Transport& t) {
    // Current F0-F3 path for a resident DSP job after allocation/upload:
    //   SUBMIT: 4 WR_REG + EXEC/result + job handle read = 10 wire transfers
    //   POLL x2: WR_REG + EXEC/result + ready read = 8 transfers
    //   COLLECT: WR_REG + EXEC/result + 3 result reads = 7 transfers
    // The production guest currently overlaps/omits a few diagnostic reads compared with
    // ntr_bus_test. Use 21 as the measured/target steady control-path order of magnitude.
    constexpr std::uint32_t transfers = 21;
    constexpr std::uint32_t tx_bytes = 16; // dynamic input already resident in this example
    constexpr std::uint32_t rx_bytes = 16; // compact result
    return {transfers, tx_bytes, rx_bytes,
            transfers * t.command_latency_us + bytes_us(tx_bytes + rx_bytes, t)};
}

Budget v1_block_steady_state(const Transport& t) {
    // F4 WRITE_BLOCK: one 512-byte data phase holding descriptor + dynamic input.
    // F2 EVENT/STATUS: one 4-byte result.
    // F5 READ_BLOCK: one 512-byte result/completion block.
    constexpr std::uint32_t transfers = 3;
    constexpr std::uint32_t tx_bytes = 512;
    constexpr std::uint32_t rx_bytes = 4 + 512;
    return {transfers, tx_bytes, rx_bytes,
            transfers * t.command_latency_us + bytes_us(tx_bytes + rx_bytes, t)};
}

} // namespace

int main() {
    const Transport t{};
    const auto f0 = bringup_steady_state(t);
    const auto v1 = v1_block_steady_state(t);

    assert(v1.transfers <= 4);
    assert(v1.transfers < f0.transfers);
    assert(v1.bus_us < f0.bus_us);

    const double reduction =
        100.0 * (1.0 - static_cast<double>(v1.transfers) / f0.transfers);
    const double bus_saved = f0.bus_us - v1.bus_us;

    std::printf("F0-F3 steady: transfers=%u tx=%u rx=%u bus=%.1f us\n",
                f0.transfers, f0.tx_bytes, f0.rx_bytes, f0.bus_us);
    std::printf("v1 block steady: transfers=%u tx=%u rx=%u bus=%.1f us\n",
                v1.transfers, v1.tx_bytes, v1.rx_bytes, v1.bus_us);
    std::printf("PASS transfer_reduction=%.1f%% modeled_bus_saved=%.1f us\n",
                reduction, bus_saved);
    return 0;
}
