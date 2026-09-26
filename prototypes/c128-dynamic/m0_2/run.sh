#!/bin/bash
# Build and run an M0.2 test under VICE. Usage: run.sh <stage> "<symbols to print>"
#   stage = gate_test (M0.2.1 ...). Sources: <stage>.c modules.s compat.c, on the SDK runtime (cache.h)
# -mlto-zp=60 shrinks the compiler's own zero-page budget (platform default
# 102) so the gate's zero-page temporaries fit in the ~102-byte pool.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
MOS_CLANG="${MOS_CLANG:-mos-clang}"
: "${SDK_INSTALL:?set SDK_INSTALL to the SDK install prefix (the directory holding bin/mos-c128.cfg)}"
WORK="${WORK:-/tmp/mos_test}"; mkdir -p "$WORK"; cd "$WORK"
STAGE="${1:-gate_test}"
"$MOS_CLANG" --config "$SDK_INSTALL/bin/mos-c128.cfg" -mlto-zp=60 -I"$HERE" $EXTRA_FLAGS \
  -o "$STAGE.prg" -Wl,-Map="$STAGE.map" "$HERE/$STAGE.c" "$HERE/modules.s" "$HERE/compat.c"
bash "$HERE/../run_vice_syms.sh" "$STAGE.prg" "$STAGE.map" "${2:-r0 r1 r2 r3 r4 err}"
