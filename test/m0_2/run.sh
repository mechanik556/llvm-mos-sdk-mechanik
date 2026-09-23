#!/bin/bash
# Build and run an M0.2 test under VICE. Usage: run.sh <stage> "<symbols to print>"
#   stage = gate_test (M0.2.1 ...). Sources: gate_test.c gate.s modules.s
# -mlto-zp=70 shrinks the compiler's own zero-page budget (platform default
# 102) so the gate's zero-page temporaries fit in the ~102-byte pool.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
MOS_CLANG="${MOS_CLANG:-/c/Users/mecha/git/llvm-mos-mechanik/build/bin/mos-clang.exe}"
SDK_INSTALL="${SDK_INSTALL:-/c/Users/mecha/git/llvm-mos-sdk-mechanik/install}"
WORK="${WORK:-/tmp/mos_test}"; mkdir -p "$WORK"; cd "$WORK"
STAGE="${1:-gate_test}"
"$MOS_CLANG" --config "$SDK_INSTALL/bin/mos-c128.cfg" -mlto-zp=70 $EXTRA_FLAGS \
  -o "$STAGE.prg" -Wl,-Map="$STAGE.map" "$HERE/$STAGE.c" "$HERE/gate.s" "$HERE/modules.s"
bash "$HERE/../run_vice_syms.sh" "$STAGE.prg" "$STAGE.map" "${2:-r0 r1 r2 r3 r4 err}"
