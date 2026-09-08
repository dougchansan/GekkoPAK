// DSpico F5 data-phase tests.
//
// These drive prototype/dspico-v1/overlay/src/gekkopakNtr.cpp -- the file the
// RP2040 firmware compiles -- through the host shim, and cover the parts of the
// F5 path the cross-target golden vectors cannot:
//
//   - the raw diagnostic window, which is firmware-local and has no emulator
//     counterpart, so it can never appear in a vector;
//   - the double buffering, which only shows up when a second transfer follows
//     the first;
//   - the ordering rule the hardware defect came down to.
//
// The diagnostic window exists so that a failure on real hardware can be
// attributed. If the pattern comes back intact, the 512-byte cartridge-to-
// console data phase works and any remaining fault is in the protocol above it.
// If the pattern comes back shifted, truncated, byte-swapped or stale, the
// transport is still wrong and the shape of the corruption says how.

#include <cassert>
#include <cstdio>
#include <cstring>

#include "gekkopak/protocol.h"
#include "prototype/dspico-v1/hostshim/dspico_host_shim.h"

using namespace gekkopak::protocol;

namespace {

constexpr std::uint8_t kSelectorDiagnostic = 0x7E;

// Mirrors buildDiagnosticBlock() in the overlay. Deliberately restated rather
// than shared: if the firmware's generator drifts, this notices.
std::uint32_t ExpectedWord(std::uint32_t i) {
    return (0xF5u << 24) | (i << 16) | ((i ^ 0x7Fu) << 8) | ((i + 0xA5u) & 0xFFu);
}

void Issue(std::uint8_t opcode, std::uint8_t index, std::uint32_t word,
           const std::uint8_t* in, std::size_t in_len, std::uint8_t* out, std::size_t out_len) {
    std::uint8_t cmd[kCommandBytes];
    EncodeCommand(opcode, index, word, cmd);
    const dspico_shim::CommandResult r =
        dspico_shim::IssueCommand(cmd, in, in_len, out, out_len);
    assert(!r.direction_mismatch);
    assert(!r.payload_length_mismatch);
}

std::uint32_t ReadReg(std::uint8_t index) {
    std::uint8_t out[4]{};
    Issue(kWireReadReg, index, 0, nullptr, 0, out, sizeof(out));
    std::uint32_t value = 0;
    std::memcpy(&value, out, sizeof(value));
    return value;
}

// -- tests ------------------------------------------------------------------

void TestDiagnosticPatternIsExact() {
    dspico_shim::Reset();

    std::uint8_t out[kBlockBytes];
    std::memset(out, 0, sizeof(out));
    Issue(kWireReadBlock, kSelectorDiagnostic, EncodeReadBlockWord(0, kBlockBytes), nullptr, 0,
          out, sizeof(out));

    for (std::uint32_t i = 0; i < kBlockBytes / 4u; ++i) {
        std::uint32_t word = 0;
        std::memcpy(&word, out + i * 4u, sizeof(word));
        if (word != ExpectedWord(i)) {
            std::printf("  word %u: got 0x%08X expected 0x%08X\n", i, word, ExpectedWord(i));
        }
        assert(word == ExpectedWord(i));
    }

    // Every failure mode the pattern is meant to separate looks different from
    // a correct read, and each is distinguishable from the others.
    assert(out[0] == 0xA5 && out[1] == 0x7F && out[2] == 0x00 && out[3] == 0xF5);
    // A 12-byte skew -- the symptom recorded on hardware -- would put word 3
    // where word 0 belongs.
    assert(ExpectedWord(3) != ExpectedWord(0));
    std::printf("F5 diagnostic: 128 words exact, first bytes A5 7F 00 F5\n");
}

void TestDiagnosticIsStateless() {
    dspico_shim::Reset();

    std::uint8_t first[kBlockBytes];
    std::uint8_t second[kBlockBytes];
    Issue(kWireReadBlock, kSelectorDiagnostic, EncodeReadBlockWord(0, kBlockBytes), nullptr, 0,
          first, sizeof(first));
    Issue(kWireReadBlock, kSelectorDiagnostic, EncodeReadBlockWord(0, kBlockBytes), nullptr, 0,
          second, sizeof(second));
    assert(std::memcmp(first, second, sizeof(first)) == 0);

    // It consumes nothing, so it cannot perturb a run it is diagnosing.
    assert(ReadReg(kEventCompletionDepth) == 0);
    std::printf("F5 diagnostic: repeatable and consumes nothing\n");
}

void TestUnknownSelectorReturnsZeros() {
    dspico_shim::Reset();

    std::uint8_t out[kBlockBytes];
    std::memset(out, 0xAA, sizeof(out));
    Issue(kWireReadBlock, 0x40, EncodeReadBlockWord(0, kBlockBytes), nullptr, 0, out,
          sizeof(out));

    for (std::size_t i = 0; i < sizeof(out); ++i) {
        assert(out[i] == 0);
    }
    // The console clocked a data phase either way, so it gets zeros rather than
    // whatever happened to be staged -- and the verdict still reaches RESULT.
    assert(ReadReg(kResult) == kBadBlock);
    std::printf("F5 unknown selector: zeroed block, BadBlock reported\n");
}

void TestBackToBackCompletionReadsDoNotTear() {
    dspico_shim::Reset();

    // Allocate, then submit a descriptor with inline input.
    Issue(kWireWriteReg, kArg0, 4096, nullptr, 0, nullptr, 0);
    Issue(kWireExec, kAlloc, 1, nullptr, 0, nullptr, 0);
    assert(ReadReg(kOut0) == 1);

    std::uint8_t block[kBlockBytes];
    std::memset(block, 0, sizeof(block));
    JobDescriptorV1 desc{};
    desc.sequence = 11;
    desc.flags = kDescriptorFlagInlineInput;
    desc.input_handle = 1;
    desc.input_length = 16;
    desc.kernel_id = 1;
    desc.work_units = 250000;
    desc.arg0 = 3500;
    std::memcpy(block, &desc, sizeof(desc));
    const std::uint32_t reference[4] = {0x11223344u, 0x55667788u, 0xAABBCCDDu, 0x0BADF00Du};
    std::memcpy(block + sizeof(desc), reference, sizeof(reference));
    Issue(kWireWriteBlock, kSelectorSubmission, EncodeWriteBlockWord(80), block, sizeof(block),
          nullptr, 0);

    // The completion is queued and visible before it is read: staging must be a
    // peek, or the event check would see an empty queue.
    assert(ReadReg(kEventCompletionDepth) == 1);

    std::uint8_t out[kBlockBytes];
    Issue(kWireReadBlock, kSelectorCompletion, EncodeReadBlockWord(0, kBlockBytes), nullptr, 0,
          out, sizeof(out));
    CompletionRecordV1 record{};
    std::memcpy(&record, out, sizeof(record));
    assert(record.magic == kCompletionMagic);
    assert(record.sequence == 11);
    assert(record.checksum == kReferenceChecksum);

    // Immediately again: the queue is drained, and the second read must return a
    // clean empty block rather than a torn or repeated copy of the first.
    assert(ReadReg(kEventCompletionDepth) == 0);
    std::uint8_t again[kBlockBytes];
    Issue(kWireReadBlock, kSelectorCompletion, EncodeReadBlockWord(0, kBlockBytes), nullptr, 0,
          again, sizeof(again));
    for (std::size_t i = 0; i < sizeof(again); ++i) {
        assert(again[i] == 0);
    }
    std::printf("F5 completion: staged, drained once, second read clean\n");
}

void TestF5SentCounterTracksTransfers() {
    dspico_shim::Reset();
    assert(ReadReg(0xF4) == 0);

    std::uint8_t out[kBlockBytes];
    Issue(kWireReadBlock, kSelectorCompletion, EncodeReadBlockWord(0, kBlockBytes), nullptr, 0,
          out, sizeof(out));
    Issue(kWireReadBlock, kSelectorDiagnostic, EncodeReadBlockWord(0, kBlockBytes), nullptr, 0,
          out, sizeof(out));
    // Counts data phases armed, so a console that sees nothing can still tell
    // whether the cartridge tried.
    assert(ReadReg(0xF4) == 2);
    std::printf("F5 instrumentation: armed-transfer counter readable over F2\n");
}

} // namespace

int main() {
    TestDiagnosticPatternIsExact();
    TestDiagnosticIsStateless();
    TestUnknownSelectorReturnsZeros();
    TestBackToBackCompletionReadsDoNotTear();
    TestF5SentCounterTracksTransfers();
    std::printf("dspico f5: all checks passed\n");
    return 0;
}
