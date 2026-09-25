#!/usr/bin/env python
"""Run the M0.2 dynamic-module PROTOTYPE tests under VICE and assert expected values.

Usage (Git Bash, after `ninja mos-platform/install` in the SDK build dir):
    python prototypes/c128-dynamic/check_m0.py
Each case shells out to m0_2/run.sh, which builds the test with the freshly
installed SDK and runs it in x128; this script parses the "symbol $ADDR:
bytes" output and compares. Takes several minutes.

These tests belong to the prototype (modules.s, compat.c and the drivers; the runtime is the SDK's cache.c and cache-gate.s) and inspect
its internal state, so they read memory dumps rather than reporting through
test_set_result(). The M0.1 (static bank-1 placement) tests are ordinary SDK
tests: see test/c128 and `ninja test-c128`.
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
    if extra:  # appended, so an outer EXTRA_FLAGS survives
        env["EXTRA_FLAGS"] = (env.get("EXTRA_FLAGS", "") + " " + extra).strip()
    out = run([BASH, str(HERE / "m0_2" / "run.sh"), stage, " ".join(syms)], env)
    return check(title, parse(out), expected)

def cost(title):
    """Gate cost (m0_2/cost_test.c): cycles per gate round trip on the hit
    path, exact under VICE (display blanked, IRQs off). Asserts a band, not an
    exact value, so small gate tweaks don't need a test edit; the measured
    values are recorded in the milestone doc (M0_C128_BANKING_PLAN.md, 3)."""
    import os
    syms = ["t_base:2", "t_00:2", "t_01:2", "t_010:2", "t_0111:2", "t_miss_a:2",
            "t_miss_b:2", "t_miss_c:2", "cr_a", "cr_b", "cr_c", "cr_d", "loads"]
    out = run([BASH, str(HERE / "m0_2" / "run.sh"), "cost_test", " ".join(syms)], dict(os.environ))
    v = parse(out)
    def w(k):
        b = v.get(k, "00 00").split()
        return int(b[0], 16) + 256 * int(b[1], 16)
    reps = 16
    per = {k: (w(k) - w("t_base")) / reps for k in ("t_00", "t_01")}
    per["t_010"] = (w("t_010") - w("t_base")) / reps / 2
    per["t_0111"] = (w("t_0111") - w("t_base")) / reps / 2
    good = (v.get("cr_a") == "0E" and v.get("cr_b") == "0E" and v.get("cr_c") == "4E"
            and v.get("cr_d") == "4E" and v.get("loads") == "04"
            and all(340 <= x <= 370 for x in per.values())
            and 15000 <= w("t_miss_a") < w("t_miss_b") and w("t_miss_c") < 65535)
    print(("PASS " if good else "FAIL ") + title)
    print("    per gate crossing (cycles, incl. trivial callee): "
          + ", ".join(f"{k[2:]}={x:.0f}" for k, x in per.items())
          + f"; miss (load+call): A={w('t_miss_a')} B={w('t_miss_b')} C={w('t_miss_c')}")
    return good

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
    edge_syms = ["big:2", "ev_delta_big", "cd_alive", "refused_delta", "r_bank", "ev_a", "ev_b", "ev_n", "f_res", "b_again", "act", "ams_end"]
    edge_exp = {"big": "01 01", "ev_delta_big": "00", "cd_alive": "01", "refused_delta": "01", "r_bank": "4E", "ev_a": "00", "ev_b": "01", "ev_n": "02",
                "f_res": "0A", "b_again": "0A", "act": "00", "ams_end": "00"}
    ok &= m0_2("edge_test", edge_syms, edge_exp, "M0.2 pinning blocks load (pre-check: no side effects); CLOCK prefers cold")
    spill_syms = ["f0_full", "sp_a", "ev_a", "b0", "b1", "b2", "b3", "b4", "data_ok1", "n_big",
                  "oom_malloc", "free0_end", "free1_end", "oom_lock_null", "data_ok2", "freed_ok"]
    spill_exp = {"f0_full": "00", "sp_a": "03", "ev_a": "00", "b0": "01", "b1": "01", "b2": "01",
                 "b3": "00", "b4": "00", "data_ok1": "01", "n_big": "0B", "oom_malloc": "01",
                 "free0_end": "02", "free1_end": "02", "oom_lock_null": "01", "data_ok2": "01",
                 "freed_ok": "01"}
    ok &= m0_2("spill_test", spill_syms, spill_exp, "M0.2 objects spill to other bank; true OOM -> NULL, no loss")
    defrag_syms = ["qb1", "qb2", "addr_before:2", "addr_pinned:2", "addr_after:2", "moves_pinned",
                   "moves_b", "delta_ok", "qa1", "qa2", "qa3", "qa4", "e_moved", "e_same", "e_data",
                   "frag_ok", "big_ok", "big_b", "a_data"]
    defrag_exp = {"qb1": "01", "qb2": "02", "addr_before": "60 10", "addr_pinned": "60 10",
                  "addr_after": "00 10", "moves_pinned": "00", "moves_b": "01", "delta_ok": "01",
                  "qa1": "02", "qa2": "0D", "qa3": "0F", "qa4": "0F", "e_moved": "01", "e_same": "01",
                  "e_data": "01", "frag_ok": "01", "big_ok": "01", "big_b": "01", "a_data": "01"}
    ok &= m0_2("defrag_test", defrag_syms, defrag_exp, "M0.2 defragmentation (module relocation, pinning, auto on alloc)")
    shared_syms = ["en", "pool_sz0", "a1", "r1", "bank0_full", "more_than_static", "all_tags_ok1",
                   "hooked", "pool_min", "yielded3", "oom_end", "disjoint1", "r_still", "a_dropped",
                   "spilled", "r_reloc_ok1", "cache_full", "tags_ok2", "malloc_still", "exact_free1",
                   "adjacent", "moved", "pool_after1", "r_reloc_ok2", "r_sm_ok", "a_reload", "objs_ok",
                   "pin_refused", "pin_size", "pin_moves_same", "after_unpin", "at_max", "want_cleared",
                   "low_ok", "polite_shrunk", "no_hook_used", "no_failed", "exact_free2", "end_ok"]
    shared_exp = {k: "01" for k in shared_syms}
    shared_exp.update({"en": "00", "pool_sz0": "06", "a1": "05", "pool_min": "03", "yielded3": "03",
                       "pool_after1": "07", "pin_size": "07", "after_unpin": "0B", "at_max": "0C"})
    ok &= m0_2("shared_test", shared_syms, shared_exp,
               "M0.2 shared mode: cache and malloc heap collaborate (polling, reclaim hook, relocation)",
               "-Os")   # -Os: at the default -O2 the program leaves the heap too little room
    auto_syms = ["en", "off_no_poll", "low_ok", "shrunk", "no_hook", "above_low", "tags1", "grew", "data_ok", "mod_ok"]
    auto_exp = {k: "01" for k in auto_syms}
    auto_exp["en"] = "00"
    ok &= m0_2("auto_test", auto_syms, auto_exp,
               "M0.2 automatic polling: safe points run the cache service (no explicit call)", "-Os")
    ok &= cost("M0.2 gate cost: hit path ~350 cycles per crossing in every bank pairing")
    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
