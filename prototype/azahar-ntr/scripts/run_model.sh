#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$HERE/build/model"
mkdir -p "$BUILD"

c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  "$HERE/model/gekkopak_ntr_model.cpp" "$HERE/model/ntr_bus_test.cpp" \
  -o "$BUILD/ntr_bus_test"
"$BUILD/ntr_bus_test"

c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  "$HERE/model/ntr_v1_budget.cpp" \
  -o "$BUILD/ntr_v1_budget"
"$BUILD/ntr_v1_budget"

c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  "$HERE/model/gekkopak_ntr_model.cpp" "$HERE/model/ntr_v1_model_test.cpp" \
  -o "$BUILD/ntr_v1_model_test"
"$BUILD/ntr_v1_model_test"
