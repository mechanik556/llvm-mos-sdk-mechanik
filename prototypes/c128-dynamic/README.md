# C128 dynamic module prototype (not part of the SDK)

Working prototype of runtime-loaded, relocatable code modules and cacheable heap
objects across the C128's two RAM banks (milestone M0.2 of the dynamic-linking
work; see `work/M0_C128_BANKING_PLAN.md` in llvm-mos-mechanik). It is a proof of
the design, not shippable library code: it is hand-authored assembly plus a C
allocator, it needs `-mlto-zp=70` to leave zero page for the gate, and nothing
in `mos-platform` depends on it. It lives here, outside `test/`, so the SDK's
test tree contains only tests.

* `m0_2/modules.s` - hand-written test modules and their module table
* `m0_2/compat.{h,c}` - map the tests' old names onto the SDK runtime and set up
  the pools
* `m0_2/*_test.c` - test drivers; `check_m0.py` runs them all under VICE

The runtime itself (call gate, module table loader/relocator/evictor, object heap,
shared mode with `malloc`, polling, host services for modules) is the SDK's:
`mos-platform/c128/cache.{h,c}`, `cache-gate.s`, `cache-host.{c,s}` (milestone
M0.3). This directory keeps the module-level tests that need hand-written module
code: gate timing, eviction/pinning, relocation, host services, and shared mode with
resident modules. (The prototype's own allocator and gate, and the rejected
Common-RAM-table gate variant, were removed when the SDK runtime replaced them; they
are in git history.)

Run: `ninja mos-platform/install` in the SDK build directory, then
`python prototypes/c128-dynamic/check_m0.py`. The tests dump memory at
`__after_main` and compare internal state (`run_vice_syms.sh`), so they are not
self-checking in the `test_set_result()` sense; paths default to this
machine's layout and can be overridden with `MOS_CLANG`, `SDK_INSTALL`,
`VICE_X128`.

The static bank-1 placement it builds on (`c128_bank1_call`, `bank1.h`) is
tested in `test/c128`.
