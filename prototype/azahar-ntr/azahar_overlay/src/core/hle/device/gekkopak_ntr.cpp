// GekkoPAK virtual NTRCARD device for Azahar.
//
// This is the Azahar transport adapter. It owns the MMIO window mapped into the
// guest address space and Azahar's logging, and nothing else. Every protocol
// decision lives in gekkopak::Device -- the same core the host model and the
// DSpico RP2040 firmware drive -- and the register decode is the same shared
// header the host model uses, so "the Azahar device and the host model agree"
// is a property of the build rather than a claim about two copies.
//
// The window is MMIO rather than backing memory because the transport is
// access-driven: reading the data FIFO is what advances a transfer and clears
// DATA_READY, exactly as on hardware. Azahar had no MMIO page type -- Citra's
// MMIORegion was removed -- so the overlay adds one back. See
// docs/AZAHAR_MMIO.md and docs/CONFORMANCE_ARCHITECTURE.md.

#include "core/hle/device/gekkopak_ntr.h"

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
    gekkopak::ntr_transport::RegisterTransport transport;
    u32 reported_faults = 0;
    u64 traced_commands = 0;
    bool announced_complete = false;
    bool mapped = false;
};

State& GetState() {
    static State state;
    return state;
}

// One line per accepted command -- not per data-phase access, of which a
// 512-byte block has 128. Off unless GEKKOPAK_TRACE is set, because a
// benchmarking guest would otherwise flood the log.
void TraceCommand(State& s) {
    static const bool enabled = std::getenv("GEKKOPAK_TRACE") != nullptr;
    if (!enabled || s.transport.commands() == s.traced_commands) {
        return;
    }
    s.traced_commands = s.transport.commands();
    const u8* cmd = s.regs->Vector().data() + gekkopak::ntr_transport::kRegCommand;
    LOG_INFO(Core, "GKPAK-TRACE {:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X} depth={}",
             cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5], cmd[6], cmd[7],
             s.core.completion_depth());
}

void ReportFaults(State& s) {
    if (s.transport.fault_count() == s.reported_faults) {
        return;
    }
    s.reported_faults = s.transport.fault_count();
    // A guest that programs the wrong ROMCNT block size for its opcode, or
    // touches the FIFO out of phase, gets no data. On hardware that
    // desynchronises the bus and the symptom shows up far from the cause.
    LOG_ERROR(Core, "GekkoPAK NTR bus fault {} ({})", s.reported_faults,
              s.transport.last_fault() ==
                      gekkopak::ntr_transport::RegisterTransport::Fault::BlockSizeMismatch
                  ? "ROMCNT block size does not match the opcode's data phase"
                  : "FIFO accessed with no transfer open");
}

void AnnounceCompletion(State& s) {
    if (s.announced_complete || !s.core.completed()) {
        return;
    }
    s.announced_complete = true;
    LOG_INFO(Core, "GekkoPAK NTR guest completed: transfers={}", s.core.transfers());
}

} // namespace

std::shared_ptr<BackingMem> GetRegisterMemory() {
    return GetState().regs;
}

void Reset() {
    auto& s = GetState();

    gekkopak::Device::Config config;
    config.pool = s.pool.data();
    config.pool_bytes = LocalMemoryBytes;
    config.reported_local_bytes = LocalMemoryBytes;
    s.core.Reset(config);
    s.transport.Reset(s.regs->Vector().data());

    s.reported_faults = 0;
    s.traced_commands = 0;
    s.announced_complete = false;
    s.mapped = true;

    LOG_INFO(Core,
             "GekkoPAK NTR virtual cartridge reset: protocol=0x{:08X} caps=0x{:08X} local={} MiB",
             gekkopak::protocol::kProtocolVersion, gekkopak::protocol::kCaps,
             LocalMemoryBytes / (1024u * 1024u));
}

bool IsMapped() {
    return GetState().mapped;
}

u32 Read32(u32 offset) {
    auto& s = GetState();
    if (!s.mapped) {
        return 0;
    }
    const u32 value = s.transport.Read32(s.core, s.regs->Vector().data(), offset);
    ReportFaults(s);
    return value;
}

void Write32(u32 offset, u32 value) {
    auto& s = GetState();
    if (!s.mapped) {
        return;
    }
    s.transport.Write32(s.core, s.regs->Vector().data(), offset, value);
    TraceCommand(s);
    ReportFaults(s);
    AnnounceCompletion(s);
}

} // namespace GekkoPakNtr
