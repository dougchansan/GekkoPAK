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
    # MMIO page type: generic, and deliberately free of any GekkoPAK reference.
    "src/core/memory.h": [
        "MMIO,",
        "struct MMIOHandler",
        "bool RegisterMMIOWindow(PAddr phys_base",
        "void MapMMIORegion(PageTable&",
    ],
    "src/core/memory.cpp": [
        "case PageType::MMIO:",
        "u32 MMIORead32(VAddr vaddr)",
        "void MMIOWrite32(VAddr vaddr, u32 value)",
        "void MemorySystem::MapMMIORegion",
    ],
    "src/core/hle/kernel/vm_manager.h": ["MMIO,", "void MakeMMIO(VMAHandle vma)"],
    "src/core/hle/kernel/vm_manager.cpp": [
        "case VMAType::MMIO:",
        "void VMManager::MakeMMIO",
    ],
    # Azahar integration points.
    "src/core/hle/kernel/memory.cpp": [
        "Mapped MMIO window",
        "Memory::FindMMIOWindowByPhys",
        "address_space.MakeMMIO(vma)",
    ],
    # The single binding site.
    "src/core/core.cpp": ["GekkoPakNtr::Install();"],
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
# The memory core and VM manager are generic infrastructure. If a GekkoPAK
# reference ever leaks into them the decoupling has regressed, so check.
for rel in ("src/core/memory.h", "src/core/memory.cpp",
            "src/core/hle/kernel/vm_manager.h", "src/core/hle/kernel/vm_manager.cpp",
            "src/core/hle/kernel/memory.cpp"):
    text = (root / rel).read_text(errors="replace")
    if "GekkoPak" in text or "gekkopak" in text:
        raise SystemExit(f"{rel} names GekkoPAK; the MMIO layer must stay device-agnostic")

print("Azahar GekkoPAK overlay verification PASS")
