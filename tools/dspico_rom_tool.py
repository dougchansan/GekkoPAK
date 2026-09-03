#!/usr/bin/env python3
"""Prepare a homebrew .nds so a DSpico can serve it as roms/default.nds.

DSpico is a cartridge emulator, not a loader: the console runs its full NTR boot
sequence against it, so the ROM must look like a real cartridge.

THIS SCRIPT IS NOT SUFFICIENT ON ITS OWN. A bootable cartridge ROM must be run
through DSRomEncryptor (https://github.com/Gericom/DSRomEncryptor), which is the
step the DSpico Bootloader README documents. That tool does two things this
script deliberately does not attempt:

  * it inserts the NTR key blocks *transformed by the ROM game code*
    (KeyTransform.TransformTable(gameCode, 2, 8, ntrBlowfish)), not the raw
    ARM7 BIOS table; and
  * it encrypts the secure area at 0x4000-0x4800 and writes the encrypted
    "encryObj" marker, which is what the console validates during boot.

Raw key injection without those was tried on hardware and the 2DS XL never
showed the cartridge at all.

The intended pipeline is therefore:

  python tools/dspico_rom_tool.py --rom gekkopak_test.nds --no-keys --out prepped.nds
  DSRomEncryptor prepped.nds default.nds

What this script contributes are the fixups DSRomEncryptor does not do, because
they are artefacts of building with devkitPro rather than BlocksDS:

1. NTR-only unit code (header 0x12).
   DSpico takes its advertised card ID straight from this byte
   (src/main.cpp: `if (gDefaultRom[0x12] & 2) cardId = CARD_ID_TWL`). devkitPro
   emits 0x02, so the cartridge claims to be TWL-capable and the console runs
   the TWL secure handshake against a ROM with no TWL area. Clearing it also
   means DSRomEncryptor needs no TWL blowfish key.

2. Secure-area CRC (header 0x6C) cleared under --no-keys.
   DSRomEncryptor only encrypts when 0x6C disagrees with what it computes.
   BlocksDS leaves 0x6C unset so the check always fires; ndstool fills it in,
   which makes a devkitPro ROM look already-prepared and silently skips
   encryption.

3. Device capacity (0x14) and header CRC (0x15E).
   Do NOT rely on `ndstool -f` for the CRC: on ndstool 2.3.1 it exits 0 without
   touching it.

What NOT to do: relocating the ARM9 binary past the secure area to 0x8000.
GBATEK says a secure area exists only for arm9_rom_offset 0x4000..0x7FFF, which
reads like an argument for relocating. A relocated image was rejected outright
by a 2DS XL. --relocate is retained for experiments only.

Blowfish tables are Nintendo copyrighted data and are NEVER stored in this
repository. --keytable exists for the legacy raw-injection path; prefer
--no-keys and let DSRomEncryptor handle keys from your own BIOS dump.
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


def crc16(data, init=0xFFFF):
    """Nitro header CRC: reflected CRC-16 with polynomial 0xA001."""
    crc = init
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc & 0xFFFF


def fix_header_crc(rom):
    """Recompute the header CRC at 0x15E over bytes 0x000..0x15D.

    Do not delegate this to `ndstool -f`: on ndstool 2.3.1 it exits 0 without
    touching the CRC, so a ROM edited anywhere in the first 0x15E bytes ships
    with a stale checksum and the console rejects the cartridge outright.
    """
    stored = struct.unpack_from("<H", rom, 0x15E)[0]
    calc = crc16(bytes(rom[0x000:0x15E]))
    if stored != calc:
        struct.pack_into("<H", rom, 0x15E, calc)
        print(f"header CRC : 0x{stored:04X} -> 0x{calc:04X}")
    else:
        print(f"header CRC : 0x{calc:04X} (already correct)")


def clear_secure_area_crc(rom):
    """Zero the secure-area CRC at 0x6C so DSRomEncryptor will act.

    DSRomEncryptor encrypts the secure area only when the stored CRC disagrees
    with the CRC it computes over [arm9_rom_offset, 0x8000):

        if (romDataSpan.ReadU16Le(0x6C) != secureCrc) { ...encrypt...; store }

    BlocksDS, which the DSpico bootloader is built with, leaves 0x6C unset, so
    the check always fires. ndstool computes and stores it, so a devkitPro ROM
    looks already-prepared and encryption is silently skipped - producing a ROM
    with valid key blocks but a plaintext secure area, which the console
    rejects. Clearing it restores the intended behaviour; DSRomEncryptor writes
    the correct value back after encrypting.
    """
    previous = struct.unpack_from("<H", rom, 0x6C)[0]
    if previous == 0:
        print("secure CRC : already 0x0000")
        return
    struct.pack_into("<H", rom, 0x6C, 0)
    print(f"secure CRC : 0x{previous:04X} -> 0x0000 (so DSRomEncryptor encrypts)")


def force_ntr_unit_code(rom):
    """Present the image as an NTR-only cartridge.

    DSpico picks its advertised card ID straight from this byte
    (src/main.cpp: `if (gDefaultRom[0x12] & 2) cardId = CARD_ID_TWL`). devkitPro
    emits 0x02 (NTR+TWL hybrid) by default, which makes the console run the TWL
    secure handshake: DSpico then derives blowfish from
    `twlArea = u16@0x92 * 0x80000`, and homebrew leaves 0x92 as zero, so it
    keys off zeros and the handshake fails before the cartridge is ever shown.

    Clearing the byte forces the NTR path, which is the one the injected NTR
    key table actually serves.
    """
    previous = rom[0x12]
    if previous == 0x00:
        print("unit code  : already 0x00 (NTR only)")
        return
    rom[0x12] = 0x00
    print(f"unit code  : 0x{previous:02X} -> 0x00 (NTR only; was advertising a TWL card ID)")


def fix_device_capacity(rom):
    """Make the declared chip size cover the image."""
    size = len(rom)
    capacity = 0
    while (128 * 1024) << capacity < size:
        capacity += 1
    if rom[0x14] != capacity:
        print(f"capacity   : 0x{rom[0x14]:02X} -> 0x{capacity:02X} "
              f"({(128 << capacity)} KiB for a {size} byte image)")
        rom[0x14] = capacity


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
    ap.add_argument("--keytable",
                    help="ARM7 BIOS dump or raw 0x1048-byte KEY1 table (never committed). "
                         "Omit with --no-keys when DSRomEncryptor will insert them.")
    ap.add_argument("--no-keys", action="store_true",
                    help="skip key injection. Use this when the output is going through "
                         "DSRomEncryptor, which inserts the key blocks AND encrypts the "
                         "secure area - the part this script cannot do.")
    ap.add_argument("--out", required=True, help="output ROM for dspico-firmware/roms/")
    ap.add_argument("--relocate", action="store_true",
                    help="move ARM9 past the secure area to 0x8000. Rejected by a "
                         "2DS XL in testing; kept only for experiments.")
    ap.add_argument("--keep-unit-code", action="store_true",
                    help="leave header byte 0x12 alone instead of forcing NTR-only")
    args = ap.parse_args()

    rom = bytearray(open(args.rom, "rb").read())
    if len(rom) < 0x200:
        raise SystemExit(f"{args.rom}: not a .nds image")
    print(f"input      : {args.rom} ({len(rom)} bytes)")
    print(f"title/code : {rom[0:12].decode(errors='replace')} / {rom[12:16].decode(errors='replace')}")

    if args.no_keys:
        table = None
        print("keys       : skipped (--no-keys; DSRomEncryptor will insert them)")
        clear_secure_area_crc(rom)
    elif args.keytable:
        table = load_keytable(args.keytable)
    else:
        raise SystemExit("need --keytable, or --no-keys if DSRomEncryptor will insert them")
    if args.relocate:
        rom, _ = relocate_past_secure_area(rom)
    if not args.keep_unit_code:
        force_ntr_unit_code(rom)
    if table is not None:
        inject_keytable(rom, table)
    fix_device_capacity(rom)
    fix_header_crc(rom)

    # Pad to a 512-byte boundary; DSpico rounds romSize up to a page anyway.
    if len(rom) % 512:
        rom.extend(b"\x00" * (512 - len(rom) % 512))

    with open(args.out, "wb") as f:
        f.write(rom)
    print(f"output     : {args.out} ({len(rom)} bytes)")
    print(f"sha256     : {hashlib.sha256(bytes(rom)).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
