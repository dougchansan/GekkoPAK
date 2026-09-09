#!/usr/bin/env bash
# Build a real DSpico GekkoPAK firmware image.
#
# This is deliberately not the CI compile-proof path: CI embeds a zero-filled
# default.nds purely to prove the overlay compiles. Here the embedded ROM is the
# actual gekkopak_test.nds, so the resulting UF2 is what goes on hardware.
#
#   ./build_firmware.sh <workdir> <gekkopak-repo> <default.nds>
#
# Set GEKKOPAK_HOST_LINK=1 to build the telemetry link on the cartridge's own
# USB port. That image is what makes the hardware loop scriptable -- results
# read back over a COM port, firmware reflashed without the BOOTSEL button --
# but it replaces upstream's DS-side USB proxy, so a DS application that drives
# USB over card commands E8-EB will not work against it. Off by default: a
# timing result should come from the plainest firmware that can produce it.
set -euo pipefail

WORK=${1:?workdir}
GEKKOPAK=${2:?path to GekkoPAK checkout}
ROM=${3:?path to prepared default.nds}

# Pinned, not a branch. Diagnosing a hardware defect against a moving
# upstream is how a fix gets attributed to the wrong change; deps.lock is
# the single source of truth for which revision a result belongs to.
DSPICO_REF=${DSPICO_REF:-472c9d8e9957ad18df367f14b9cc337b9b887e65}
PICO_SDK_REF=${PICO_SDK_REF:-6a7db34ff63345a7badec79ebea3aaef1712f374}

mkdir -p "$WORK"
cd "$WORK"

if [ ! -d dspico-firmware/.git ]; then
    git clone --quiet https://github.com/LNH-team/dspico-firmware.git dspico-firmware
fi
cd dspico-firmware
git fetch --quiet origin "$DSPICO_REF"
git checkout --quiet FETCH_HEAD
# reset --hard as well as clean: a previous run leaves the overlay applied to
# tracked files, and apply_overlay.py anchors on the pristine upstream text.
git reset --quiet --hard FETCH_HEAD
git clean -qfd -e pico-sdk -e roms
echo "dspico   : $(git rev-parse HEAD)"

if [ ! -d pico-sdk/.git ]; then
    git clone --quiet https://github.com/raspberrypi/pico-sdk.git pico-sdk
fi
git -C pico-sdk fetch --quiet origin "$PICO_SDK_REF"
git -C pico-sdk checkout --quiet "$PICO_SDK_REF"
git -C pico-sdk submodule update --quiet --init
echo "pico-sdk : $(git -C pico-sdk rev-parse HEAD)"

OVERLAY_ARGS=()
if [ "${GEKKOPAK_HOST_LINK:-0}" != "0" ]; then
    OVERLAY_ARGS+=(--host-link)
fi
python3 "$GEKKOPAK/prototype/dspico-v1/apply_overlay.py" . "${OVERLAY_ARGS[@]}"
python3 "$GEKKOPAK/prototype/dspico-v1/verify_overlay.py" .
echo "hostlink : ${GEKKOPAK_HOST_LINK:-0}"

mkdir -p roms
cp "$ROM" roms/default.nds
echo "rom      : $(sha256sum roms/default.nds | cut -d' ' -f1)"

export PICO_SDK_PATH="$PWD/pico-sdk"
export CMAKE_POLICY_VERSION_MINIMUM=3.5
# Pin the interpreter to the one this script is already using. The SDK
# searches for "python" before "python3", which on a WSL host can find a
# shim wrapping the Windows interpreter -- it then fails to open a POSIX
# path and the boot stage 2 checksum step dies with a Windows error.
cmake -G Ninja -S . -B build-gekkopak -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPython3_EXECUTABLE="$(command -v python3)" >/dev/null
cmake --build build-gekkopak -j "$(nproc)"

test -f build-gekkopak/DSpico.uf2
echo "--- arm-none-eabi-size ---"
arm-none-eabi-size build-gekkopak/DSpico.elf
echo "--- artifact ---"
cp build-gekkopak/DSpico.uf2 build-gekkopak/DSpico-GekkoPAK.uf2
sha256sum build-gekkopak/DSpico-GekkoPAK.uf2
