# Unit Tests

## Adding new Emutest/Libretro tests

You can write tests against any Libretro core found by CMake:

```cmake
  add_emutest_test(<name> <ext> <source_dir> <libretro_core>)
```

* name - Project suffix, also prefix of C file
* ext - Output binary/ROM file extension (ex: a26)
* source_dir - Look for {name}.c in this relative path, usually "."
* libretro_core - Variable containing path to Libretro library file

Usually these are invoked via wrapper function, see `test/test.cmake`.

To add new Libretro cores and wrapper functions to the project, search for `LIBRETRO_STELLA_CORE` and use those lines as a template for your new core.

How to report results from a test case:

* Call `test_set_result(bool)` with a pass/fail value, and then go into a busy loop or video display loop, or
* Exit from `main()` with a status code -- zero for success, non-zero for failure, or
* Set the `EMUTEST_FB_CRC_PASS` variable to the CRC of a known good video frame (you can find these in the test log files.)

## C128 tests (VICE)

`test/c128` holds the Commodore 128 tests. They are built and registered like
the other platforms' (`ninja test-c128`, or `ninja test` for everything), and
contain three kinds of test:

* `compile/` - programs that must build and link (`add_compile_test`).
* `no-compile/` - programs that must fail to link, e.g. by overflowing a
  memory region (`add_no_compile_test`).
* Emulator tests (`add_vice_test`) - run under VICE's `x128` by
  `test/vice-runner.py` and reported through the same protocol as the emutest
  tests above: the program exits with a status - returns `EXIT_SUCCESS` or
  `EXIT_FAILURE` from `main` - and `test-lib-emutest`'s `_Exit` stores
  `TestPass`/`TestFail` in the RAM array `test_result`. The runner runs the
  program through its exit handlers and `_Exit` (a program that hangs or never
  exits fails), reads `test_result` from RAM through the VICE monitor, and
  decodes it.

  Do not call `test_set_result()` and then `return 0`: on this platform
  returning from `main` goes through `exit()` to `_Exit(0)`, which overwrites
  the signature with `TestPass`. Return the status instead.

  Keep emulator test programs small. At exit the platform restores BASIC's
  memory configuration before `_Exit` runs, which maps ROM over `$4000-$BFFF`;
  a program whose `.rodata` or `test_result` lies above `$4000` (roughly, one
  that pulls in stdio) makes `_Exit` store ROM bytes instead of the signature,
  and the runner reports "no result". Use the KERNAL wrappers in `<cbm.h>`
  rather than `printf` where a test needs screen or disk activity.

VICE is found through the `VICE_DIR` environment variable (its install
directory) or `-DVICE_X128_COMMAND=<path to x128>`. Without it the emulator
tests are not registered - the programs are still built, and the compile and
no-compile tests still run. The tests need Python 3 for the runner and open an
emulator window; CTest runs them one at a time.

```cmake
  add_vice_test(<name>)                 # <name>.c
  add_vice_test(<name> SOURCE other.c   # same source, different link options
    LINK_OPTIONS -Wl,--defsym=...
    RESTORE_RANGE 0c00-0dff)            # Common-RAM code area (see below)
```

When a program links the C128 bank-1 support (`bank1.h`), the runner also
checks the platform's promise to restore the Common-RAM code area at exit: it
dumps `$0800-$09FF` (or `RESTORE_RANGE`) before the platform first overwrites
it and again after the exit handlers, and the two must be identical.

Run `vice-runner.py` directly for one program (`--vice`, `--prg`, `--map`; the
map is written by `-Wl,-Map=`); its exit status is 0 for pass, 1 for fail and 2
for no result.

