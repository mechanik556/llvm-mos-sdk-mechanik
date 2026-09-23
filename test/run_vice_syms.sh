#!/bin/bash
# Usage: run_vice_syms.sh <prog.prg> <prog.map> "sym[:len] sym[:len] ..."
# Runs the PRG under VICE x128 to __after_main (runs only after main returns;
# exit() itself is a 6-byte function the linker can fold with an early one), dumps $0000-$BFFF
# (bank 0 view) and prints the named symbols' bytes (addresses from the
# link map). Env: VICE_X128, WORK.
set -e
PRG="$(realpath "$1")"; MAP="$(realpath "$2")"; SYMS="$3"
VICE_X128="${VICE_X128:-/c/C64/GTK3VICE-3.10-win64/bin/x128.exe}"
WORK="${WORK:-/tmp/mos_test}"; mkdir -p "$WORK"; cd "$WORK"
NAME=$(basename "$PRG" .prg)
EXIT_HEX=$(grep -E "\(\.after_main\)" "$MAP" | head -1 | awk '{print $1}')
W=$(cygpath -m "$WORK")
printf 'save "%s/%s.mem" 0 0000 bfff\nquit\n' "$W" "$NAME" > "$NAME.mon"
rm -f "$NAME.mem"; taskkill //F //IM x128.exe >/dev/null 2>&1 || true
"$VICE_X128" -default -initbreak "$((16#$EXIT_HEX))" -moncommands "$NAME.mon" -autostart "$PRG" > "$NAME.vice.log" 2>&1 &
disown
for i in $(seq 1 8); do sleep 5; [ "$(tasklist 2>/dev/null | grep -c x128.exe)" -eq 0 ] && break; done
taskkill //F //IM x128.exe >/dev/null 2>&1 || true
[ -f "$NAME.mem" ] || { echo "FAIL: never reached exit (hang?)"; exit 1; }
python - "$NAME.mem" "$MAP" "$SYMS" << 'PYEOF'
import sys,re
mem=open(sys.argv[1],'rb').read()[2:]
addr={}
for l in open(sys.argv[2],errors='replace'):
    p=l.split()
    if len(p)==5 and p[3]=='1' and re.fullmatch(r'[0-9a-f]+',p[0]) and re.fullmatch(r'[A-Za-z_][\w.]*',p[4]): addr.setdefault(p[4],int(p[0],16))
for spec in sys.argv[3].split():
    n,_,ln=spec.partition(':'); ln=int(ln or 1)
    if n not in addr: print(f"{n}: (not in map)"); continue
    a=addr[n]; print(f"{n:10s} ${a:04X}: "+' '.join(f"{b:02X}" for b in mem[a:a+ln]))
PYEOF
