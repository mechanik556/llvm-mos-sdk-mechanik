# C128 dynamic module prototype (not part of the SDK)

Working prototype of runtime-loaded, relocatable code modules and cacheable heap
objects across the C128's two RAM banks (milestone M0.2 of the dynamic-linking
work; see `work/M0_C128_BANKING_PLAN.md` in llvm-mos-mechanik). It is a proof of
the design, not shippable library code: it is hand-authored assembly plus a C
allocator, it needs `-mlto-zp=70` to leave zero page for the gate, and nothing
in `mos-platform` depends on it. It lives here, outside `test/`, so the SDK's
test tree contains only tests.

* `m0_2/gate.s` - `call_gate`/`exit_gate` (bank-aware, load-on-miss)
* `m0_2/gate_ct.s` - variant with the Module Table in Common RAM, kept as
  evidence for the (rejected) mirror-array optimization
* `m0_2/modtab.c` - Module Table, two-bank allocator, relocation, eviction,
  spilling, defragmentation, heap objects
* `m0_2/modules.s` - hand-written test modules
* `m0_2/*_test.c` - test drivers; `check_m0.py` runs them all under VICE

Run: `ninja mos-platform/install` in the SDK build directory, then
`python prototypes/c128-dynamic/check_m0.py`. The tests dump memory at
`__after_main` and compare internal state (`run_vice_syms.sh`), so they are not
self-checking in the `test_set_result()` sense; paths default to this
machine's layout and can be overridden with `MOS_CLANG`, `SDK_INSTALL`,
`VICE_X128`.

The static bank-1 placement it builds on (`c128_bank1_call`, `bank1.h`) is
tested in `test/c128`.
