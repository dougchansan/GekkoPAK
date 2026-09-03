#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${GEKKOPAK_E2E_BUILD:-$HERE/build}"
CORE="${AZAHAR_CORE:-$HERE/azahar_libretro.so}"
FRONTEND="$BUILD/gekkopak_retro_frontend"
PAYLOAD_OBJ="$BUILD/gekkopak_guest.o"
PAYLOAD="$BUILD/gekkopak_guest.elf"
TRACE="$BUILD/gekkopak_guest_trace.jsonl"
LOG="$BUILD/gekkopak_e2e.log"

[[ -f "$CORE" ]] || {
  echo "Azahar libretro core not found: $CORE" >&2
  echo "Set AZAHAR_CORE=/path/to/azahar_libretro.so" >&2
  exit 2
}
command -v gcc >/dev/null
command -v clang >/dev/null
command -v ld.lld >/dev/null
mkdir -p "$BUILD/system" "$BUILD/saves" "$BUILD/assets"

gcc -O2 -Wall -Wextra -o "$FRONTEND" "$HERE/gekkopak_retro_frontend.c" -ldl
clang --target=arm-none-eabi -march=armv6k -marm -c "$HERE/gekkopak_guest.S" -o "$PAYLOAD_OBJ"
ld.lld -Ttext=0x00100000 -e _start --oformat=elf32-littlearm "$PAYLOAD_OBJ" -o "$PAYLOAD"

export GEKKOPAK_SYSTEM_DIR="$BUILD/system"
export GEKKOPAK_SAVE_DIR="$BUILD/saves"
export GEKKOPAK_ASSETS_DIR="$BUILD/assets"
export GEKKOPAK_TRACE="$TRACE"

"$FRONTEND" "$CORE" "$PAYLOAD" 2>&1 | tee "$LOG"
python3 "$HERE/validate_trace.py" "$TRACE"
