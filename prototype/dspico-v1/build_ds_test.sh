#!/usr/bin/env bash
# Build gekkopak_test.nds reproducibly, through the devkitPro container.
#
# devkitARM is not installed on the host and should not be: a hand-configured
# toolchain is exactly the sort of thing that makes a hardware result
# unreproducible six months later. The container is pinned by digest below.
#
#   ./build_ds_test.sh [output-dir]
#
# Produces the .nds and its SHA-256. Run from anywhere; paths are resolved
# relative to this script.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/ds-test"
OUT="${1:-$HERE/build/ds-test}"

# devkitpro/devkitarm. The application needs calico (pmMainLoop), which older
# tagged images predate. docs/HARDWARE_DSPICO_V1.md records the exact toolchain
# version and image digest a given result was built with; override the image
# with GEKKOPAK_DEVKITARM_IMAGE to reproduce an older one.
IMAGE="${GEKKOPAK_DEVKITARM_IMAGE:-devkitpro/devkitarm:latest}"

if ! command -v docker >/dev/null 2>&1; then
    echo "docker not found. Install Docker, or set GEKKOPAK_DEVKITARM_IMAGE and" >&2
    echo "provide devkitARM some other way." >&2
    exit 2
fi

mkdir -p "$OUT"

# The Makefile builds in-tree, so the source is mounted read-write and the
# artefacts are copied out afterwards. Running as the invoking user keeps the
# build tree from filling up with root-owned files.
docker run --rm \
    -v "$APP:/work" \
    -w /work \
    -u "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    "$IMAGE" \
    /bin/bash -lc 'make clean >/dev/null 2>&1 || true; make -j"$(nproc)"'

cp -f "$APP/gekkopak_test.nds" "$OUT/gekkopak_test.nds"

echo
echo "artefact: $OUT/gekkopak_test.nds"
sha256sum "$OUT/gekkopak_test.nds"
ls -l "$OUT/gekkopak_test.nds" | awk '{print $5 " bytes"}'
