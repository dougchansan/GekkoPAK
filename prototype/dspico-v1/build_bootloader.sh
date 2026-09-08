#!/usr/bin/env bash
# Build the official DSpico Bootloader as roms/default.nds.
#
# This is the pipeline the DSpico Bootloader README documents, and it is the
# supported way to get a bootable cartridge. Preparing an arbitrary homebrew
# .nds as the boot ROM by hand does not work: see docs/HARDWARE_DSPICO_V1.md.
#
# With this as default.nds the cartridge boots Pico Loader, which chainloads
# fat:/_picoboot.nds from the SD card. gekkopak_test.nds then runs as ordinary
# homebrew from /roms, with a working DSpico DLDI driver for SD access - so no
# cartridge-ROM surgery is needed to iterate on the test application at all.
#
#   ./build_bootloader.sh <workdir> <bios7.bin> <bios7i.bin>
#
# The BIOS dumps are yours and are only ever read. Nothing derived from them is
# written into this repository.
set -euo pipefail

WORK=${1:?workdir}
BIOS7=${2:?path to NTR arm7 bios dump}
BIOS7I=${3:?path to TWL arm7i bios dump}

BLOCKSDS_IMAGE=${BLOCKSDS_IMAGE:-skylyrac/blocksds:slim-latest}
DOTNET_IMAGE=${DOTNET_IMAGE:-mcr.microsoft.com/dotnet/sdk:9.0}
DLDITOOL=/opt/wonderful/thirdparty/blocksds/core/tools/dlditool/dlditool

mkdir -p "$WORK"
cd "$WORK"

[ -d dspico-bootloader ] || git clone --quiet --recurse-submodules https://github.com/LNH-team/dspico-bootloader.git
[ -d dspico-dldi ]       || git clone --quiet https://github.com/LNH-team/dspico-dldi.git
[ -d DSRomEncryptor ]    || git clone --quiet https://github.com/Gericom/DSRomEncryptor.git

docker run --rm -v "$PWD:/w" -w /w/dspico-dldi "$BLOCKSDS_IMAGE" bash -lc 'make'
docker run --rm -v "$PWD:/w" -w /w/dspico-bootloader "$BLOCKSDS_IMAGE" bash -lc 'make'

cp dspico-bootloader/BOOTLOADER.nds boot_patched.nds
docker run --rm -v "$PWD:/w" -w /w "$BLOCKSDS_IMAGE" \
    "$DLDITOOL" dspico-dldi/DSpico.dldi /w/boot_patched.nds

docker run --rm -v "$PWD:/w" -w /w/DSRomEncryptor "$DOTNET_IMAGE" \
    bash -c 'dotnet publish DSRomEncryptor/DSRomEncryptor.csproj -c Release -o /w/dsre-out'

# DSRomEncryptor reads its key files from the executable directory.
cp "$BIOS7" dsre-out/biosnds7.rom
python3 - "$BIOS7I" <<'PY'
import hashlib, sys
# The TWL table is not at a fixed offset across dump variants, so locate it by
# hash rather than assuming one. Never printed, only written to a local file.
TARGET = "2dea11191f28c6cc1956dadb8941affd4b2b5102"
data = open(sys.argv[1], "rb").read()
for off in range(0, len(data) - 0x1048 + 1):
    if hashlib.sha1(data[off:off + 0x1048]).hexdigest() == TARGET:
        open("dsre-out/twlBlowfish.bin", "wb").write(data[off:off + 0x1048])
        print(f"twl blowfish table located at 0x{off:X}")
        break
else:
    raise SystemExit("TWL blowfish table not found in the supplied arm7i dump")
PY

# BlocksDS' ndstool fills in the secure-area CRC, and DSRomEncryptor only
# encrypts when that CRC disagrees with what it computes. Clear it so the
# encryption step actually runs.
python3 - <<'PY'
import struct
rom = bytearray(open("boot_patched.nds", "rb").read())
struct.pack_into("<H", rom, 0x6C, 0)
open("boot_prepped.nds", "wb").write(bytes(rom))
PY

docker run --rm -v "$PWD:/w" -w /w/dsre-out "$DOTNET_IMAGE" \
    bash -c 'dotnet DSRomEncryptor.dll /w/boot_prepped.nds /w/default.nds'

python3 - <<'PY'
import struct
def crc16(b):
    c = 0xFFFF
    for x in b:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ (0xA001 if c & 1 else 0)
    return c & 0xFFFF
rom = open("default.nds", "rb").read()
arm9 = struct.unpack_from("<I", rom, 0x20)[0]
twl = struct.unpack_from("<H", rom, 0x92)[0] * 0x80000
assert any(rom[0x1600:0x1648]),                          "NTR key block missing"
assert any(rom[twl + 0x600:twl + 0x648]),                "TWL key block missing"
assert struct.unpack_from("<H", rom, 0x6C)[0] == crc16(rom[arm9:0x8000]), "secure CRC bad"
assert struct.unpack_from("<H", rom, 0x15E)[0] == crc16(rom[:0x15E]),     "header CRC bad"
print(f"default.nds OK: {len(rom)} bytes, twl area 0x{twl:X}")
PY

echo "default.nds ready in $WORK"
