#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$HERE/build/model"
mkdir -p "$BUILD"

CXXFLAGS=(-std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror "-I$ROOT/include")
CORE="$ROOT/src/device.cpp"

c++ "${CXXFLAGS[@]}" \
  "$CORE" "$HERE/model/gekkopak_ntr_model.cpp" "$HERE/model/ntr_bus_test.cpp" \
  -o "$BUILD/ntr_bus_test"
"$BUILD/ntr_bus_test"

c++ "${CXXFLAGS[@]}" \
  "$HERE/model/ntr_v1_budget.cpp" \
  -o "$BUILD/ntr_v1_budget"
"$BUILD/ntr_v1_budget"

c++ "${CXXFLAGS[@]}" \
  "$CORE" "$HERE/model/gekkopak_ntr_model.cpp" "$HERE/model/ntr_v1_model_test.cpp" \
  -o "$BUILD/ntr_v1_model_test"
"$BUILD/ntr_v1_model_test"
