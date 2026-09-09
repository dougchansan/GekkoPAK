#!/usr/bin/env python3
"""Apply the GekkoPAK F0-F5 transport shim to an upstream DSpico firmware tree."""

from pathlib import Path
import argparse
import shutil
import sys


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}: {old[:80]!r}")
    path.write_text(text.replace(old, new, 1))


# The block of upstream's CMakeLists.txt that builds the DS-side USB proxy: the
# proxy itself, its event queue, and the flattened TinyUSB device controller
# driver it calls into. All of it goes when the host link comes in, because the
# RP2040 has one USB device controller and the two would both drive it.
UPSTREAM_USB_SOURCES = """  src/ntrCardRomGameUsb.cpp
  src/ntrCardRomGameUsb.h
  src/usbEventQueue.c
  src/usbEventQueue.h
  src/tinyusb/dcd_rp2040.c
  src/tinyusb/rp2040_usb.c
  src/tinyusb/rp2040_usb.h
  src/tinyusb/dcd.h
  src/tinyusb/tusb_option.h
  src/tinyusb/tusb_compiler.h
  src/tinyusb/tusb_common.h
  src/tinyusb/tusb_config.h
  src/tinyusb/tusb_mcu.h
  src/tinyusb/tusb_verify.h
  src/tinyusb/tusb_types.h
  src/tinyusb/tusb_debug.h
  src/tinyusb/osal.h
  src/tinyusb/osal_none.h
"""

# What replaces it. A complete TinyUSB tree rather than upstream's flattened
# fragment, because the device stack and the CDC class are needed as well as the
# controller driver -- and a stack compiled against one tree's headers must not
# be linked against another tree's driver.
HOST_LINK_SOURCES = """  src/gekkopakLink.cpp
  src/gekkopakLink.h
  src/gekkopakUsbStubs.cpp
  src/gekkopakUsb/tusb_config.h
  src/gekkopakUsb/tinyusb/tusb.c
  src/gekkopakUsb/tinyusb/common/tusb_fifo.c
  src/gekkopakUsb/tinyusb/device/usbd.c
  src/gekkopakUsb/tinyusb/device/usbd_control.c
  src/gekkopakUsb/tinyusb/class/cdc/cdc_device.c
  src/gekkopakUsb/tinyusb/portable/raspberrypi/rp2040/dcd_rp2040.c
  src/gekkopakUsb/tinyusb/portable/raspberrypi/rp2040/rp2040_usb.c
"""


def apply_host_link(root: Path, here: Path) -> None:
    """Point the RP2040's USB port at the host instead of at the console."""
    src = root / "src"

    for name in ("gekkopakLink.h", "gekkopakLink.cpp", "gekkopakUsbStubs.cpp"):
        shutil.copy2(here / "overlay/src" / name, src / name)

    usb = src / "gekkopakUsb"
    usb.mkdir(parents=True, exist_ok=True)
    shutil.copy2(here / "overlay/src/tusb_config.h", usb / "tusb_config.h")

    # The same TinyUSB the DS-side test application already vendors, rather than
    # a second copy: one tree in the repository, one version to account for in a
    # hardware result.
    tinyusb = here / "ds-test/usb/tinyusb"
    if not (tinyusb / "device/usbd.c").exists():
        raise RuntimeError(f"no TinyUSB device stack at {tinyusb}")
    if (usb / "tinyusb").exists():
        shutil.rmtree(usb / "tinyusb")
    shutil.copytree(tinyusb, usb / "tinyusb")

    cmake = root / "CMakeLists.txt"
    replace_once(cmake, UPSTREAM_USB_SOURCES, HOST_LINK_SOURCES)

    # TinyUSB resolves its own includes within its own tree; tusb_config.h sits
    # one level above it, which is where tusb_option.h looks for it.
    replace_once(
        cmake,
        "target_link_libraries(DSpico pico_stdlib hardware_pio hardware_dma pico_multicore)",
        "target_include_directories(DSpico PRIVATE\n"
        "  ${CMAKE_CURRENT_LIST_DIR}/src/gekkopakUsb\n"
        "  ${CMAKE_CURRENT_LIST_DIR}/src/gekkopakUsb/tinyusb\n"
        ")\n"
        "target_compile_definitions(DSpico PRIVATE GEKKOPAK_HOST_LINK=1)\n"
        "target_link_libraries(DSpico pico_stdlib hardware_pio hardware_dma pico_multicore\n"
        "  hardware_irq hardware_resets)",
    )

    main_cpp = src / "main.cpp"
    replace_once(
        main_cpp,
        '#include "gekkopakNtr.h"\n',
        '#include "gekkopakNtr.h"\n#include "gekkopakLink.h"\n',
    )
    # After the card transport is up and after the SD has had its chance to
    # request BOOTSEL, so a firmware that cannot mount its card still reaches
    # the bootloader by upstream's route.
    replace_once(
        main_cpp,
        "    tryRebootToBootsel();\n",
        "    tryRebootToBootsel();\n\n    gekkopak_link_init();\n",
    )
    # In the idle loop, next to the SD pump, at thread priority. The cartridge
    # interrupt preempts everything here.
    replace_once(
        main_cpp,
        "        gSdCard.Update();\n        gSdCard.Update();\n",
        "        gSdCard.Update();\n        gSdCard.Update();\n        gekkopak_link_task();\n",
    )
    # __wfi() with deep sleep enabled parks the core until the next cartridge
    # interrupt, which may never come -- the console may be off, or hung, which
    # are exactly the cases a host needs to be able to ask about. The link has
    # to answer whether or not a DS is attached, so the idle loop stops idling.
    replace_once(main_cpp, "        __wfi();\n", "")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("dspico", type=Path, help="path to an upstream dspico-firmware checkout")
    parser.add_argument(
        "--host-link",
        action="store_true",
        help="expose the GekkoPAK telemetry link on the cartridge's own USB port. "
        "Replaces upstream's DS-side USB proxy, which cannot coexist with it: "
        "there is one device controller and they would both drive it.",
    )
    args = parser.parse_args()
    root = args.dspico.resolve()
    here = Path(__file__).resolve().parent

    src = root / "src"
    if not (root / "CMakeLists.txt").exists() or not (src / "ntrCardIrq.S").exists():
        raise RuntimeError(f"not a DSpico source tree: {root}")

    for name in ("gekkopakNtr.h", "gekkopakNtr.cpp",
                 "gekkopakTrace.h", "gekkopakTrace.cpp"):
        shutil.copy2(here / "overlay/src" / name, src / name)

    # The shared GekkoPAK protocol/device core, copied verbatim. This is the
    # same source Azahar and the host model compile; the firmware differs only
    # in its transport adapter and its 64 KiB pool.
    repo = here.parents[1]
    core_dst = src / "gekkopak"
    core_dst.mkdir(parents=True, exist_ok=True)
    for name in ("protocol.h", "device.h"):
        shutil.copy2(repo / "include/gekkopak" / name, core_dst / name)
    shutil.copy2(repo / "src/device.cpp", src / "gekkopakDevice.cpp")

    cmake = root / "CMakeLists.txt"
    replace_once(
        cmake,
        "  src/ntrCardRomGameNoScramble.h\n  src/ntrCardRomGameSd.cpp\n",
        "  src/ntrCardRomGameNoScramble.h\n"
        "  src/gekkopakNtr.cpp\n"
        "  src/gekkopakDevice.cpp\n"
        "  src/gekkopakTrace.cpp\n"
        "  src/gekkopakNtr.h\n"
        "  src/gekkopakTrace.h\n"
        "  src/ntrCardRomGameSd.cpp\n",
    )

    main_cpp = src / "main.cpp"
    replace_once(
        main_cpp,
        '#include "ntrCardRom.h"\n',
        '#include "ntrCardRom.h"\n#include "gekkopakNtr.h"\n',
    )
    replace_once(
        main_cpp,
        "static void resetNtrCard(void)\n{\n    ntrc_resetUsb();\n",
        "static void resetNtrCard(void)\n{\n    ntrc_resetUsb();\n    gekkopak_ntr_reset();\n",
    )

    irq = src / "ntrCardIrq.S"
    text = irq.read_text()

    cmd0_old = """    .word ntrc_gameNoScrambleCmd0Dummy //F0
    .word ntrc_gameNoScrambleCmd0Dummy //F1
    .word ntrc_gameNoScrambleCmd0Dummy //F2
    .word ntrc_gameNoScrambleCmd0Dummy //F3
    .word ntrc_gameNoScrambleCmd0Dummy //F4
    .word ntrc_gameNoScrambleCmd0Dummy //F5
"""
    cmd0_new = """    .word ntrc_gekkopakWriteRegCmd0 //F0 GekkoPAK WRITE_REG
    .word ntrc_gekkopakExecCmd0 //F1 GekkoPAK EXEC
    .word ntrc_gekkopakReadRegCmd0 //F2 GekkoPAK READ_REG/EVENT
    .word ntrc_gekkopakPayloadWordCmd0 //F3 GekkoPAK PAYLOAD_WORD
    .word ntrc_gekkopakWriteBlockCmd0 //F4 GekkoPAK WRITE_BLOCK
    .word ntrc_gekkopakReadBlockCmd0 //F5 GekkoPAK READ_BLOCK
"""
    if text.count(cmd0_old) != 1:
        raise RuntimeError("ntrCardIrq.S: F0-F5 cmd0 table no longer matches expected upstream layout")
    text = text.replace(cmd0_old, cmd0_new, 1)

    cmd1_old = """    .word ntrc_gameNoScrambleCmd1Unknown //F0
    .word ntrc_gameNoScrambleCmd1Unknown //F1
    .word ntrc_gameNoScrambleCmd1Unknown //F2
    .word ntrc_gameNoScrambleCmd1Unknown //F3
    .word ntrc_gameNoScrambleCmd1Unknown //F4
    .word ntrc_gameNoScrambleCmd1Unknown //F5
"""
    cmd1_new = """    .word ntrc_gekkopakWriteRegCmd1 //F0 GekkoPAK WRITE_REG
    .word ntrc_gekkopakExecCmd1 //F1 GekkoPAK EXEC
    .word ntrc_gekkopakReadRegCmd1 //F2 GekkoPAK READ_REG/EVENT
    .word ntrc_gekkopakPayloadWordCmd1 //F3 GekkoPAK PAYLOAD_WORD
    .word ntrc_gekkopakWriteBlockCmd1 //F4 GekkoPAK WRITE_BLOCK
    .word ntrc_gekkopakReadBlockCmd1 //F5 GekkoPAK READ_BLOCK
"""
    if text.count(cmd1_old) != 1:
        raise RuntimeError("ntrCardIrq.S: F0-F5 cmd1 table no longer matches expected upstream layout")
    irq.write_text(text.replace(cmd1_old, cmd1_new, 1))

    if args.host_link:
        apply_host_link(root, here)

    print(f"Applied GekkoPAK F0-F5 DSpico overlay to {root}"
          + (" with the host link" if args.host_link else ""))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"overlay failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
