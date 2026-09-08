#include "gekkopak_ntr_model.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace gekkopak::ntr;

namespace {
std::uint32_t load32(const std::uint8_t* p, std::size_t off) {
    std::uint32_t v{};
    std::memcpy(&v, p + off, sizeof(v));
    return v;
}

std::uint32_t wire(Device& d, std::uint8_t op, std::uint8_t index, std::uint32_t value,
                   bool read = false) {
    // Canonical big-endian wire form, the order DSpico's PIO delivers, staged
    // into CMD0/CMD1 as the little-endian words a guest would store.
    std::uint8_t cmd[protocol::kCommandBytes]{};
    protocol::EncodeCommand(op, index, value, cmd);
    d.Write32(kRegCommand, load32(cmd, 0));
    d.Write32(kRegCommand + 4, load32(cmd, 4));
    // A four-byte read declares block size 7; everything else declares none.
    d.Write32(kRegRomCnt, kCardResetHigh | kCardActivate | (read ? kCardBlock4 : kCardBlockNone));

    // The command runs on the ROMCNT write. A four-byte read leaves one word in
    // the FIFO, and taking it is what ends the transfer.
    std::uint32_t response = 0;
    if (read) {
        assert((d.Read32(kRegRomCnt) & kCardDataReady) != 0);
        response = d.Read32(kRegFifo);
    }
    assert((d.Read32(kRegRomCnt) & kCardActivate) == 0);
    return read ? response : 0;
}

void wr(Device& d, Register reg, std::uint32_t value) {
    (void)wire(d, kWireWriteReg, static_cast<std::uint8_t>(reg), value);
}

std::uint32_t rd(Device& d, Register reg) {
    return wire(d, kWireReadReg, static_cast<std::uint8_t>(reg), 0, true);
}

void exec(Device& d, HighCommand cmd, std::uint32_t seq) {
    (void)wire(d, kWireExec, static_cast<std::uint8_t>(cmd), seq);
    assert(rd(d, Result) == Ok);
}

std::uint32_t alloc_persistent(Device& d) {
    std::uint32_t seq = 0;
    exec(d, Hello, ++seq);
    assert((rd(d, Out1) & kCapBlockTransport) != 0);
    wr(d, Arg0, 4096);
    exec(d, Alloc, ++seq);
    const auto handle = rd(d, Out0);
    assert(handle != 0);
    return handle;
}

JobDescriptorV1 descriptor(std::uint32_t sequence, std::uint32_t handle, bool inline_input) {
    JobDescriptorV1 d{};
    d.sequence = sequence;
    d.flags = inline_input ? kDescriptorFlagInlineInput : 0;
    d.input_handle = handle;
    d.input_length = 16;
    d.output_length = 16;
    d.kernel_id = 1;
    d.work_units = 250000;
    d.arg0 = 3500;
    return d;
}

CompletionRecordV1 completion_at(const std::array<std::uint8_t, kBlockBytes>& block,
                                 std::size_t index) {
    CompletionRecordV1 c{};
    std::memcpy(&c, block.data() + index * sizeof(c), sizeof(c));
    return c;
}
} // namespace

int main() {
    Device device;
    const auto allocation = alloc_persistent(device);

    const std::array<std::uint32_t, 4> payload_words = {
        0x11223344u, 0x55667788u, 0xAABBCCDDu, 0x0BADF00Du};

    std::array<std::uint8_t, kBlockBytes> submit{};
    const auto single_desc = descriptor(1, allocation, true);
    std::memcpy(submit.data(), &single_desc, sizeof(single_desc));
    std::memcpy(submit.data() + sizeof(single_desc), payload_words.data(), sizeof(payload_words));

    const auto single_start = device.transfers();
    assert(device.WriteBlock(BlockSelector::Submission, submit.data(), submit.size()));
    assert(device.ReadEvent() == 1);
    std::array<std::uint8_t, kBlockBytes> result{};
    assert(device.ReadBlock(BlockSelector::Completion, 0, result.data(), result.size()) ==
           sizeof(CompletionRecordV1));
    const auto single = completion_at(result, 0);
    assert(device.transfers() - single_start == 3);
    assert(single.magic == kCompletionMagic && single.status == Ok);
    assert(single.sequence == 1 && single.batch_size == 1);
    assert(single.checksum == 0xF269B734u);
    assert(single.modeled_us == 2738u);
    assert(single.speedup_x1000 == 1278u);

    std::printf("V1 SINGLE PASS transfers=3 modeled=%u us speedup=%u.%03ux checksum=0x%08x "
                "transport=%.3f us\n",
                single.modeled_us, single.speedup_x1000 / 1000, single.speedup_x1000 % 1000,
                single.checksum, single.transport_us_x1000 / 1000.0);

    submit.fill(0);
    for (std::size_t i = 0; i < kMaxDescriptorsPerBlock; ++i) {
        const auto d = descriptor(static_cast<std::uint32_t>(100 + i), allocation, false);
        std::memcpy(submit.data() + i * sizeof(d), &d, sizeof(d));
    }

    const auto batch_start = device.transfers();
    assert(device.WriteBlock(BlockSelector::Submission, submit.data(), submit.size()));
    assert(device.ReadEvent() == kMaxDescriptorsPerBlock);
    result.fill(0);
    assert(device.ReadBlock(BlockSelector::Completion, 0, result.data(), result.size()) ==
           kBlockBytes);
    assert(device.transfers() - batch_start == 3);

    for (std::size_t i = 0; i < kMaxDescriptorsPerBlock; ++i) {
        const auto c = completion_at(result, i);
        assert(c.magic == kCompletionMagic && c.status == Ok);
        assert(c.sequence == 100 + i);
        assert(c.batch_size == kMaxDescriptorsPerBlock);
        assert(c.checksum == 0xF269B734u);
        assert(c.modeled_us == 2530u);
        assert(c.speedup_x1000 == 1383u);
    }

    const auto batch0 = completion_at(result, 0);
    std::printf("V1 BATCH8 PASS transfers=3 transactions_per_job=0.375 modeled/job=%u us "
                "speedup=%u.%03ux transport/job=%.3f us\n",
                batch0.modeled_us, batch0.speedup_x1000 / 1000, batch0.speedup_x1000 % 1000,
                batch0.transport_us_x1000 / 1000.0);
    return 0;
}
