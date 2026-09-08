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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("dspico", type=Path, help="path to an upstream dspico-firmware checkout")
    args = parser.parse_args()
    root = args.dspico.resolve()
    here = Path(__file__).resolve().parent

    src = root / "src"
    if not (root / "CMakeLists.txt").exists() or not (src / "ntrCardIrq.S").exists():
        raise RuntimeError(f"not a DSpico source tree: {root}")

    for name in ("gekkopakNtr.h", "gekkopakNtr.cpp"):
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
        "  src/gekkopakNtr.h\n"
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

    print(f"Applied GekkoPAK F0-F5 DSpico overlay to {root}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"overlay failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
