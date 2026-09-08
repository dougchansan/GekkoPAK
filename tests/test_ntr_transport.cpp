// Register-page NTR transport tests.
//
// These cover the bus itself rather than the protocol: the ROMCNT handshake,
// the block-size field, and the word-at-a-time data phase. The golden vectors
// prove the device answers correctly; this proves the transfer underneath them
// behaves like the bus it is modelling.

#include "gekkopak/device.h"
#include "gekkopak/ntr_register_transport.h"
#include "gekkopak/protocol.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace gekkopak;
using namespace gekkopak::protocol;
using namespace gekkopak::ntr_transport;

namespace {

constexpr std::uint32_t kPoolBytes = 64u * 1024u;

struct Bus {
    std::vector<std::uint8_t> pool = std::vector<std::uint8_t>(kPoolBytes, 0);
    std::uint8_t page[kRegisterPageSize]{};
    Device device;
    RegisterTransport transport;

    Bus() {
        Device::Config config;
        config.pool = pool.data();
        config.pool_bytes = kPoolBytes;
        config.reported_local_bytes = kPoolBytes;
        device.Reset(config);
        transport.Reset(page);
    }

    std::uint32_t Read(std::uint32_t offset) { return transport.Read32(device, page, offset); }
    void Write(std::uint32_t offset, std::uint32_t value) {
        transport.Write32(device, page, offset, value);
    }

    void Stage(std::uint8_t opcode, std::uint8_t index, std::uint32_t word,
               std::uint32_t block_field) {
        std::uint8_t cmd[kCommandBytes];
        EncodeCommand(opcode, index, word, cmd);
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;
        std::memcpy(&lo, cmd, sizeof(lo));
        std::memcpy(&hi, cmd + 4, sizeof(hi));
        Write(kRegCommand, lo);
        Write(kRegCommand + 4, hi);
        Write(kRegRomCnt, kCardResetHigh | kCardActivate | block_field);
    }

    std::uint32_t romcnt() { return Read(kRegRomCnt); }
    bool active() { return (romcnt() & kCardActivate) != 0; }
    bool ready() { return (romcnt() & kCardDataReady) != 0; }

    // Runs a whole transaction, returning how many data words crossed.
    std::size_t Run(std::uint8_t opcode, std::uint8_t index, std::uint32_t word,
                    std::uint32_t block_field, const std::uint8_t* in, std::uint8_t* out) {
        Stage(opcode, index, word, block_field);
        std::size_t words = 0;
        while (active()) {
            assert(ready());
            if (out != nullptr) {
                const std::uint32_t value = Read(kRegFifo);
                std::memcpy(out + words * 4u, &value, sizeof(value));
            } else if (in != nullptr) {
                std::uint32_t value = 0;
                std::memcpy(&value, in + words * 4u, sizeof(value));
                Write(kRegFifo, value);
            } else {
                (void)Read(kRegFifo);
            }
            ++words;
            assert(words <= kBlockBytes / 4);
        }
        return words;
    }
};

std::uint32_t ReadReg(Bus& bus, std::uint8_t index) {
    std::uint8_t out[4]{};
    const std::size_t words = bus.Run(kWireReadReg, index, 0, kCardBlock4, nullptr, out);
    assert(words == 1);
    std::uint32_t value = 0;
    std::memcpy(&value, out, sizeof(value));
    return value;
}

void Exec(Bus& bus, std::uint8_t command, std::uint32_t sequence) {
    bus.Run(kWireExec, command, sequence, kCardBlockNone, nullptr, nullptr);
}

void WriteReg(Bus& bus, std::uint8_t index, std::uint32_t value) {
    bus.Run(kWireWriteReg, index, value, kCardBlockNone, nullptr, nullptr);
}

// -- tests ------------------------------------------------------------------

void TestCommandPhaseHasNoData() {
    Bus bus;
    const std::size_t words = bus.Run(kWireExec, kHello, 1, kCardBlockNone, nullptr, nullptr);
    assert(words == 0);
    assert(ReadReg(bus, kOut0) == kProtocolVersion);
    std::printf("command phase: F1 carries no data phase\n");
}

void TestFourByteReadIsOneWord() {
    Bus bus;
    Exec(bus, kHello, 1);

    bus.Stage(kWireReadReg, kOut0, 0, kCardBlock4);
    // Writing ROMCNT ran the command. The transfer holds CARD_START until the
    // word is taken, so both bits are set before the FIFO is touched.
    assert(bus.active());
    assert(bus.ready());

    const std::uint32_t value = bus.Read(kRegFifo);
    // Reading the FIFO is what ends the transfer. Nothing acknowledged it.
    assert(!bus.active());
    assert(!bus.ready());
    assert(value == kProtocolVersion);
    std::printf("four-byte read: reading the FIFO ends the transfer by itself\n");
}

void TestBlockSizeMustMatchOpcode() {
    Bus bus;

    // A four-byte read that forgets to declare block size 7.
    bus.Stage(kWireReadReg, kOut0, 0, kCardBlockNone);
    assert(!bus.active());
    assert(bus.transport.fault_count() == 1);
    assert(bus.transport.last_fault() == RegisterTransport::Fault::BlockSizeMismatch);

    // A command with no data phase that wrongly declares one.
    bus.Stage(kWireExec, kHello, 1, kCardBlock512);
    assert(!bus.active());
    assert(bus.transport.fault_count() == 2);
    // The refused command did not run: OUT0 is still zero.
    assert(ReadReg(bus, kOut0) == 0);

    // F4 without the 512-byte declaration.
    bus.Stage(kWireWriteBlock, kSelectorSubmission, EncodeWriteBlockWord(64), kCardBlockNone);
    assert(!bus.active());
    assert(bus.transport.fault_count() == 3);
    std::printf("block size: a mismatched ROMCNT field is a fault, not a silent pass\n");
}

void TestBlockWriteStreams128Words() {
    Bus bus;

    WriteReg(bus, kArg0, 4096);
    Exec(bus, kAlloc, 1);
    const std::uint32_t handle = ReadReg(bus, kOut0);
    assert(handle == 1);

    std::uint8_t block[kBlockBytes];
    std::memset(block, 0, sizeof(block));
    JobDescriptorV1 desc{};
    desc.sequence = 1;
    desc.flags = kDescriptorFlagInlineInput;
    desc.input_handle = handle;
    desc.input_length = 16;
    desc.output_length = 16;
    desc.kernel_id = 1;
    desc.work_units = 250000;
    desc.arg0 = 3500;
    std::memcpy(block, &desc, sizeof(desc));
    const std::uint32_t reference[4] = {0x11223344u, 0x55667788u, 0xAABBCCDDu, 0x0BADF00Du};
    std::memcpy(block + sizeof(desc), reference, sizeof(reference));

    const std::uint64_t before = bus.device.transfers();
    const std::size_t words = bus.Run(kWireWriteBlock, kSelectorSubmission,
                                      EncodeWriteBlockWord(80), kCardBlock512, block, nullptr);
    assert(words == kBlockBytes / 4);
    // One NTR transaction, 128 words. The transfer counter must not count words.
    assert(bus.device.transfers() - before == 1);
    assert(bus.device.completion_depth() == 1);
    std::printf("F4: %zu words in one transaction, completion queued\n", words);
}

void TestBlockReadStreams128Words() {
    Bus bus;

    WriteReg(bus, kArg0, 4096);
    Exec(bus, kAlloc, 1);

    std::uint8_t block[kBlockBytes];
    std::memset(block, 0, sizeof(block));
    JobDescriptorV1 desc{};
    desc.sequence = 7;
    desc.flags = kDescriptorFlagInlineInput;
    desc.input_handle = 1;
    desc.input_length = 16;
    desc.kernel_id = 1;
    desc.work_units = 250000;
    desc.arg0 = 3500;
    std::memcpy(block, &desc, sizeof(desc));
    const std::uint32_t reference[4] = {0x11223344u, 0x55667788u, 0xAABBCCDDu, 0x0BADF00Du};
    std::memcpy(block + sizeof(desc), reference, sizeof(reference));
    bus.Run(kWireWriteBlock, kSelectorSubmission, EncodeWriteBlockWord(80), kCardBlock512, block,
            nullptr);

    assert(ReadReg(bus, kEventCompletionDepth) == 1);

    std::uint8_t out[kBlockBytes];
    std::memset(out, 0, sizeof(out));
    const std::uint64_t before = bus.device.transfers();
    const std::size_t words = bus.Run(kWireReadBlock, kSelectorCompletion,
                                      EncodeReadBlockWord(0, kBlockBytes), kCardBlock512, nullptr,
                                      out);
    assert(words == kBlockBytes / 4);
    assert(bus.device.transfers() - before == 1);

    CompletionRecordV1 record{};
    std::memcpy(&record, out, sizeof(record));
    assert(record.magic == kCompletionMagic);
    assert(record.status == kOk);
    assert(record.sequence == 7);
    assert(record.checksum == kReferenceChecksum);
    // The record survived 128 separate FIFO words in the right order.
    assert(out[0] == 0x47 && out[1] == 0x4B && out[2] == 0x43 && out[3] == 0x31);
    std::printf("F5: %zu words out, GKC1 record intact and correctly aligned\n", words);
}

void TestBadDiscriminatorStillEndsTheTransfer() {
    Bus bus;
    std::uint8_t cmd[kCommandBytes];
    EncodeCommand(kWireExec, kHello, 1, cmd);
    cmd[1] = 0x00; // break "GK"
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    std::memcpy(&lo, cmd, sizeof(lo));
    std::memcpy(&hi, cmd + 4, sizeof(hi));
    bus.Write(kRegCommand, lo);
    bus.Write(kRegCommand + 4, hi);
    bus.Write(kRegRomCnt, kCardResetHigh | kCardActivate | kCardBlockNone);
    assert(!bus.active());
    assert(bus.transport.fault_count() == 0); // not our command, not our fault
    assert(ReadReg(bus, kOut0) == 0);
    std::printf("discriminator: foreign traffic is dropped and the bus released\n");
}

} // namespace

int main() {
    TestCommandPhaseHasNoData();
    TestFourByteReadIsOneWord();
    TestBlockSizeMustMatchOpcode();
    TestBlockWriteStreams128Words();
    TestBlockReadStreams128Words();
    TestBadDiscriminatorStillEndsTheTransfer();
    std::printf("ntr transport: all checks passed\n");
    return 0;
}
