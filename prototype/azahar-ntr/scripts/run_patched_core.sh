#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
CORE="${AZAHAR_CORE:-${1:-}}"
if [[ -z "$CORE" || ! -f "$CORE" ]]; then
  echo "Set AZAHAR_CORE=/path/to/patched/azahar_libretro.so" >&2
  exit 2
fi
BUILD="$HERE/build/e2e"
mkdir -p "$BUILD/system" "$BUILD/saves" "$BUILD/assets"
GUEST="$($HERE/scripts/build_guest.sh "$BUILD")"
gcc -O2 -std=c11 -Wall -Wextra -Werror "$HERE/frontend/ntr_frontend.c" -ldl -o "$BUILD/ntr_frontend"
GEKKOPAK_SYSTEM_DIR="$BUILD/system" \
GEKKOPAK_SAVE_DIR="$BUILD/saves" \
GEKKOPAK_ASSETS_DIR="$BUILD/assets" \
  "$BUILD/ntr_frontend" "$CORE" "$GUEST"
