// Shared device core unit tests.
//
// These pin the deterministic values the whole project is built on -- the
// reference checksum 0xf269b734, the modeled times 2530/2738 us and the
// speedups 1.278x/1.383x -- so that re-expressing the timing model in integers
// cannot silently move them. They were produced by the original double-based
// host model and confirmed on DSpico hardware for the checksum.

#include "gekkopak/device.h"
#include "gekkopak/protocol.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace gekkopak;
using namespace gekkopak::protocol;

namespace {

constexpr u32 kPoolBytes = 64u * 1024u;

struct Fixture {
    std::vector<u8> pool = std::vector<u8>(kPoolBytes, 0);
    Device device;

    Fixture() {
        Device::Config config;
        config.pool = pool.data();
        config.pool_bytes = kPoolBytes;
        device.Reset(config);
    }
};

// The 16-byte reference pattern, as four little-endian words.
const u32 kReferenceWords[4] = {0x11223344u, 0x55667788u, 0xAABBCCDDu, 0x0BADF00Du};

void UploadReference(Device& d, u32 handle) {
    for (u8 i = 0; i < 4; ++i) {
        d.WritePayloadWord(i, kReferenceWords[i]);
    }
    d.WriteReg(kArg0, handle);
    d.WriteReg(kArg1, 0);
    d.WriteReg(kPayloadLen, 16);
    d.Exec(kUpload, 0);
}

JobDescriptorV1 MakeDescriptor(u32 sequence, u32 handle, bool inline_input) {
    JobDescriptorV1 d{};
    d.magic = kDescriptorMagic;
    d.version = kBlockProtocolVersion;
    d.opcode = kDescriptorOpcodeSubmit;
    d.sequence = sequence;
    d.flags = inline_input ? kDescriptorFlagInlineInput : 0u;
    d.input_handle = handle;
    d.input_length = 16;
    d.output_length = 16;
    d.kernel_id = 1;
    d.work_units = 250000;
    d.arg0 = 3500;
    return d;
}

// -- wire encoding ----------------------------------------------------------

void TestWireCodec() {
    u8 bytes[kCommandBytes];
    EncodeCommand(kWireWriteReg, kArg0, 0xDEADBEEFu, bytes);
    // Canonical big-endian header, as confirmed on DSpico silicon.
    assert(bytes[0] == 0xF0 && bytes[1] == 0x47 && bytes[2] == 0x4B && bytes[3] == 0x00);
    assert(bytes[4] == 0xDE && bytes[5] == 0xAD && bytes[6] == 0xBE && bytes[7] == 0xEF);

    const WireCommand decoded = DecodeCommand(bytes);
    assert(decoded.valid);
    assert(decoded.opcode == kWireWriteReg && decoded.index == kArg0);
    assert(decoded.word == 0xDEADBEEFu);

    bytes[1] = 0x00; // break the discriminator
    assert(!DecodeCommand(bytes).valid);
    std::printf("wire codec: big-endian header, GK discriminator enforced\n");
}

// -- F0-F3 lifecycle --------------------------------------------------------

void TestLegacyLifecycle() {
    Fixture f;
    Device& d = f.device;

    d.Exec(kHello, 1);
    assert(d.ReadReg(kResult) == kOk);
    assert(d.ReadReg(kOut0) == kProtocolVersion);
    assert(d.ReadReg(kOut1) == kCaps);
    assert(d.ReadReg(kOut2) == kPoolBytes);
    assert(d.ReadReg(kOut3) == 2);

    d.Exec(kGetCaps, 2);
    assert(d.ReadReg(kOut0) == kCaps);
    assert((d.ReadReg(kOut1) & kCapBlockTransport) == 0); // Out1 is size, not caps
    assert(d.ReadReg(kOut3) == 25);                       // command latency in us

    d.WriteReg(kArg0, 4096);
    d.Exec(kAlloc, 3);
    assert(d.ReadReg(kResult) == kOk);
    const u32 handle = d.ReadReg(kOut0);
    assert(handle == 1);
    assert(d.ReadReg(kOut1) == 4096);

    UploadReference(d, handle);
    assert(d.ReadReg(kResult) == kOk);
    assert(d.ReadReg(kOut0) == 16);
    assert(d.ReadReg(kOut1) == kReferenceChecksum);

    d.WriteReg(kArg0, handle);
    d.WriteReg(kArg1, 1);      // kernel
    d.WriteReg(kArg2, 250000); // work units
    d.WriteReg(kArg3, 3500);   // software reference time
    d.Exec(kSubmit, 4);
    assert(d.ReadReg(kResult) == kOk);
    const u32 job = d.ReadReg(kOut0);
    assert(job == 1);
    assert(d.ReadReg(kOut1) == 2530u);

    // POLL is not ready on the first call and ready on the second.
    d.WriteReg(kArg0, job);
    d.Exec(kPoll, 5);
    assert(d.ReadReg(kOut0) == 0);
    assert(d.ReadReg(kOut1) == 1);
    d.WriteReg(kArg0, job);
    d.Exec(kPoll, 6);
    assert(d.ReadReg(kOut0) == 1);

    d.WriteReg(kArg0, job);
    d.Exec(kCollect, 7);
    assert(d.ReadReg(kResult) == kOk);
    assert(d.ReadReg(kOut0) == 2530u);
    assert(d.ReadReg(kOut1) == 1383u);
    assert(d.ReadReg(kOut2) == kReferenceChecksum);
    assert(d.ReadReg(kOut3) == 3500u);

    d.WriteReg(kArg0, handle);
    d.Exec(kFree, 8);
    assert(d.ReadReg(kResult) == kOk);

    d.Exec(kComplete, 9);
    assert(d.ReadReg(kOut0) == kCompleteAck);
    assert(d.completed());
    std::printf("legacy F0-F3: checksum=0x%08x modeled=2530us speedup=1.383x\n",
                kReferenceChecksum);
}

// -- error paths ------------------------------------------------------------

void TestErrors() {
    Fixture f;
    Device& d = f.device;

    d.Exec(0x7F, 1);
    assert(d.ReadReg(kResult) == kBadCommand);

    d.WriteReg(kArg0, 999);
    d.Exec(kPoll, 2);
    assert(d.ReadReg(kResult) == kBadHandle);

    d.WriteReg(kArg0, 999);
    d.Exec(kFree, 3);
    assert(d.ReadReg(kResult) == kBadHandle);

    d.WriteReg(kArg0, 0);
    d.Exec(kAlloc, 4);
    assert(d.ReadReg(kResult) == kNoMemory);

    d.WriteReg(kArg0, kPoolBytes + 1);
    d.Exec(kAlloc, 5);
    assert(d.ReadReg(kResult) == kNoMemory);

    // COLLECT before the job is ready.
    d.WriteReg(kArg0, 4096);
    d.Exec(kAlloc, 6);
    const u32 handle = d.ReadReg(kOut0);
    UploadReference(d, handle);
    d.WriteReg(kArg0, handle);
    d.WriteReg(kArg2, 250000);
    d.WriteReg(kArg3, 3500);
    d.Exec(kSubmit, 7);
    const u32 job = d.ReadReg(kOut0);
    d.WriteReg(kArg0, job);
    d.Exec(kCollect, 8);
    assert(d.ReadReg(kResult) == kNotReady);

    // A freed handle stops resolving, and its slot is reissued.
    d.WriteReg(kArg0, handle);
    d.Exec(kFree, 9);
    d.WriteReg(kArg0, handle);
    d.Exec(kUpload, 10);
    assert(d.ReadReg(kResult) == kBadHandle);
    d.WriteReg(kArg0, 256);
    d.Exec(kAlloc, 11);
    assert(d.ReadReg(kOut0) == handle); // slot reuse is deliberate
    std::printf("error paths: bad command/handle/no memory/not ready\n");
}

// -- F4/F5 block transport --------------------------------------------------

void TestBlockSingle() {
    Fixture f;
    Device& d = f.device;

    d.WriteReg(kArg0, 4096);
    d.Exec(kAlloc, 1);
    const u32 handle = d.ReadReg(kOut0);

    u8 block[kBlockBytes];
    std::memset(block, 0, sizeof(block));
    const JobDescriptorV1 desc = MakeDescriptor(1, handle, true);
    std::memcpy(block, &desc, sizeof(desc));
    std::memcpy(block + sizeof(desc), kReferenceWords, sizeof(kReferenceWords));

    const u32 status = d.WriteBlock(kSelectorSubmission,
                                    EncodeWriteBlockWord(sizeof(desc) + 16), block, sizeof(block));
    assert(status == kOk);
    assert(d.completion_depth() == 1);
    assert(d.ReadReg(kEventCompletionDepth) == 1);

    u8 out[kBlockBytes];
    const std::size_t written =
        d.ReadBlock(kSelectorCompletion, EncodeReadBlockWord(0, kBlockBytes), out, sizeof(out));
    assert(written == sizeof(CompletionRecordV1));

    CompletionRecordV1 c{};
    std::memcpy(&c, out, sizeof(c));
    assert(c.magic == kCompletionMagic);
    assert(c.version == kBlockProtocolVersion);
    assert(c.status == kOk);
    assert(c.sequence == 1);
    assert(c.batch_size == 1);
    assert(c.checksum == kReferenceChecksum);
    assert(c.modeled_us == 2738u);
    assert(c.speedup_x1000 == 1278u);
    assert(c.transport_us_x1000 == 238396u);
    assert(d.completion_depth() == 0);

    // A correct record begins 47 4B 43 31 01 00 00 00 -- the byte pattern the
    // hardware bring-up looks for when diagnosing F5 misalignment.
    assert(out[0] == 0x47 && out[1] == 0x4B && out[2] == 0x43 && out[3] == 0x31);
    assert(out[4] == 0x01 && out[5] == 0x00 && out[6] == 0x00 && out[7] == 0x00);
    std::printf("block single: modeled=2738us speedup=1.278x transport=238.396us\n");
}

void TestBlockBatch8() {
    Fixture f;
    Device& d = f.device;

    d.WriteReg(kArg0, 4096);
    d.Exec(kAlloc, 1);
    const u32 handle = d.ReadReg(kOut0);
    UploadReference(d, handle);

    u8 block[kBlockBytes];
    std::memset(block, 0, sizeof(block));
    for (std::size_t i = 0; i < kMaxDescriptorsPerBlock; ++i) {
        const JobDescriptorV1 desc =
            MakeDescriptor(static_cast<u32>(100 + i), handle, false);
        std::memcpy(block + i * sizeof(desc), &desc, sizeof(desc));
    }

    assert(d.WriteBlock(kSelectorSubmission, EncodeWriteBlockWord(kBlockBytes), block,
                        sizeof(block)) == kOk);
    assert(d.completion_depth() == kMaxDescriptorsPerBlock);

    u8 out[kBlockBytes];
    const std::size_t written =
        d.ReadBlock(kSelectorCompletion, EncodeReadBlockWord(0, kBlockBytes), out, sizeof(out));
    assert(written == kBlockBytes);

    for (std::size_t i = 0; i < kMaxDescriptorsPerBlock; ++i) {
        CompletionRecordV1 c{};
        std::memcpy(&c, out + i * sizeof(c), sizeof(c));
        assert(c.magic == kCompletionMagic);
        assert(c.status == kOk);
        assert(c.sequence == 100 + i);
        assert(c.batch_size == kMaxDescriptorsPerBlock);
        assert(c.checksum == kReferenceChecksum);
        assert(c.modeled_us == 2530u);
        assert(c.speedup_x1000 == 1383u);
        assert(c.transport_us_x1000 == 29800u);
    }
    std::printf("block batch8: modeled/job=2530us speedup=1.383x transport/job=29.800us\n");
}

void TestBlockErrors() {
    Fixture f;
    Device& d = f.device;

    u8 block[kBlockBytes];
    std::memset(block, 0, sizeof(block));

    // Wrong selector.
    assert(d.WriteBlock(kSelectorCompletion, EncodeWriteBlockWord(64), block, sizeof(block)) ==
           kBadBlock);
    // Zero and oversized lengths.
    assert(d.WriteBlock(kSelectorSubmission, EncodeWriteBlockWord(0), block, sizeof(block)) ==
           kBadBlock);
    assert(d.WriteBlock(kSelectorSubmission, EncodeWriteBlockWord(kBlockBytes + 1), block,
                        sizeof(block)) == kBadBlock);
    // All-zero block: no descriptors at all.
    assert(d.WriteBlock(kSelectorSubmission, EncodeWriteBlockWord(kBlockBytes), block,
                        sizeof(block)) == kBadDescriptor);

    // Bad descriptor magic.
    JobDescriptorV1 desc = MakeDescriptor(1, 1, false);
    desc.magic = 0x21212121u;
    std::memcpy(block, &desc, sizeof(desc));
    assert(d.WriteBlock(kSelectorSubmission, EncodeWriteBlockWord(sizeof(desc)), block,
                        sizeof(block)) == kBadDescriptor);

    // Valid descriptor naming a handle that was never allocated: the block is
    // accepted, and the failure is reported per job in the completion record.
    desc = MakeDescriptor(7, 99, false);
    std::memset(block, 0, sizeof(block));
    std::memcpy(block, &desc, sizeof(desc));
    assert(d.WriteBlock(kSelectorSubmission, EncodeWriteBlockWord(sizeof(desc)), block,
                        sizeof(block)) == kOk);
    u8 out[kBlockBytes];
    assert(d.ReadBlock(kSelectorCompletion, EncodeReadBlockWord(0, kBlockBytes), out,
                       sizeof(out)) == sizeof(CompletionRecordV1));
    CompletionRecordV1 c{};
    std::memcpy(&c, out, sizeof(c));
    assert(c.status == kBadHandle);
    assert(c.sequence == 7);

    // F5 with a bad selector or non-zero offset returns nothing.
    assert(d.ReadBlock(kSelectorSubmission, EncodeReadBlockWord(0, kBlockBytes), out,
                       sizeof(out)) == 0);
    assert(d.ReadBlock(kSelectorCompletion, EncodeReadBlockWord(64, kBlockBytes), out,
                       sizeof(out)) == 0);
    std::printf("block errors: selector/length/descriptor/handle rejected\n");
}

void TestRoundTripChecksum() {
    // Write 512 bytes through F4 and read 512 bytes back through F5, and check
    // that what the device hashed is what was sent.
    Fixture f;
    Device& d = f.device;

    d.WriteReg(kArg0, 4096);
    d.Exec(kAlloc, 1);
    const u32 handle = d.ReadReg(kOut0);

    u8 pattern[256];
    for (std::size_t i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = static_cast<u8>(i * 7u + 3u);
    }

    u8 block[kBlockBytes];
    std::memset(block, 0, sizeof(block));
    JobDescriptorV1 desc = MakeDescriptor(42, handle, true);
    desc.input_length = sizeof(pattern);
    desc.output_length = sizeof(pattern);
    std::memcpy(block, &desc, sizeof(desc));
    std::memcpy(block + sizeof(desc), pattern, sizeof(pattern));

    assert(d.WriteBlock(kSelectorSubmission,
                        EncodeWriteBlockWord(sizeof(desc) + sizeof(pattern)), block,
                        sizeof(block)) == kOk);

    u8 out[kBlockBytes];
    assert(d.ReadBlock(kSelectorCompletion, EncodeReadBlockWord(0, kBlockBytes), out,
                       sizeof(out)) == sizeof(CompletionRecordV1));
    CompletionRecordV1 c{};
    std::memcpy(&c, out, sizeof(c));
    assert(c.status == kOk);
    assert(c.checksum == Fnv1a(pattern, sizeof(pattern)));
    std::printf("block round trip: 256B in, checksum 0x%08x matches host\n", c.checksum);
}

} // namespace

int main() {
    TestWireCodec();
    TestLegacyLifecycle();
    TestErrors();
    TestBlockSingle();
    TestBlockBatch8();
    TestBlockErrors();
    TestRoundTripChecksum();
    std::printf("device core: all checks passed\n");
    return 0;
}
