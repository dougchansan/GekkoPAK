#include "targets.h"

#include <cstring>

#include "gekkopak/device.h"
#include "gekkopak/ntr_register_transport.h"
#include "gekkopak/protocol.h"
#include "prototype/dspico-v1/hostshim/dspico_host_shim.h"

namespace gekkopak {
namespace conformance {
namespace {

namespace gp = protocol;

void StoreLe32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8);
    out[2] = static_cast<std::uint8_t>(value >> 16);
    out[3] = static_cast<std::uint8_t>(value >> 24);
}

// ---------------------------------------------------------------------------
// Target 1: the shared device core, driven through its own API.
//
// No register page and no bus: this is the reference for what the protocol
// means, and it exists so that a transport bug can be told apart from a
// protocol bug.
// ---------------------------------------------------------------------------

std::uint8_t gCorePool[kConformancePoolBytes];
Device gCore;

void CoreReset() {
    Device::Config config;
    config.pool = gCorePool;
    config.pool_bytes = kConformancePoolBytes;
    config.reported_local_bytes = kConformancePoolBytes;
    gCore.Reset(config);
}

bool CoreIssue(const std::uint8_t command[8], const std::uint8_t* in, std::size_t in_len,
               std::uint8_t* out, std::size_t out_len) {
    if (out != nullptr) {
        std::memset(out, 0, out_len);
    }
    const gp::WireCommand cmd = gp::DecodeCommand(command);
    gCore.count_transfer();
    if (!cmd.valid) {
        return true; // dropped, exactly as a cartridge would
    }

    switch (cmd.opcode) {
    case gp::kWireWriteReg:
        gCore.WriteReg(cmd.index, cmd.word);
        return true;
    case gp::kWireExec:
        gCore.Exec(cmd.index, cmd.word);
        return true;
    case gp::kWireReadReg:
        if (out == nullptr || out_len < 4) {
            return false;
        }
        StoreLe32(out, gCore.ReadReg(cmd.index));
        return true;
    case gp::kWireWritePayloadWord:
        gCore.WritePayloadWord(cmd.index, cmd.word);
        return true;
    case gp::kWireWriteBlock:
        if (in == nullptr || in_len < gp::kBlockBytes) {
            return false;
        }
        gCore.WriteBlock(cmd.index, cmd.word, in, in_len);
        return true;
    case gp::kWireReadBlock:
        if (out == nullptr || out_len < gp::kBlockBytes) {
            return false;
        }
        gCore.ReadBlock(cmd.index, cmd.word, out, out_len);
        return true;
    default:
        return true;
    }
}

// ---------------------------------------------------------------------------
// Target 2: the register-page transport.
//
// This is the Azahar device model. Azahar's own device differs only in where
// the 4 KiB page lives (a BackingMem mapped into the guest) and in its logging;
// the command decode is the same ntr_transport::Tick() the overlay calls.
// ---------------------------------------------------------------------------

std::uint8_t gRegPool[kConformancePoolBytes];
std::uint8_t gRegPage[ntr_transport::kRegisterPageSize];
Device gRegCore;
ntr_transport::RegisterTransport gRegTransport;

void RegReset() {
    std::memset(gRegPage, 0, sizeof(gRegPage));
    Device::Config config;
    config.pool = gRegPool;
    config.pool_bytes = kConformancePoolBytes;
    config.reported_local_bytes = kConformancePoolBytes;
    gRegCore.Reset(config);
    gRegTransport.Reset(gRegPage);
}

// Drives a full transaction the way the ARM guest does: stage the command,
// program the block-size field the opcode requires, then move the data phase
// through the FIFO a word at a time, acknowledging each one.
bool RegIssue(const std::uint8_t command[8], const std::uint8_t* in, std::size_t in_len,
              std::uint8_t* out, std::size_t out_len) {
    if (out != nullptr) {
        std::memset(out, 0, out_len);
    }

    const ntr_transport::ExpectedPhase phase = ntr_transport::PhaseForOpcode(command[0]);
    const std::size_t words = phase.bytes / 4u;
    if (phase.direction == ntr_transport::PhaseDirection::CartToConsole &&
        (out == nullptr || out_len < phase.bytes)) {
        return false;
    }
    if (phase.direction == ntr_transport::PhaseDirection::ConsoleToCart &&
        (in == nullptr || in_len < phase.bytes)) {
        return false;
    }

    std::memcpy(gRegPage + ntr_transport::kRegCommand, command, gp::kCommandBytes);
    ntr_transport::Store32(gRegPage, ntr_transport::kRegRomCnt,
                           ntr_transport::kCardResetHigh | ntr_transport::kCardActivate |
                               phase.block_field);

    std::size_t word = 0;
    // Bounded so a transport bug fails the test instead of hanging it.
    for (std::size_t guard = 0; guard < 4u * (words + 8u); ++guard) {
        gRegTransport.Tick(gRegCore, gRegPage);
        const std::uint32_t romcnt = ntr_transport::Load32(gRegPage, ntr_transport::kRegRomCnt);
        if ((romcnt & ntr_transport::kCardActivate) == 0) {
            return word == words;
        }
        if ((romcnt & ntr_transport::kCardDataReady) == 0) {
            continue;
        }
        if (word >= words) {
            return false; // more words offered than the block size declared
        }
        if (phase.direction == ntr_transport::PhaseDirection::CartToConsole) {
            const std::uint32_t value =
                ntr_transport::Load32(gRegPage, ntr_transport::kRegFifo);
            std::memcpy(out + word * 4u, &value, sizeof(value));
        } else {
            std::uint32_t value = 0;
            std::memcpy(&value, in + word * 4u, sizeof(value));
            ntr_transport::Store32(gRegPage, ntr_transport::kRegFifo, value);
        }
        ++word;
        ntr_transport::Store32(gRegPage, ntr_transport::kRegRomCnt,
                               romcnt & ~ntr_transport::kCardDataReady);
    }
    return false; // transfer never completed
}

// ---------------------------------------------------------------------------
// Target 3: the DSpico firmware handlers, on the host.
//
// prototype/dspico-v1/overlay/src/gekkopakNtr.cpp compiled unmodified against
// the host shim -- the same file the RP2040 firmware compiles.
// ---------------------------------------------------------------------------

void DspicoReset() {
    dspico_shim::Reset();
}

bool DspicoIssue(const std::uint8_t command[8], const std::uint8_t* in, std::size_t in_len,
                 std::uint8_t* out, std::size_t out_len) {
    if (out != nullptr) {
        std::memset(out, 0, out_len);
    }
    const dspico_shim::CommandResult result =
        dspico_shim::IssueCommand(command, in, in_len, out, out_len);
    // A handler that declares one data-phase length and then supplies another
    // does not fail cleanly on hardware -- it shifts the payload and leaves the
    // next transaction desynchronised. Treat it as a conformance failure here,
    // where it is still cheap to find.
    return !result.direction_mismatch && !result.payload_length_mismatch;
}

} // namespace

const std::vector<Target>& AllTargets() {
    static const std::vector<Target> targets = {
        Target{"host-core", CoreReset, CoreIssue},
        Target{"azahar-model", RegReset, RegIssue},
        Target{"dspico-shim", DspicoReset, DspicoIssue},
    };
    return targets;
}

} // namespace conformance
} // namespace gekkopak
