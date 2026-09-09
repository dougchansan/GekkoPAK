// Cartridge-side trace ring, driven through the real DSpico handlers.
//
// The ring is the automation channel's producer half: it is what lets a host
// see what the cartridge saw, rather than only what the console noticed. Three
// properties have to hold for that to be worth anything, and all three are
// checked here rather than on hardware, where a wrong answer costs a card swap
// and a photograph.
//
//   - It is off by default. A timing campaign must measure the transport, not
//     the transport plus instrumentation, so tracing is opt-in per category.
//   - It records what actually happened, including the outcomes the console
//     cannot observe -- a refused F4, a rejected descriptor block, an F5 armed
//     from the wrong buffer.
//   - It loses records honestly. A full ring drops and says how many; it never
//     blocks the interrupt that feeds it, and it never overwrites history.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gekkopak/protocol.h"
#include "gekkopakTrace.h"
#include "prototype/dspico-v1/hostshim/dspico_host_shim.h"

using namespace gekkopak::protocol;

namespace {

constexpr std::uint8_t kSelectorDiagnostic = 0x7E;

void Issue(std::uint8_t opcode, std::uint8_t index, std::uint32_t word,
           const std::uint8_t* in, std::size_t in_len, std::uint8_t* out, std::size_t out_len) {
    std::uint8_t cmd[kCommandBytes];
    EncodeCommand(opcode, index, word, cmd);
    const dspico_shim::CommandResult r =
        dspico_shim::IssueCommand(cmd, in, in_len, out, out_len);
    assert(!r.direction_mismatch);
    assert(!r.payload_length_mismatch);
}

std::vector<gpk_trace_record_t> Drain() {
    std::vector<gpk_trace_record_t> out;
    gpk_trace_record_t record{};
    while (gpk_trace_pop(&record)) {
        out.push_back(record);
    }
    return out;
}

// Submits one valid v1 descriptor block, which queues a completion.
void SubmitJob(std::uint32_t sequence) {
    Issue(kWireWriteReg, kArg0, 4096, nullptr, 0, nullptr, 0);
    Issue(kWireExec, kAlloc, 1, nullptr, 0, nullptr, 0);

    std::uint8_t block[kBlockBytes];
    std::memset(block, 0, sizeof(block));
    JobDescriptorV1 desc{};
    desc.sequence = sequence;
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
}

// -- tests ------------------------------------------------------------------

void TestTracingIsOffByDefault() {
    gpk_trace_set_mask(0);
    dspico_shim::Reset();

    SubmitJob(1);
    std::uint8_t out[kBlockBytes];
    Issue(kWireReadBlock, kSelectorCompletion, EncodeReadBlockWord(0, kBlockBytes), nullptr, 0,
          out, sizeof(out));

    // A full submit-and-collect round trip leaves no trace at all, so a timing
    // run measures the transport rather than the instrumentation.
    assert(gpk_trace_depth() == 0);
    assert(gpk_trace_dropped() == 0);
    std::printf("trace: silent until asked\n");
}

void TestCategoriesAreIndependent() {
    gpk_trace_set_mask(1u << GPK_TRACE_WRITE_REG);
    dspico_shim::Reset();

    Issue(kWireWriteReg, kArg0, 0xC0FFEEu, nullptr, 0, nullptr, 0);
    Issue(kWireExec, kAlloc, 1, nullptr, 0, nullptr, 0);
    std::uint8_t reg[4];
    Issue(kWireReadReg, kOut0, 0, nullptr, 0, reg, sizeof(reg));

    const std::vector<gpk_trace_record_t> records = Drain();
    // Three commands went by; only the requested category was recorded.
    assert(records.size() == 1);
    assert(records[0].kind == GPK_TRACE_WRITE_REG);
    assert(records[0].index == kArg0);
    assert(records[0].value == 0xC0FFEEu);

    // F3 is excluded from the convenience mask: it is per-word and voluminous.
    assert((GPK_TRACE_MASK_DEFAULT_ON & (1u << GPK_TRACE_PAYLOAD)) == 0);
    assert((GPK_TRACE_MASK_DEFAULT_ON & (1u << GPK_TRACE_BLOCK_PARSE)) != 0);
    std::printf("trace: categories independently selectable\n");
}

void TestRefusedBlockIsRecorded() {
    gpk_trace_set_mask(GPK_TRACE_MASK_DEFAULT_ON);
    dspico_shim::Reset();
    Drain();

    // A zero-length F4. The handler refuses the data phase, and from the
    // console this is indistinguishable from a block that was delivered and
    // then rejected -- nothing arrives either way.
    Issue(kWireWriteBlock, kSelectorSubmission, EncodeWriteBlockWord(0), nullptr, 0, nullptr, 0);

    const std::vector<gpk_trace_record_t> records = Drain();
    bool seen = false;
    for (const gpk_trace_record_t& r : records) {
        if (r.kind == GPK_TRACE_BLOCK_IN) {
            assert(r.aux == GPK_TRACE_BLOCK_REFUSED);
            seen = true;
        }
        // A refused command never reaches the data phase, so it must not be
        // reported as parsed.
        assert(r.kind != GPK_TRACE_BLOCK_PARSE);
    }
    assert(seen);
    std::printf("trace: refused F4 distinguishable from a rejected one\n");
}

void TestReadBlockSourceIsRecorded() {
    gpk_trace_set_mask(GPK_TRACE_MASK_DEFAULT_ON);
    dspico_shim::Reset();
    Drain();

    std::uint8_t out[kBlockBytes];
    // An unknown selector still gets a data phase, driven from the zeroes
    // buffer. Recording the source is what separates "the transport worked and
    // the block was empty" from "the selector was wrong".
    Issue(kWireReadBlock, 0x40, EncodeReadBlockWord(0, kBlockBytes), nullptr, 0, out, sizeof(out));
    Issue(kWireReadBlock, kSelectorDiagnostic, EncodeReadBlockWord(0, kBlockBytes), nullptr, 0,
          out, sizeof(out));

    std::vector<std::uint16_t> sources;
    for (const gpk_trace_record_t& r : Drain()) {
        if (r.kind == GPK_TRACE_BLOCK_ARM) {
            sources.push_back(r.aux);
        }
    }
    assert(sources.size() == 2);
    assert(sources[0] == GPK_TRACE_SOURCE_ZEROES);
    assert(sources[1] == GPK_TRACE_SOURCE_DIAGNOSTIC);
    std::printf("trace: F5 records which buffer was driven\n");
}

void TestCompletionLifecycleIsVisible() {
    gpk_trace_set_mask(GPK_TRACE_MASK_DEFAULT_ON);
    dspico_shim::Reset();
    Drain();

    SubmitJob(7);
    std::uint8_t out[kBlockBytes];
    Issue(kWireReadBlock, kSelectorCompletion, EncodeReadBlockWord(0, kBlockBytes), nullptr, 0,
          out, sizeof(out));

    bool accepted = false, parsed = false, armed = false, acked = false;
    std::uint32_t dropped_records = 0;
    for (const gpk_trace_record_t& r : Drain()) {
        if (r.kind == GPK_TRACE_BLOCK_IN && r.aux == GPK_TRACE_BLOCK_ACCEPTED) accepted = true;
        if (r.kind == GPK_TRACE_BLOCK_PARSE && r.aux == kOk) parsed = true;
        if (r.kind == GPK_TRACE_BLOCK_ARM && r.aux == GPK_TRACE_SOURCE_COMPLETION) armed = true;
        if (r.kind == GPK_TRACE_BLOCK_ACK && r.aux == kOk) {
            acked = true;
            dropped_records = r.value;
        }
    }
    // The whole path a job takes through the cartridge, in one transcript:
    // accepted, parsed, staged, sent, and the queue advanced by exactly the one
    // record that was sent.
    assert(accepted && parsed && armed && acked);
    assert(dropped_records == 1);
    std::printf("trace: submit-to-completion lifecycle visible end to end\n");
}

void TestFullRingDropsAndReportsIt() {
    gpk_trace_set_mask(1u << GPK_TRACE_WRITE_REG);
    dspico_shim::Reset();
    Drain();
    gpk_trace_reset();

    // Deliberately overrun. The ring holds CAPACITY-1 records, because one slot
    // separates full from empty.
    const std::uint32_t issued = GPK_TRACE_CAPACITY + 50u;
    for (std::uint32_t i = 0; i < issued; ++i) {
        Issue(kWireWriteReg, kArg0, i, nullptr, 0, nullptr, 0);
    }

    const std::uint32_t dropped = gpk_trace_dropped();
    const std::vector<gpk_trace_record_t> records = Drain();
    assert(records.size() == GPK_TRACE_CAPACITY - 1u);
    assert(dropped == issued - (GPK_TRACE_CAPACITY - 1u));

    // The oldest records survive: what a run started with is what explains the
    // rest of it, and a reader is told about the hole rather than left to infer
    // it from a gap in the timestamps.
    assert(records.front().value == 0);
    assert(records[1].value == 1);
    std::printf("trace: full ring keeps the oldest %u and reports %u dropped\n",
                GPK_TRACE_CAPACITY - 1u, dropped);
}

} // namespace

int main() {
    TestTracingIsOffByDefault();
    TestCategoriesAreIndependent();
    TestRefusedBlockIsRecorded();
    TestReadBlockSourceIsRecorded();
    TestCompletionLifecycleIsVisible();
    TestFullRingDropsAndReportsIt();
    std::printf("dspico trace: all checks passed\n");
    return 0;
}
