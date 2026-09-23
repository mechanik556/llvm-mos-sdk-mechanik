#!/bin/bash
# Usage: check_restore.sh <test.c> [driver flags...]
# In ONE VICE run (separate runs differ: VICE seeds random noise into RAM),
# dumps $0800-$09FF at .init.012 (before the borrowed area is overwritten)
# and after all exit handlers (.fini_rts), and reports whether identical.
set -e
SRC="$1"; shift
export EXTRA_FLAGS="$*"
cd "$(dirname "$0")/.."
bash test/run_m0_vice.sh "$(realpath "$SRC")" >/dev/null 2>&1 || true
NAME=$(basename "$SRC" .c); cd /tmp/mos_test
A=$(grep -E "\(\.init\.012\)" $NAME.map | awk '{print $1}')
B=$(grep -E "\(\.fini_rts\)" $NAME.map | awk '{print $1}')
W=$(cygpath -m /tmp/mos_test)
cat > mon_two.txt << EOM
save "$W/${NAME}_before.bin" 0 0800 09ff
break $B
x
save "$W/${NAME}_after.bin" 0 0800 09ff
quit
EOM
rm -f ${NAME}_before.bin ${NAME}_after.bin; taskkill //F //IM x128.exe >/dev/null 2>&1 || true
"/c/C64/GTK3VICE-3.10-win64/bin/x128.exe" -default -initbreak "$((16#$A))" -moncommands mon_two.txt -autostart $NAME.prg > v2.log 2>&1 &
disown
for i in $(seq 1 8); do sleep 5; [ "$(tasklist 2>/dev/null | grep -c x128.exe)" -eq 0 ] && break; done
taskkill //F //IM x128.exe >/dev/null 2>&1 || true
[ -f ${NAME}_after.bin ] || { echo "[$*] FAIL: never reached end of exit handlers"; exit 1; }
if cmp -s ${NAME}_before.bin ${NAME}_after.bin; then echo "[$*] IDENTICAL"; else echo "[$*] DIFFER: $(cmp -l ${NAME}_before.bin ${NAME}_after.bin | awk '{printf "$%04X ", 0x0800+$1-3}')"; fi
