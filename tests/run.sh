#!/bin/sh
# The app's logic and screens on this computer: no chip, no emulator for flashing.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
CC=${CC:-cc}
FLAGS="-std=gnu11 -O1 -Wall -Wextra -Isrc -Iplatform -Iplatform/hal -Iplatform/gen -DFOMA_BLUETOOTH=0 -Wno-missing-field-initializers"
$CC -std=c11 -O2 -Wall -Wextra -Isrc -o build/logic_test tests/logic_test.c
./build/logic_test
for board in 0 1; do
  $CC $FLAGS -DBOARD_SMK37=$board -o build/host_render_$board tests/host_render.c
  ./build/host_render_$board
done
