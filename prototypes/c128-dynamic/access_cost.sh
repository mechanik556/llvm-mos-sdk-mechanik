#!/bin/bash
# Measure the cost of reading one byte from C128 bank 1 versus a normal load
# (see access_cost.c): builds it standalone, runs it under VICE, prints cycles
# per access. Env: MOS_CLANG, SDK_INSTALL, WORK (as m0_2/run.sh).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
MOS_CLANG="${MOS_CLANG:-/c/Users/mecha/git/llvm-mos-mechanik/build/bin/mos-clang.exe}"
SDK_INSTALL="${SDK_INSTALL:-/c/Users/mecha/git/llvm-mos-sdk-mechanik/install}"
WORK="${WORK:-/tmp/mos_test}"; mkdir -p "$WORK"; cd "$WORK"
"$MOS_CLANG" --config "$SDK_INSTALL/bin/mos-c128.cfg" -Os -o access_cost.prg \
  -Wl,-Map=access_cost.map "$HERE/access_cost.c"
bash "$HERE/run_vice_syms.sh" access_cost.prg access_cost.map \
  "t_empty:2 t_direct:2 t_call:2 ok" | tee access_cost.out
python - access_cost.out << 'PYEOF'
import re, sys
v = {}
for line in open(sys.argv[1]):
    m = re.match(r"(\w+)\s+\$[0-9A-F]+: ((?:[0-9A-F]{2} ?)+)", line)
    if m:
        v[m.group(1)] = int.from_bytes(bytes.fromhex(m.group(2).replace(" ", "")), "little")
e = v["t_empty"]
print(f"ok={v['ok']}  direct load: {(v['t_direct'] - e) / 16:.1f} cycles/access,"
      f" bank-1 read via c128_bank1_call: {(v['t_call'] - e) / 16:.1f} cycles/access")
PYEOF
