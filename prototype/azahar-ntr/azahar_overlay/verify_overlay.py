#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1])
checks = {
    "src/core/hle/device/gekkopak_ntr.cpp": ["GekkoPAK NTR virtual cartridge reset", "WireExec"],
    "src/core/hle/device/gekkopak_ntr.h": ["PhysicalBase = 0x10164000u"],
    "src/core/hle/kernel/memory.cpp": ["Mapped GekkoPAK NTRCARD window"],
    "src/core/hle/kernel/process.cpp": ["0x1EC64000, 0x1000"],
    "src/core/core.cpp": ["GekkoPakNtr::Tick();"],
    "src/core/CMakeLists.txt": ["hle/device/gekkopak_ntr.cpp"],
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
