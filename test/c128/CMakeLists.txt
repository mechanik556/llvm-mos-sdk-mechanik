cmake_minimum_required(VERSION 3.18)

project(test-c128 LANGUAGES C CXX ASM)

include(../test.cmake)

set(VICE_EMULATOR ${VICE_X128_COMMAND})
set(VICE_LIBRETRO_CORE ${LIBRETRO_VICE_X128_CORE})

# Common-RAM code area (bank1.h): the default, and moved to the RS-232 buffer
# area with a link-time override.
add_vice_test(bank1-call)
add_vice_test(bank1-call-rs232-area SOURCE bank1-call.c
  RESTORE_RANGE 0c00-0dff
  LINK_OPTIONS -Wl,--defsym=__c128_common_code_origin=0x0C00)
add_vice_test(bank1-state)
add_vice_test(bank1-copy)
add_vice_test(cache-objects)
add_vice_test(cache-shared)
add_vice_test(cache-tiers)
# -mreserve-zp: the gate and the bank-1 staging buffer take zero page the
# compiler cannot see (cache.h).
add_vice_test(cache-modules EXTRA_SOURCES cache-modules.s
  LINK_OPTIONS -mreserve-zp=29)
add_vice_test(bank1-isolation)
add_vice_test(bank1-rcr)
add_vice_test(bank1-malloc)
add_vice_test(bank1-static-data)
add_vice_test(bank1-malloc-stress)
# Heap tests shared with the C64 (../commodore).
add_vice_test(heap-growth SOURCE ../commodore/heap-growth.c)
add_vice_test(realloc SOURCE ../commodore/realloc.c)
add_vice_test(aligned-alloc SOURCE ../commodore/aligned-alloc.c)
add_vice_test(malloc-hook SOURCE ../commodore/malloc-hook.c)
add_vice_test(cxx-new-delete SOURCE ../commodore/cxx-new-delete.cc)
add_vice_test(bank1-large-data)
add_vice_test(common-code-area)

# Exit path in a program that is large (exit() above $4000): finishes and
# reports, and hands control back to BASIC.
add_vice_test(exit-large)
add_vice_basic_return_test(basic-return)
add_vice_basic_return_test(basic-return-large)

# A program that does not use the cache runtime must not link any of it.
foreach(t bank1-call heap-growth)
  add_test(NAME cache-not-linked-${t}
    COMMAND ${CMAKE_COMMAND} -DMAP=$<TARGET_FILE:${t}.prg>.map
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check-not-linked.cmake)
endforeach()

add_subdirectory(compile)
add_subdirectory(no-compile)
