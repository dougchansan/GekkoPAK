#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$HERE/build/guest}"
mkdir -p "$BUILD"
clang --target=arm-none-eabi -mcpu=mpcore -marm -c \
  "$HERE/guest/gekkopak_guest_ntr.S" -o "$BUILD/gekkopak_guest_ntr.o"
ld.lld -m armelf -Ttext=0x00100000 -e _start \
  "$BUILD/gekkopak_guest_ntr.o" -o "$BUILD/gekkopak_guest_ntr.elf"
echo "$BUILD/gekkopak_guest_ntr.elf"
