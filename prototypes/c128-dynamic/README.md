# C128 dynamic module prototype (not part of the SDK)

Module-level tests for the C128 runtime (milestone M0.2), kept from the original
proof of the design (see `work/M0_C128_BANKING_PLAN.md` in llvm-mos-mechanik). The
runtime they exercise now lives in the SDK (`mos-platform/c128`, milestone M0.3);
what stays here is hand-authored assembly test modules and drivers that inspect
the runtime's internal state, so they are not part of `test/`, which contains only
self-checking tests. Nothing in `mos-platform` depends on this directory. The
drivers are built with `-mlto-zp=60`, which leaves room for the gate's zero page
(the SDK tests use `-mreserve-zp=29`, see `cache.h`).

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
`python prototypes/c128-dynamic/check_m0.py` with `SDK_INSTALL` set to the SDK
install prefix (the directory holding `bin/mos-c128.cfg`), and `MOS_CLANG` and
`VICE_X128` if `mos-clang` and `x128` are not on `PATH`. The tests dump memory at
`__after_main` and compare internal state (`run_vice_syms.sh`), so they are not
self-checking in the `test_set_result()` sense. The scripts use Windows tools
(`taskkill`, `tasklist`, `cygpath`) and expect Git Bash; they belong to this
fork's development workflow and are not part of the SDK.

The static bank-1 placement it builds on (`c128_bank1_call`, `bank1.h`) is
tested in `test/c128`.
