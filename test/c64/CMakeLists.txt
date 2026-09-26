cmake_minimum_required(VERSION 3.18)

project(test-c64 LANGUAGES C CXX)

include(../test.cmake)

set(VICE_EMULATOR ${VICE_X64SC_COMMAND})
set(VICE_LIBRETRO_CORE ${LIBRETRO_VICE_X64SC_CORE})

# Exit path in a program that is large (exit() in the range where BASIC's ROM
# reappears, $A000 and up): finishes and reports, and hands control back to
# BASIC.
add_vice_test(exit-large)
# Heap tests shared with the C128 (../commodore).
add_vice_test(heap-growth SOURCE ../commodore/heap-growth.c)
add_vice_test(realloc SOURCE ../commodore/realloc.c)
add_vice_test(aligned-alloc SOURCE ../commodore/aligned-alloc.c)
add_vice_test(malloc-hook SOURCE ../commodore/malloc-hook.c)
add_vice_test(cxx-new-delete SOURCE ../commodore/cxx-new-delete.cc)
add_vice_basic_return_test(basic-return)
add_vice_basic_return_test(basic-return-large)
