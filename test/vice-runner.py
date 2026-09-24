#!/usr/bin/env python3
"""Run a C128 test program under VICE (x128) and report its result.

Usage: vice-runner.py --vice <x128> --prg <test.prg> --map <test.map>
                      [--restore-range 0800-09ff] [--timeout 120]

Result protocol: the same one the emutest runner uses (see test/README.md and
test-lib-emutest.c). The test calls test_set_result(bool), which stores the
signature "TestPass" or "TestFail" in the RAM array test_result. This script
runs the program until the end of its exit handlers (the .fini_rts section,
reached both when main returns and when exit() is called), dumps test_result
through the VICE monitor, and decodes it.

If the program links bank1.o (it has an .init.012 section), the
"Common-RAM code area" that c128_bank1_call borrows is also checked: its
bytes are dumped just before the program overwrites them and again after the
exit handlers, and must be identical (the platform promises to restore
them). The area defaults to the platform default, $0800-$09FF; pass
--restore-range if the test moves it.

Exit status: 0 pass, 1 fail, 2 no result (crash, hang, timeout, or the
signature was never set).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

SIGNATURES = {b"TestPass": 0, b"TestFail": 1}


def parse_map(path):
    """Return (symbols, sections): name -> address, from an lld -Map file."""
    symbols, sections = {}, {}
    for line in open(path, errors="replace"):
        m = re.match(r"^\s+([0-9a-f]+)\s+[0-9a-f]+\s+[0-9a-f]+\s+\d+\s+(.*\S)\s*$", line)
        if not m:
            continue
        addr, rest = int(m.group(1), 16), m.group(2)
        sec = re.search(r"\((\.[^)\s]+)\)$", rest)
        if sec:
            sections.setdefault(sec.group(1), addr)
        elif re.fullmatch(r"[A-Za-z_][\w.]*", rest):
            symbols.setdefault(rest, addr)
    return symbols, sections


def strip_header(data):
    # The monitor's `save` writes a two-byte load address first.
    return data[2:]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--vice", required=True, help="path to x128")
    ap.add_argument("--prg", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--restore-range", default="0800-09ff",
                    help="hex start-end of the Common-RAM code area to check")
    ap.add_argument("--timeout", type=float, default=120)
    args = ap.parse_args()

    symbols, sections = parse_map(args.map)
    if "test_result" not in symbols or ".fini_rts" not in sections:
        print("runner: test_result/.fini_rts not found in the map; does the test "
              "link test-lib-emutest?", file=sys.stderr)
        return 2
    result_addr = symbols["test_result"]
    end_addr = sections[".fini_rts"]
    check_restore = ".init.012" in sections
    lo, hi = (int(x, 16) for x in args.restore_range.split("-"))

    with tempfile.TemporaryDirectory(prefix="vice-test-") as tmp:
        def p(name):
            return os.path.join(tmp, name).replace("\\", "/")

        mon = []
        if check_restore:
            # First stop: just before .init.012 copies the common code over the area.
            mon += [f'save "{p("before.bin")}" 0 {lo:04x} {hi:04x}',
                    f"break {end_addr:x}", "x"]
            first_stop = sections[".init.012"]
        else:
            first_stop = end_addr
        mon += [f'save "{p("result.bin")}" 0 {result_addr:04x} {result_addr + 7:04x}']
        if check_restore:
            mon += [f'save "{p("after.bin")}" 0 {lo:04x} {hi:04x}']
        mon += ["quit"]
        with open(p("script.mon"), "w") as f:
            f.write("\n".join(mon) + "\n")

        # -initbreak takes a DECIMAL address.
        cmd = [args.vice, "-default", "-initbreak", str(first_stop),
               "-moncommands", p("script.mon"), "-autostart", os.path.abspath(args.prg)]
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + args.timeout
        while proc.poll() is None and time.time() < deadline:
            time.sleep(0.25)
        timed_out = proc.poll() is None
        if timed_out:
            proc.kill()
        proc.wait()

        if not os.path.exists(p("result.bin")):
            print("FAIL (no result): the program never reached the end of its exit "
                  "handlers" + (" (timeout)" if timed_out else ""))
            return 2
        sig = strip_header(open(p("result.bin"), "rb").read())[:8]
        status = SIGNATURES.get(sig)
        if status is None:
            print(f"FAIL (no result): test_result holds {sig!r}; the test never "
                  "called test_set_result()")
            return 2
        if status != 0:
            print("FAIL: the test reported failure (TestFail)")
            return 1
        if check_restore:
            before = strip_header(open(p("before.bin"), "rb").read())
            after = strip_header(open(p("after.bin"), "rb").read())
            if before != after:
                diff = [f"${lo + i:04X}" for i, (a, b) in enumerate(zip(before, after)) if a != b]
                print(f"FAIL: ${lo:04X}-${hi:04X} was not restored at exit; differs at "
                      + " ".join(diff[:16]))
                return 1
        print("PASS" + (f" (${lo:04X}-${hi:04X} restored)" if check_restore else ""))
        return 0


if __name__ == "__main__":
    sys.exit(main())
