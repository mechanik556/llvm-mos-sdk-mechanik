#!/bin/bash
# Build and run an M0 bank-1 test under VICE x128, then print zero page
# ($2A onward) and the decoded text screen. Usage: run_m0_vice.sh <test.c>
# Env overrides: MOS_CLANG, SDK_INSTALL, VICE_X128, WORK.
# Notes: -initbreak needs a DECIMAL address; monitor `echo` output is not
# capturable, so results are dumped to files with `save`. Run from Git Bash.
set -e
SRC="$1"
MOS_CLANG="${MOS_CLANG:-/c/Users/mecha/git/llvm-mos-mechanik/build/bin/mos-clang.exe}"
SDK_INSTALL="${SDK_INSTALL:-/c/Users/mecha/git/llvm-mos-sdk-mechanik/install}"
VICE_X128="${VICE_X128:-/c/C64/GTK3VICE-3.10-win64/bin/x128.exe}"
WORK="${WORK:-/tmp/mos_test}"
mkdir -p "$WORK"; cd "$WORK"
NAME=$(basename "$SRC" .c)
"$MOS_CLANG" --config "$SDK_INSTALL/bin/mos-c128.cfg" -o "$NAME.prg" \
  -Wl,-Map="$NAME.map" "$SRC"
EXIT_HEX=$(grep -E "\(\.text\.exit\)" "$NAME.map" | head -1 | awk '{print $1}')
EXIT_DEC=$((16#$EXIT_HEX))
echo "exit at \$$EXIT_HEX ($EXIT_DEC)"
grep -E "^ +[0-9a-f]+ +[0-9a-f]+ +[0-9a-f]+ +1 +.*c128commoncode$|__c128commoncode_(vma_start|size)" "$NAME.map" || true
WIN_WORK=$(cygpath -m "$WORK")
cat > "$NAME.mon" <<EOF
save "$WIN_WORK/$NAME.zp.bin" 0 0000 00ff
save "$WIN_WORK/$NAME.screen.bin" 0 0400 07e7
quit
EOF
rm -f "$NAME.zp.bin" "$NAME.screen.bin"
taskkill //F //IM x128.exe >/dev/null 2>&1 || true
"$VICE_X128" -default -initbreak "$EXIT_DEC" -moncommands "$NAME.mon" \
  -autostart "$NAME.prg" > "$NAME.vice.log" 2>&1 &
disown
for i in $(seq 1 8); do
  sleep 5
  [ "$(tasklist 2>/dev/null | grep -c x128.exe)" -eq 0 ] && break
done
taskkill //F //IM x128.exe >/dev/null 2>&1 || true
[ -f "$NAME.zp.bin" ] || { echo "FAIL: never reached exit (hang?)"; exit 1; }
xxd -s 2 -l 256 "$NAME.zp.bin" | sed -n '3,4p'   # bytes $20-$4F of zero page
python - "$NAME.screen.bin" << 'PYEOF'
import sys
sc=open(sys.argv[1],'rb').read()[2:]
dec=lambda b: chr(96+b) if 1<=b<=26 else chr(b) if 32<=b<=63 else '.'
for r in range(25):
    l=''.join(dec(b) for b in sc[r*40:(r+1)*40]).rstrip()
    if l.strip('. '): print(l)
PYEOF
