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

void RegReset() {
    std::memset(gRegPage, 0, sizeof(gRegPage));
    Device::Config config;
    config.pool = gRegPool;
    config.pool_bytes = kConformancePoolBytes;
    config.reported_local_bytes = kConformancePoolBytes;
    gRegCore.Reset(config);
    ntr_transport::Store32(gRegPage, ntr_transport::kRegRomCnt, ntr_transport::kCardResetHigh);
}

bool RegIssue(const std::uint8_t command[8], const std::uint8_t* in, std::size_t in_len,
              std::uint8_t* out, std::size_t out_len) {
    if (out != nullptr) {
        std::memset(out, 0, out_len);
    }
    if (in != nullptr) {
        if (in_len > gp::kBlockBytes) {
            return false;
        }
        std::memset(gRegPage + ntr_transport::kRegBlockTx, 0, gp::kBlockBytes);
        std::memcpy(gRegPage + ntr_transport::kRegBlockTx, in, in_len);
    }

    std::memcpy(gRegPage + ntr_transport::kRegCommand, command, gp::kCommandBytes);
    ntr_transport::Store32(gRegPage, ntr_transport::kRegRomCnt,
                           ntr_transport::kCardResetHigh | ntr_transport::kCardActivate |
                               (command[0] == gp::kWireReadReg ? ntr_transport::kCardBlock4 : 0u));
    if (!ntr_transport::Tick(gRegCore, gRegPage)) {
        return false;
    }

    if (out == nullptr) {
        return true;
    }
    if (command[0] == gp::kWireReadBlock) {
        if (out_len < gp::kBlockBytes) {
            return false;
        }
        std::memcpy(out, gRegPage + ntr_transport::kRegBlockRx, gp::kBlockBytes);
        return true;
    }
    if (out_len < 4) {
        return false;
    }
    StoreLe32(out, ntr_transport::Load32(gRegPage, ntr_transport::kRegFifo));
    return true;
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
