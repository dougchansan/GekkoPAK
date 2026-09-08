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
        "transport.Tick",
        "GKPAK-TRACE",
    ],
    "src/core/hle/device/gekkopak_ntr.h": ["PhysicalBase = 0x10164000u"],
    # Shared protocol/device core, copied in alongside the adapter.
    "src/gekkopak/protocol.h": ["kWireWriteBlock = 0xF4", "kMagic0 = 0x47"],
    "src/gekkopak/device.h": ["class Device", "kCompletionQueueDepth"],
    "src/gekkopak/ntr_register_transport.h": [
        "class RegisterTransport",
        "kCardBlock512",
        "BlockSizeMismatch",
    ],
    "src/core/hle/device/gekkopak_device.cpp": ["Device::Exec", "ProcessDescriptorBlock"],
    # Azahar integration points.
    "src/core/hle/kernel/memory.cpp": ["Mapped GekkoPAK NTRCARD window"],
    "src/core/hle/kernel/process.cpp": ["0x1EC64000, 0x1000"],
    "src/core/core.cpp": ["GekkoPakNtr::Tick();"],
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
