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
        "GEKKOPAK_IRQ_FN(ntrc_gekkopakReadBlockCmd0)",
    ],
    # The RAM-placement rule itself, now in the header every file on the IRQ
    # path includes, so it cannot be applied to the handlers and quietly
    # forgotten for the instrumentation that runs inside them.
    "src/gekkopakTrace.h": [
        "#define GEKKOPAK_IRQ_FN(name) __not_in_flash_func(name)",
        "GPK_TRACE_CAPACITY",
    ],
    "src/gekkopakTrace.cpp": ["gpk_trace_pop", "++sDropped"],
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

# --------------------------------------------------------------------------
# Host link, when the overlay was applied with --host-link.
# --------------------------------------------------------------------------
#
# Presence is optional -- a plain firmware is the right thing to take a timing
# measurement with -- but a build that has it must have all of it, because the
# failure mode of a partial application is a cartridge that enumerates and then
# fights the console for the USB controller.

if (root / "src/gekkopakLink.cpp").exists():
    link_checks = {
        "CMakeLists.txt": [
            "src/gekkopakLink.cpp",
            "src/gekkopakUsbStubs.cpp",
            "src/gekkopakUsb/tinyusb/device/usbd.c",
            "src/gekkopakUsb/tinyusb/class/cdc/cdc_device.c",
            "src/gekkopakUsb/tinyusb/portable/raspberrypi/rp2040/dcd_rp2040.c",
            "GEKKOPAK_HOST_LINK=1",
        ],
        "src/main.cpp": ["gekkopak_link_init();", "gekkopak_link_task();"],
        # The controller has no clock until power saving is lifted, and the
        # cartridge interrupt must outrank USB: 0x40 against 0x80.
        "src/gekkopakLink.cpp": [
            "pwr_disableUsbPowerSaving();",
            "irq_set_priority(USBCTRL_IRQ, 0x80);",
            "reset_usb_boot(0, 0);",
        ],
        # Upstream tears USB down on every console reset. The link is not the
        # console's and has to survive one, or it can never observe a cold start.
        "src/gekkopakUsbStubs.cpp": ["void ntrc_resetUsb(void) {}"],
    }
    for rel, needles in link_checks.items():
        path = root / rel
        if not path.exists():
            raise SystemExit(f"host link: missing patched file: {rel}")
        text = path.read_text(errors="replace")
        for needle in needles:
            if needle not in text:
                raise SystemExit(f"host link: missing marker in {rel}: {needle}")

    # One device controller means one driver. Upstream's proxy and its
    # flattened TinyUSB fragment must be gone from the build, or the image
    # carries two copies of dcd_init() -- or worse, links one tree's device
    # stack against another tree's driver.
    cmake_text = (root / "CMakeLists.txt").read_text(errors="replace")
    for gone in ("src/ntrCardRomGameUsb.cpp", "src/usbEventQueue.c",
                 "src/tinyusb/dcd_rp2040.c"):
        if gone in cmake_text:
            raise SystemExit(
                f"host link: {gone} is still built; it cannot coexist with the link")

    main_text = (root / "src/main.cpp").read_text(errors="replace")

    # The idle loop must not sleep: a host has to be answered whether or not a
    # console is attached, and __wfi() parks core0 until the next cartridge
    # interrupt, which may never arrive.
    if "__wfi();" in main_text:
        raise SystemExit("host link: main loop still sleeps; the link would stall")

    # Ordering, which is not a style question. pwr_initPowerSaving() stops
    # clk_usb and deinitialises pll_usb; gekkopak_link_init() puts them back.
    # The wrong way round enumerates a controller and then removes its clock,
    # and the symptom -- no serial port at all -- looks exactly like a firmware
    # built without the link. That cost a flash to find once.
    saving = main_text.find("pwr_initPowerSaving();")
    link = main_text.find("gekkopak_link_init();")
    if saving < 0 or link < 0:
        raise SystemExit("host link: power-saving init or link init missing from main")
    if link < saving:
        raise SystemExit(
            "host link: gekkopak_link_init() runs before pwr_initPowerSaving(), "
            "which would stop the USB clock it just started")

    print("DSpico GekkoPAK host link verification PASS")

print("DSpico GekkoPAK F0-F5 overlay verification PASS")
