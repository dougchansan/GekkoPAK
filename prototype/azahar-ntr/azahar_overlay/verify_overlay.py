#!/usr/bin/env python3
"""Verify that the GekkoPAK NTRCARD overlay landed in an Azahar source tree.

Checks both the Azahar-side adapter and the shared GekkoPAK device core it now
depends on, so a partially applied overlay fails here rather than in a 40-minute
compile.
"""

from pathlib import Path
import sys

root = Path(sys.argv[1])
checks = {
    # Azahar transport adapter.
    "src/core/hle/device/gekkopak_ntr.cpp": [
        "GekkoPAK NTR virtual cartridge reset",
        "transport.Read32",
        "transport.Write32",
        "GKPAK-TRACE",
    ],
    "src/core/hle/device/gekkopak_ntr.h": ["PhysicalBase = 0x10164000u", "u32 Read32(u32 offset)"],
    # Shared protocol/device core, copied in alongside the adapter.
    "src/gekkopak/protocol.h": ["kWireWriteBlock = 0xF4", "kMagic0 = 0x47"],
    "src/gekkopak/device.h": ["class Device", "kCompletionQueueDepth"],
    "src/gekkopak/ntr_register_transport.h": [
        "class RegisterTransport",
        "kCardBlock512",
        "BlockSizeMismatch",
    ],
    "src/core/hle/device/gekkopak_device.cpp": ["Device::Exec", "ProcessDescriptorBlock"],
    # MMIO page type: the part that touches emulator internals.
    "src/core/memory.h": ["MMIO,", "void MapMMIORegion(PageTable&"],
    "src/core/memory.cpp": [
        "case PageType::MMIO:",
        "GekkoPakNtr::Read32(vaddr & CITRA_PAGE_MASK)",
        "GekkoPakNtr::Write32(vaddr & CITRA_PAGE_MASK",
        "void MemorySystem::MapMMIORegion",
    ],
    "src/core/hle/kernel/vm_manager.h": ["MMIO,", "void MakeMMIO(VMAHandle vma)"],
    "src/core/hle/kernel/vm_manager.cpp": [
        "case VMAType::MMIO:",
        "void VMManager::MakeMMIO",
    ],
    # Azahar integration points.
    "src/core/hle/kernel/memory.cpp": [
        "Mapped GekkoPAK NTRCARD window",
        "address_space.MakeMMIO(vma)",
    ],
    "src/core/hle/kernel/process.cpp": ["0x1EC64000, 0x1000"],
    "src/core/CMakeLists.txt": [
        "hle/device/gekkopak_ntr.cpp",
        "hle/device/gekkopak_device.cpp",
    ],
}
for rel, needles in checks.items():
    path = root / rel
    if not path.exists():
        raise SystemExit(f"missing patched file: {rel}")
    text = path.read_text(errors="replace")
    for needle in needles:
        if needle not in text:
            raise SystemExit(f"missing marker in {rel}: {needle}")
print("Azahar GekkoPAK overlay verification PASS")
