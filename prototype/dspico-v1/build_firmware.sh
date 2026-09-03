#!/usr/bin/env bash
# Build a real DSpico GekkoPAK firmware image.
#
# This is deliberately not the CI compile-proof path: CI embeds a zero-filled
# default.nds purely to prove the overlay compiles. Here the embedded ROM is the
# actual gekkopak_test.nds, so the resulting UF2 is what goes on hardware.
#
#   ./build_firmware.sh <workdir> <gekkopak-repo> <default.nds>
#
set -euo pipefail

WORK=${1:?workdir}
GEKKOPAK=${2:?path to GekkoPAK checkout}
ROM=${3:?path to prepared default.nds}

DSPICO_REF=${DSPICO_REF:-develop}
PICO_SDK_REF=${PICO_SDK_REF:-6a7db34ff63345a7badec79ebea3aaef1712f374}

mkdir -p "$WORK"
cd "$WORK"

if [ ! -d dspico-firmware/.git ]; then
    git clone --quiet https://github.com/LNH-team/dspico-firmware.git dspico-firmware
fi
cd dspico-firmware
git fetch --quiet origin "$DSPICO_REF"
git checkout --quiet FETCH_HEAD
git clean -qfd
echo "dspico   : $(git rev-parse HEAD)"

if [ ! -d pico-sdk/.git ]; then
    git clone --quiet https://github.com/raspberrypi/pico-sdk.git pico-sdk
fi
git -C pico-sdk fetch --quiet origin "$PICO_SDK_REF"
git -C pico-sdk checkout --quiet "$PICO_SDK_REF"
git -C pico-sdk submodule update --quiet --init
echo "pico-sdk : $(git -C pico-sdk rev-parse HEAD)"

python3 "$GEKKOPAK/prototype/dspico-v1/apply_overlay.py" .
python3 "$GEKKOPAK/prototype/dspico-v1/verify_overlay.py" .

mkdir -p roms
cp "$ROM" roms/default.nds
echo "rom      : $(sha256sum roms/default.nds | cut -d' ' -f1)"

export PICO_SDK_PATH="$PWD/pico-sdk"
export CMAKE_POLICY_VERSION_MINIMUM=3.5
cmake -G Ninja -S . -B build-gekkopak -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build build-gekkopak -j "$(nproc)"

test -f build-gekkopak/DSpico.uf2
echo "--- arm-none-eabi-size ---"
arm-none-eabi-size build-gekkopak/DSpico.elf
echo "--- artifact ---"
cp build-gekkopak/DSpico.uf2 build-gekkopak/DSpico-GekkoPAK.uf2
sha256sum build-gekkopak/DSpico-GekkoPAK.uf2
