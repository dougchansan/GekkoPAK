// GekkoPAK virtual NTRCARD device for Azahar.
//
// This is the Azahar transport adapter. It owns the BackingMem that gets mapped
// into the guest address space at the NTRCARD register page, the per-CPU-slice
// service tick, and Azahar's logging. Every protocol decision lives in
// gekkopak::Device, the same core the host model and the DSpico RP2040 firmware
// drive, and the register-page command decode is the same shared header the
// host model uses -- so "the Azahar device and the host model agree" is a
// property of the build, not a claim about two copies.
//
// See docs/CONFORMANCE_ARCHITECTURE.md.

#include "core/hle/device/gekkopak_ntr.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>
#include "common/logging/log.h"
#include "common/memory_ref.h"
#include "gekkopak/device.h"
#include "gekkopak/ntr_register_transport.h"
#include "gekkopak/protocol.h"

namespace GekkoPakNtr {
namespace {

// The emulated device advertises the 32 MiB local-memory target rather than the
// RP2040's real 64 KiB. That is a device capability difference, not a protocol
// difference; the conformance runner configures both with the same pool so
// responses are byte-identical.
constexpr u32 LocalMemoryBytes = 32u * 1024u * 1024u;

struct State {
    std::shared_ptr<BufferMem> regs =
        std::make_shared<BufferMem>(gekkopak::ntr_transport::kRegisterPageSize);
    std::vector<u8> pool = std::vector<u8>(LocalMemoryBytes, 0);
    gekkopak::Device core;
    bool initialised = false;
};

State& GetState() {
    static State state;
    return state;
}

} // namespace

std::shared_ptr<BackingMem> GetRegisterMemory() {
    return GetState().regs;
}

void Reset() {
    auto& s = GetState();
    std::fill(s.regs->Vector().begin(), s.regs->Vector().end(), 0);

    gekkopak::Device::Config config;
    config.pool = s.pool.data();
    config.pool_bytes = LocalMemoryBytes;
    config.reported_local_bytes = LocalMemoryBytes;
    s.core.Reset(config);
    s.initialised = true;

    gekkopak::ntr_transport::Store32(s.regs->Vector().data(), gekkopak::ntr_transport::kRegRomCnt,
                                     gekkopak::ntr_transport::kCardResetHigh);
    LOG_INFO(Core, "GekkoPAK NTR virtual cartridge reset: protocol=0x{:08X} caps=0x{:08X} "
                   "local={} MiB",
             gekkopak::protocol::kProtocolVersion, gekkopak::protocol::kCaps,
             LocalMemoryBytes / (1024u * 1024u));
}

void Tick() {
    auto& s = GetState();
    if (!s.initialised) {
        return;
    }

    const bool serviced = gekkopak::ntr_transport::Tick(s.core, s.regs->Vector().data());
    if (!serviced) {
        return;
    }

    // One line per serviced command, so the Azahar command stream can be
    // compared against the golden vectors. Off unless GEKKOPAK_TRACE is set,
    // because a benchmarking guest would otherwise flood the log.
    static const bool trace_enabled = std::getenv("GEKKOPAK_TRACE") != nullptr;
    if (trace_enabled) {
        const u8* cmd = s.regs->Vector().data() + gekkopak::ntr_transport::kRegCommand;
        LOG_INFO(Core,
                 "GKPAK-TRACE {:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X} depth={}",
                 cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5], cmd[6], cmd[7],
                 s.core.completion_depth());
    }

    if (s.core.completed()) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            LOG_INFO(Core, "GekkoPAK NTR guest completed: transfers={}", s.core.transfers());
        }
    }
}

} // namespace GekkoPakNtr
