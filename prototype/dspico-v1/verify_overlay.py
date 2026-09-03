#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
checks = {
    "CMakeLists.txt": ["src/gekkopakNtr.cpp", "src/gekkopakNtr.h"],
    "src/main.cpp": ["#include \"gekkopakNtr.h\"", "gekkopak_ntr_reset();"],
    "src/ntrCardIrq.S": [
        "ntrc_gekkopakWriteRegCmd0 //F0",
        "ntrc_gekkopakExecCmd1 //F1",
        "ntrc_gekkopakReadRegCmd1 //F2",
        "ntrc_gekkopakPayloadWordCmd1 //F3",
        "ntrc_gekkopakWriteBlockCmd1 //F4",
        "ntrc_gekkopakReadBlockCmd1 //F5",
    ],
    "src/gekkopakNtr.h": ["ntrc_gekkopakWriteBlockCmd1", "gekkopak_ntr_reset"],
    "src/gekkopakNtr.cpp": [
        "kDescriptorMagic = 0x31444B47u",
        "kCompletionMagic = 0x31434B47u",
        "ntrc_beginRead(pio, kBlockBytes)",
        "ntrc_dmaToBus(sBlockRx.data(), kBlockBytes)",
        "kEventCompletionDepth = 0xFE",
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

print("DSpico GekkoPAK F0-F5 overlay verification PASS")
