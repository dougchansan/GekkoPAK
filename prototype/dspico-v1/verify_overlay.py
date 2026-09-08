#!/usr/bin/env python3
"""Verify that the GekkoPAK F0-F5 overlay landed in a DSpico source tree.

Covers the transport adapter, the dispatch-table edits, and the shared GekkoPAK
device core the adapter now delegates to, so a partial application fails here
rather than in the RP2040 link step.
"""

from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
checks = {
    "CMakeLists.txt": [
        "src/gekkopakNtr.cpp",
        "src/gekkopakNtr.h",
        "src/gekkopakDevice.cpp",
    ],
    "src/main.cpp": ['#include "gekkopakNtr.h"', "gekkopak_ntr_reset();"],
    "src/ntrCardIrq.S": [
        "ntrc_gekkopakWriteRegCmd0 //F0",
        "ntrc_gekkopakExecCmd1 //F1",
        "ntrc_gekkopakReadRegCmd1 //F2",
        "ntrc_gekkopakPayloadWordCmd1 //F3",
        "ntrc_gekkopakWriteBlockCmd1 //F4",
        "ntrc_gekkopakReadBlockCmd1 //F5",
    ],
    "src/gekkopakNtr.h": ["ntrc_gekkopakWriteBlockCmd1", "gekkopak_ntr_reset"],
    # The adapter itself: bus primitives stay here, protocol does not.
    "src/gekkopakNtr.cpp": [
        "ntrc_beginRead(pio, kBlockBytes)",
        "ntrc_dmaToBus(source, kBlockBytes)",
        "sDevice.Exec",
        "sDevice.WriteBlock",
        "sDevice.PeekBlock",
        # The two rules the F5 hardware defect came down to: handlers out of XIP
        # flash, and the cartridge-to-console transfer armed in cmd0 from a
        # buffer staged in advance. Losing either silently reintroduces it.
        "#define GEKKOPAK_IRQ_FN(name) __not_in_flash_func(name)",
        "GEKKOPAK_IRQ_FN(ntrc_gekkopakReadBlockCmd0)",
    ],
    # Shared protocol/device core, copied in alongside the adapter.
    "src/gekkopak/protocol.h": [
        "kDescriptorMagic = 0x31444B47u",
        "kCompletionMagic = 0x31434B47u",
        "kEventCompletionDepth = 0xFE",
        "kWireReadBlock = 0xF5",
    ],
    "src/gekkopak/device.h": ["class Device", "kCompletionQueueDepth"],
    "src/gekkopakDevice.cpp": ["Device::Exec", "ProcessDescriptorBlock"],
}

for rel, needles in checks.items():
    path = root / rel
    if not path.exists():
        raise SystemExit(f"missing patched file: {rel}")
    text = path.read_text(errors="replace")
    for needle in needles:
        if needle not in text:
            raise SystemExit(f"missing marker in {rel}: {needle}")

# The 512-byte cartridge-to-console transfer must be armed in the cmd0 handler,
# before anything else runs. Arming it in cmd1 is what lost the opening words of
# the data phase on real hardware.
overlay = (root / "src/gekkopakNtr.cpp").read_text(errors="replace")
cmd0 = overlay.split("GEKKOPAK_IRQ_FN(ntrc_gekkopakReadBlockCmd0)", 1)[1].split("}", 1)[0]
if "ntrc_beginWrite(pio, kBlockBytes)" not in cmd0:
    raise SystemExit("F5 must arm its data phase in cmd0, not cmd1")

# Every handler runs from RAM, and specifically not from SCRATCH_Y: core0's
# stack shares that bank and no linker assert catches the overlap, so filling it
# links cleanly and then corrupts the handlers at runtime. That is not
# hypothetical -- it killed every GekkoPAK handler on hardware while DSpico's
# own B8 handler, which happened to land lower, kept answering.
# Check code, not prose: the file explains this hazard at length in comments.
if any("__scratch_y(" in l for l in overlay.splitlines()
       if not l.lstrip().startswith("//")):
    raise SystemExit("handlers must not be in SCRATCH_Y; core0's stack shares it")
for name in ("ntrc_gekkopakReadBlockCmd0", "ntrc_gekkopakWriteBlockCmd1",
             "ntrc_gekkopakReadRegCmd1"):
    if f"void GEKKOPAK_IRQ_FN({name})(" not in overlay:
        raise SystemExit(f"{name} is on the IRQ path and must not run from flash")

print("DSpico GekkoPAK F0-F5 overlay verification PASS")
