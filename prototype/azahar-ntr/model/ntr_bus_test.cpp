#include "gekkopak_ntr_model.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace gekkopak::ntr;

static void store32(std::uint8_t* p, std::size_t off, std::uint32_t v) {
    std::memcpy(p + off, &v, sizeof(v));
}
static std::uint32_t load32(const std::uint8_t* p, std::size_t off) {
    std::uint32_t v{};
    std::memcpy(&v, p + off, sizeof(v));
    return v;
}

static std::uint64_t wire(Device& d, std::uint8_t op, std::uint8_t index,
                          std::uint32_t value, bool read = false) {
    auto* r = d.registers();
    // Canonical big-endian wire form, the order DSpico's PIO delivers.
    std::uint8_t cmd[protocol::kCommandBytes]{};
    protocol::EncodeCommand(op, index, value, cmd);
    std::memcpy(r + kRegCommand, cmd, sizeof(cmd));
    // A four-byte read declares block size 7; everything else declares none.
    store32(r, kRegRomCnt,
            kCardResetHigh | kCardActivate | (read ? kCardBlock4 : kCardBlockNone));

    // Drive the data phase the way the guest does: take each word the cartridge
    // offers and clear DATA_READY to acknowledge it.
    std::uint32_t response = 0;
    for (int guard = 0; guard < 64; ++guard) {
        d.Tick();
        const std::uint32_t romcnt = load32(r, kRegRomCnt);
        if ((romcnt & kCardActivate) == 0)
            break;
        if ((romcnt & kCardDataReady) == 0)
            continue;
        response = load32(r, kRegFifo);
        store32(r, kRegRomCnt, romcnt & ~kCardDataReady);
    }
    assert((load32(r, kRegRomCnt) & kCardActivate) == 0);
    return read ? response : 0;
}

static void wr(Device& d, Register reg, std::uint32_t value) {
    wire(d, kWireWriteReg, static_cast<std::uint8_t>(reg), value);
}
static std::uint32_t rd(Device& d, Register reg) {
    return static_cast<std::uint32_t>(
        wire(d, kWireReadReg, static_cast<std::uint8_t>(reg), 0, true));
}
static void payload_word(Device& d, std::uint8_t word_index, std::uint32_t value) {
    wire(d, kWireWritePayloadWord, word_index, value);
}
static void exec(Device& d, HighCommand cmd, std::uint32_t seq) {
    wire(d, kWireExec, static_cast<std::uint8_t>(cmd), seq);
    assert(rd(d, Result) == Ok);
}

int main() {
    Device d;
    std::uint32_t seq = 0;

    exec(d, Hello, ++seq);
    assert(rd(d, Out0) == kProtocolVersion);
    assert(rd(d, Out3) == 2);
    std::printf("HELLO protocol=%u.%u transport=NTRCARD\n", rd(d, Out0) >> 16,
                rd(d, Out0) & 0xffff);

    exec(d, GetCaps, ++seq);
    std::printf("CAPS=0x%08x local=%u MiB\n", rd(d, Out0), rd(d, Out1) / (1024 * 1024));

    wr(d, Arg0, 4096);
    exec(d, Alloc, ++seq);
    const auto alloc = rd(d, Out0);
    assert(alloc != 0);

    payload_word(d, 0, 0x11223344);
    payload_word(d, 1, 0x55667788);
    payload_word(d, 2, 0xAABBCCDD);
    payload_word(d, 3, 0x0BADF00D);
    wr(d, Arg0, alloc);
    wr(d, Arg1, 0);
    wr(d, PayloadLen, 16);
    exec(d, Upload, ++seq);
    const auto upload_checksum = rd(d, Out1);
    assert(upload_checksum == 0xF269B734u);

    wr(d, Arg0, alloc);
    wr(d, Arg1, 1);
    wr(d, Arg2, 250000);
    wr(d, Arg3, 3500);
    exec(d, Submit, ++seq);
    const auto job = rd(d, Out0);

    do {
        wr(d, Arg0, job);
        exec(d, Poll, ++seq);
    } while (rd(d, Out0) == 0);

    wr(d, Arg0, job);
    exec(d, Collect, ++seq);
    const auto offload_us = rd(d, Out0);
    const auto speedup = rd(d, Out1);
    const auto checksum = rd(d, Out2);
    assert(checksum == upload_checksum);

    wr(d, Arg0, alloc);
    exec(d, Free, ++seq);
    exec(d, Complete, ++seq);
    assert(d.completed());

    std::printf("PASS offload=%u us speedup=%u.%03ux checksum=0x%08x wire_transfers=%llu\n",
                offload_us, speedup / 1000, speedup % 1000, checksum,
                static_cast<unsigned long long>(d.transfers()));
    return 0;
}
