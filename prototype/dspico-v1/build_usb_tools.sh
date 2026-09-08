#!/usr/bin/env bash
# Build the DSpico USB mass-storage app, DLDI-patched for this cartridge.
#
# With this on the SD card, launching it from Pico Loader exposes the DSpico's
# microSD to the host over USB. That removes the physical card swap from every
# iteration: a new test build can be copied and results read back while the card
# stays in the cartridge.
#
# It needs a PRE-CALICO devkitARM. libtwl, which the USB platform is built on,
# defines REG_IME/REG_IE/REG_IF and calls setVectorBase(), all of which collide
# with calico in current libnds - the build fails on rtosIrq.c. 20230622 is the
# newest tag verified to work.
#
#   ./build_usb_tools.sh <workdir>
set -euo pipefail

WORK=${1:?workdir}
DKA_IMAGE=${DKA_IMAGE:-devkitpro/devkitarm:20230622}
BLOCKSDS_IMAGE=${BLOCKSDS_IMAGE:-skylyrac/blocksds:slim-latest}
DLDITOOL=/opt/wonderful/thirdparty/blocksds/core/tools/dlditool/dlditool

mkdir -p "$WORK"
cd "$WORK"

[ -d dspico-usb-examples ] || git clone --quiet --recurse-submodules \
    https://github.com/LNH-team/dspico-usb-examples.git dspico-usb-examples
[ -d dspico-dldi ] || git clone --quiet https://github.com/LNH-team/dspico-dldi.git

docker run --rm -v "$PWD:/w" -w /w/dspico-dldi "$BLOCKSDS_IMAGE" bash -lc 'make'
docker run --rm -v "$PWD:/w" -w /w/dspico-usb-examples/examples/mass-storage \
    "$DKA_IMAGE" bash -lc 'make'

cp dspico-usb-examples/examples/mass-storage/mass-storage.nds usb-massstorage.nds
docker run --rm -v "$PWD:/w" -w /w "$BLOCKSDS_IMAGE" \
    "$DLDITOOL" dspico-dldi/DSpico.dldi /w/usb-massstorage.nds

echo "usb-massstorage.nds ready in $WORK - copy it to /roms on the DSpico SD"
