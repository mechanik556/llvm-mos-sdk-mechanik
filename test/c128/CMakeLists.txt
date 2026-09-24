cmake_minimum_required(VERSION 3.18)

project(test-c128 LANGUAGES C)

include(../test.cmake)

# Common-RAM code area (bank1.h): the default, and moved to the RS-232 buffer
# area with a link-time override.
add_vice_test(bank1-call)
add_vice_test(bank1-call-rs232-area SOURCE bank1-call.c
  RESTORE_RANGE 0c00-0dff
  LINK_OPTIONS -Wl,--defsym=__c128_common_code_origin=0x0C00)
add_vice_test(bank1-state)
add_vice_test(bank1-isolation)
add_vice_test(bank1-large-data)
add_vice_test(common-code-area)

# Exit path in a program that is large (exit() above $4000): finishes and
# reports, and hands control back to BASIC.
add_vice_test(exit-large)
add_vice_basic_return_test(basic-return)
add_vice_basic_return_test(basic-return-large)

add_subdirectory(compile)
add_subdirectory(no-compile)
