#!/usr/bin/env python3
"""Prepare a homebrew .nds so a DSpico can serve it as roms/default.nds.

DSpico is a cartridge emulator, not a loader: the console runs the full NTR boot
sequence against it. That imposes two requirements a devkitPro homebrew ROM does
not meet on its own.

1. KEY1 command encryption.
   src/ntrCardRomNorm.c calls bf_init() with pointers straight into the ROM
   image at 0x1600 (P table) and 0x1C00 (S boxes). A homebrew ROM has zeros
   there, so every secure-mode command decrypts to garbage and the console never
   reaches game mode. The table has to be present in the image.

2. The secure area.
   GBATEK: a secure area exists only when arm9_rom_offset is in 0x4000..0x7FFF.
   devkitPro emits 0x4000, so the console tries to read an encrypted secure area
   that homebrew does not have. Relocating the ARM9 binary to 0x8000 removes the
   secure area from the picture entirely, which is how homebrew boots from real
   cartridge hardware.

The KEY1 table is Nintendo copyrighted data. It is NEVER stored in this
repository: pass --keytable pointing at your own dumped ARM7 BIOS (the table is
0x1048 bytes at offset 0x30) or at a raw 0x1048-byte table you already hold.

Usage:
  python tools/dspico_rom_tool.py \
      --rom  prototype/dspico-v1/ds-test/gekkopak_test.nds \
      --keytable /path/to/bios7.bin \
      --out  dspico-firmware/roms/default.nds

Run `ndstool -f <out>` afterwards to refresh the header CRC.
"""

import argparse
import hashlib
import struct
import sys

KEYTABLE_LEN = 0x1048          # 18-word P table + 4 x 256-word S boxes
P_TABLE_LEN = 18 * 4           # 0x48
S_BOX_LEN = 4 * 256 * 4        # 0x1000
ROM_P_TABLE_OFFSET = 0x1600
ROM_S_BOX_OFFSET = 0x1C00
BIOS7_KEYTABLE_OFFSET = 0x30
SECURE_AREA_END = 0x8000

# Header fields holding an absolute ROM offset, which must move with the payload.
OFFSET_FIELDS = (
    (0x20, "arm9_rom_offset"),
    (0x30, "arm7_rom_offset"),
    (0x40, "fnt_offset"),
    (0x48, "fat_offset"),
    (0x50, "arm9_overlay_offset"),
    (0x58, "arm7_overlay_offset"),
    (0x68, "banner_offset"),
)


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def put_u32(buf, off, value):
    struct.pack_into("<I", buf, off, value)


def load_keytable(path):
    data = open(path, "rb").read()
    if len(data) == KEYTABLE_LEN:
        table = data
        source = "raw keytable"
    elif len(data) >= BIOS7_KEYTABLE_OFFSET + KEYTABLE_LEN:
        table = data[BIOS7_KEYTABLE_OFFSET:BIOS7_KEYTABLE_OFFSET + KEYTABLE_LEN]
        source = f"ARM7 BIOS +0x{BIOS7_KEYTABLE_OFFSET:X}"
    else:
        raise SystemExit(f"{path}: too small to contain a KEY1 keytable")

    # A key table is high-entropy; ARM code is not. This catches the common
    # mistake of pointing at the wrong file without ever printing key material.
    distinct = len(set(table))
    if distinct < 200:
        raise SystemExit(
            f"{path}: data at the keytable offset does not look like key material "
            f"({distinct} distinct byte values); wrong file?"
        )
    print(f"keytable   : {source}, sha1 {hashlib.sha1(table).hexdigest()}")
    return table


def relocate_past_secure_area(rom):
    """Move the payload so arm9_rom_offset lands at 0x8000."""
    arm9_off = u32(rom, 0x20)
    if arm9_off >= SECURE_AREA_END:
        print(f"relocation : not needed (arm9_rom_offset 0x{arm9_off:X})")
        return rom, 0

    # Overlay tables carry their own absolute offsets; refuse rather than
    # silently emit a ROM with stale pointers. 0x54/0x5C are the table sizes.
    if u32(rom, 0x54) != 0 or u32(rom, 0x5C) != 0:
        raise SystemExit(
            "ROM contains overlays; offset fixups for overlay tables are not "
            "implemented. Rebuild without overlays."
        )

    shift = SECURE_AREA_END - arm9_off
    head, tail = rom[:arm9_off], rom[arm9_off:]
    rom = bytearray(head + b"\x00" * shift + tail)

    for off, name in OFFSET_FIELDS:
        value = u32(rom, off)
        if value >= arm9_off:
            put_u32(rom, off, value + shift)

    total = u32(rom, 0x80)
    if total:
        put_u32(rom, 0x80, total + shift)
    # Header size field, so tools agree on where the payload starts.
    put_u32(rom, 0x84, SECURE_AREA_END)

    print(f"relocation : arm9 0x{arm9_off:X} -> 0x{SECURE_AREA_END:X} (+0x{shift:X})")
    return rom, shift


def inject_keytable(rom, table):
    need = ROM_S_BOX_OFFSET + S_BOX_LEN
    if len(rom) < need:
        rom.extend(b"\x00" * (need - len(rom)))
    rom[ROM_P_TABLE_OFFSET:ROM_P_TABLE_OFFSET + P_TABLE_LEN] = table[:P_TABLE_LEN]
    rom[ROM_S_BOX_OFFSET:ROM_S_BOX_OFFSET + S_BOX_LEN] = table[P_TABLE_LEN:P_TABLE_LEN + S_BOX_LEN]
    print(f"keys       : P table @0x{ROM_P_TABLE_OFFSET:X}, S boxes @0x{ROM_S_BOX_OFFSET:X}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", required=True, help="input homebrew .nds")
    ap.add_argument("--keytable", required=True,
                    help="ARM7 BIOS dump or raw 0x1048-byte KEY1 table (never committed)")
    ap.add_argument("--out", required=True, help="output ROM for dspico-firmware/roms/")
    ap.add_argument("--no-relocate", action="store_true",
                    help="keep arm9_rom_offset as-is (use when chainloading instead of booting)")
    args = ap.parse_args()

    rom = bytearray(open(args.rom, "rb").read())
    if len(rom) < 0x200:
        raise SystemExit(f"{args.rom}: not a .nds image")
    print(f"input      : {args.rom} ({len(rom)} bytes)")
    print(f"title/code : {rom[0:12].decode(errors='replace')} / {rom[12:16].decode(errors='replace')}")

    table = load_keytable(args.keytable)
    if not args.no_relocate:
        rom, _ = relocate_past_secure_area(rom)
    inject_keytable(rom, table)

    # Pad to a 512-byte boundary; DSpico rounds romSize up to a page anyway.
    if len(rom) % 512:
        rom.extend(b"\x00" * (512 - len(rom) % 512))

    with open(args.out, "wb") as f:
        f.write(rom)
    print(f"output     : {args.out} ({len(rom)} bytes)")
    print(f"sha256     : {hashlib.sha256(bytes(rom)).hexdigest()}")
    print("next       : ndstool -f " + args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
