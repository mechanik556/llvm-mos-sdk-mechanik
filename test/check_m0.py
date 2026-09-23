#!/usr/bin/env python
"""Run every M0 (C128 banking) test under VICE and assert the expected values.

Usage (Git Bash, after `ninja mos-platform/install` in the SDK build dir):
    python test/check_m0.py
Each case shells out to the existing runners, which build the test with the
freshly installed SDK and run it in x128; this script parses their
"symbol $ADDR: bytes" output and compares. Takes several minutes.
"""
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASH = "bash"

def run(cmd, env=None):
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return p.stdout + p.stderr

def parse(out):
    vals = {}
    for line in out.splitlines():
        m = re.match(r"^(\S+)\s+\$[0-9A-F]{4}: ((?:[0-9A-F]{2} ?)+)\s*$", line)
        if m:
            vals[m.group(1)] = m.group(2).strip()
    return vals

def check(title, vals, expected):
    bad = [(k, v, vals.get(k)) for k, v in expected.items() if vals.get(k) != v]
    print(("PASS " if not bad else "FAIL ") + title)
    for k, want, got in bad:
        print(f"    {k}: expected {want}, got {got}")
    return not bad

def m0_2(stage, syms, expected, title, extra=""):
    import os
    env = dict(os.environ)
    if extra:
        env["EXTRA_FLAGS"] = extra
    out = run([BASH, str(HERE / "m0_2" / "run.sh"), stage, " ".join(syms)], env)
    return check(title, parse(out), expected)

def main():
    ok = True
    gate_syms = ["r1", "r2", "r3", "r4", "r_sum", "r_carry", "r_cin", "r_if", "r_inest",
                 "r_fail", "loads0", "q1", "q2", "q3", "q4", "q5", "cr1", "q_pinned",
                 "q_still", "ev", "q_gone", "ndiff", "diff_ok", "q_f", "q_sm2", "q_entry2",
                 "q_h", "res_c", "res_d", "res_f", "res_r", "res_h", "q_thrash", "q_sm3",
                 "r_cr", "r_act", "r_ams", "err", "ndiff2", "diff_ok2",
                 "rec10:2", "rec20:2", "calle:2", "act_h", "ams_h"]
    gate_exp = {"r1": "0A", "r2": "6C", "r3": "04", "r4": "08", "r_sum": "07",
                "r_carry": "01", "r_cin": "00", "r_if": "04", "r_inest": "04", "r_fail": "01",
                "loads0": "04", "q1": "0D", "q2": "0F", "q3": "0F", "q4": "01", "q5": "02",
                "cr1": "4E", "q_pinned": "01", "q_still": "01", "ev": "00", "q_gone": "00",
                "ndiff": "01", "diff_ok": "01", "q_f": "0A", "q_sm2": "02", "q_entry2": "0D",
                "q_h": "2A", "res_c": "00", "res_d": "00", "res_f": "00", "res_r": "00",
                "res_h": "01", "q_thrash": "08", "q_sm3": "02", "r_cr": "0E", "r_act": "00",
                "r_ams": "00", "err": "00", "ndiff2": "01", "diff_ok2": "01",
                "rec10": "0A 00", "rec20": "02 01", "calle": "01 01", "act_h": "00", "ams_h": "00"}
    ok &= m0_2("gate_test", gate_syms, gate_exp, "M0.2 gate/alloc/relocation/eviction/nesting")
    ok &= m0_2("gate_test", gate_syms, gate_exp, "  ... same with 80-column display", "-DVIDEO80")
    heap_syms = ["v_init", "e1", "b_after_incr", "v1", "b_after_lock0", "lk1", "e2_refused",
                 "e_free_locked", "e3", "v2", "lk_nested", "e_free", "e_free2", "b_h2_init",
                 "b_h2_lock0", "ev_delta", "b_h2_after_sum", "bytes_ok", "b_h2_back",
                 "r_b_reload", "cr_b_reload", "leak_err", "r_act", "r_ams"]
    heap_exp = {"v_init": "05", "e1": "00", "b_after_incr": "01", "v1": "06", "b_after_lock0": "00",
                "lk1": "01", "e2_refused": "01", "e_free_locked": "01", "e3": "00", "v2": "07",
                "lk_nested": "02", "e_free": "00", "e_free2": "02", "b_h2_init": "01",
                "b_h2_lock0": "00", "ev_delta": "01", "b_h2_after_sum": "01", "bytes_ok": "01",
                "b_h2_back": "00", "r_b_reload": "0A", "cr_b_reload": "4E", "leak_err": "00",
                "r_act": "00", "r_ams": "00"}
    ok &= m0_2("heap_test", heap_syms, heap_exp, "M0.2.4 heap objects / caller-bank locking")
    edge_syms = ["big:2", "r_bank", "ev_a", "ev_b", "ev_n", "f_res", "b_again", "act", "ams_end"]
    edge_exp = {"big": "01 01", "r_bank": "4E", "ev_a": "00", "ev_b": "01", "ev_n": "02",
                "f_res": "0A", "b_again": "0A", "act": "00", "ams_end": "00"}
    ok &= m0_2("edge_test", edge_syms, edge_exp, "M0.2 pinning blocks load; CLOCK prefers cold")
    spill_syms = ["f0_full", "sp_a", "ev_a", "b0", "b1", "b2", "b3", "b4", "data_ok1", "n_big",
                  "oom_malloc", "free0_end", "free1_end", "oom_lock_null", "data_ok2", "freed_ok"]
    spill_exp = {"f0_full": "00", "sp_a": "03", "ev_a": "00", "b0": "01", "b1": "01", "b2": "01",
                 "b3": "00", "b4": "00", "data_ok1": "01", "n_big": "0B", "oom_malloc": "01",
                 "free0_end": "02", "free1_end": "02", "oom_lock_null": "01", "data_ok2": "01",
                 "freed_ok": "01"}
    ok &= m0_2("spill_test", spill_syms, spill_exp, "M0.2 objects spill to other bank; true OOM -> NULL, no loss")
    # M0.1 tests use zero-page/screen output; check via the screen text.
    for t, want in (("m0_bank1_test", "initial=153 after=154"),
                    ("m0_bank1_test2", "counter=200 cr=14")):
        out = run([BASH, str(HERE / "run_m0_vice.sh"), str(HERE / f"{t}.c")])
        good = want in out
        print(("PASS " if good else "FAIL ") + f"M0.1 {t}: screen shows '{want}'")
        ok &= good
    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
