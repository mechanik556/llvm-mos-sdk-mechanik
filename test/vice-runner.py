#!/usr/bin/env python3
"""Run a C64 or C128 test program under VICE (x64sc/x128) and report its result.

Usage: vice-runner.py --vice <x128> --prg <test.prg> --map <test.map>
                      [--restore-range 0800-09ff] [--timeout 120]
                      [--expect-basic-prompt]

Result protocol: the same one the emutest runner uses (see test/README.md and
test-lib-emutest.c). The program exits with a status - returning it from main,
or calling exit() - and test-lib-emutest's _Exit stores the signature
"TestPass" (status 0) or "TestFail" in the RAM array test_result and then
spins. This script runs the program until the end of its exit handlers (the
.fini_rts section), steps the CPU on through _Exit, dumps test_result through
the VICE monitor, and decodes it.

On the c128, if the program links bank1.o (it has an .init.012 section), the
"Common-RAM code area" that c128_bank1_call borrows is also checked: its
bytes are dumped just before the program overwrites them and again after the
exit handlers, and must be identical (the platform promises to restore
them). The area defaults to the platform default, $0800-$09FF; pass
--restore-range if the test moves it.

--expect-basic-prompt is a different kind of test: the program is linked with
save-basic.o (not test-lib-emutest) and returns to BASIC. The runner stops at
_Exit, lets the machine run on, and passes if BASIC's READY. prompt is on the
screen afterwards - i.e. the return, including the restore of BASIC's memory
configuration, worked.

Exit status: 0 pass, 1 fail, 2 no result (crash, hang, timeout, or the
program never reached _Exit).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

SIGNATURES = {b"TestPass": 0, b"TestFail": 1}

# Instructions to execute after the exit handlers so _Exit can store the
# signature (about a hundred are needed; the rest are the spin loop).
EXIT_STEPS = 3000


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


def screen_text(data):
    """Decode 40 x 25 screen RAM (screen codes) to text lines."""
    def dec(b):
        b &= 0x7F
        return chr(64 + b) if 1 <= b <= 26 else chr(b) if 32 <= b <= 63 else "."
    return ["".join(dec(b) for b in data[r * 40:(r + 1) * 40]).rstrip() for r in range(25)]


def run_basic_prompt(args, symbols):
    if "_Exit" not in symbols:
        print("runner: _Exit not found in the map; is the program linked with "
              "save-basic.o?", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="vice-test-") as tmp:
        def p(name):
            return os.path.join(tmp, name).replace("\\", "/")

        # Stop when the program calls _Exit; BASIC then needs some time to
        # print its prompt (the count is hexadecimal instructions).
        mon = ["bank ram", "z 20000", f'save "{p("screen.bin")}" 0 0400 07e7', "quit"]
        with open(p("script.mon"), "w") as f:
            f.write("\n".join(mon) + "\n")
        cmd = [args.vice, "-default", "-initbreak", str(symbols["_Exit"]),
               "-moncommands", p("script.mon"), "-autostart", os.path.abspath(args.prg)]
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + args.timeout
        while proc.poll() is None and time.time() < deadline:
            time.sleep(0.25)
        timed_out = proc.poll() is None
        if timed_out:
            proc.kill()
        proc.wait()
        if not os.path.exists(p("screen.bin")):
            print("FAIL (no result): the program never reached _Exit"
                  + (" (timeout)" if timed_out else ""))
            return 2
        text = screen_text(open(p("screen.bin"), "rb").read()[2:])
        if any("READY." in line for line in text):
            print("PASS (returned to BASIC)")
            return 0
        print("FAIL: BASIC's READY. prompt is not on the screen after _Exit")
        return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--vice", required=True, help="path to x128")
    ap.add_argument("--prg", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--restore-range", default="0800-09ff",
                    help="hex start-end of the Common-RAM code area to check")
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--expect-basic-prompt", action="store_true",
                    help="pass if the program returns to BASIC's READY. prompt")
    args = ap.parse_args()

    symbols, sections = parse_map(args.map)
    if args.expect_basic_prompt:
        return run_basic_prompt(args, symbols)
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

        # Read RAM, not the CPU's current view: after the exit handlers restore
        # BASIC's memory configuration, $4000-$BFFF shows BASIC ROM.
        mon = ["bank ram"]
        if check_restore:
            # First stop: just before .init.012 copies the common code over the area.
            mon += [f'save "{p("before.bin")}" 0 {lo:04x} {hi:04x}',
                    f"break {end_addr:x}", "x",
                    f'save "{p("after.bin")}" 0 {lo:04x} {hi:04x}']
            first_stop = sections[".init.012"]
        else:
            first_stop = end_addr
        mon += [f"z {EXIT_STEPS}",
                f'save "{p("result.bin")}" 0 {result_addr:04x} {result_addr + 7:04x}',
                "quit"]
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
            print(f"FAIL (no result): test_result holds {sig!r}; the program never "
                  "reached _Exit (is test-lib-emutest linked?)")
            return 2
        if status != 0:
            print("FAIL: the program exited with a failure status (TestFail)")
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
