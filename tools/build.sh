#!/bin/bash
# Configure and build the firmware. Environment (all optional):
#   PICO_SDK_PATH   pico-sdk 2.1.1 checkout (default: third_party/pico-sdk)
#   TOOLCHAIN_BIN   directory with arm-none-eabi-gcc (default: from PATH)
#   BUILD_DIR       output directory (default: build)
set -euo pipefail
cd "$(dirname "$0")/.."
[ -n "${TOOLCHAIN_BIN:-}" ] && export PATH="$TOOLCHAIN_BIN:$PATH"
B="${BUILD_DIR:-build}"
cmake -S . -B "$B" -G Ninja -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release
cmake --build "$B"
ls -l "$B"/*.uf2
