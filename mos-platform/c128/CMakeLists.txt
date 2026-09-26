platform(c128 COMPLETE HOSTED PARENT commodore)

if(NOT CMAKE_CROSSCOMPILING)
  return()
endif()

install(FILES
  ../c64/_6526.h
  ../c64/_sid.h
  _vdc.h
  ../c64/_vic2.h
  c128.h
  bank1.h
  cache.h
  ../c64/charset.h
TYPE INCLUDE)
install(FILES link.ld TYPE LIB)

add_platform_library(c128-crt0)
merge_libraries(c128-crt0
  common-init-stack
  common-exit-loop
)

install(FILES c128.inc DESTINATION ${ASMINCDIR})

add_platform_object_file(c128-basic-header basic-header.o basic-header.S)
add_platform_object_file(c128-init-mmu init-mmu.o init-mmu.S)
target_include_directories(c128-init-mmu PRIVATE .)

add_platform_library(c128-c
  c128.c
  devnum.s
  kernal.S
  bank1.s
  bank1-load.c
  cache.c
  cache-nomodules.c
  cache-gate.s
  cache-host.c
  cache-host.s
)
# The cache runtime is the memory manager of a machine with very little memory:
# optimize it for the smallest code (-Oz), which is a little smaller than the
# SDK's default -Os for this code.
set_source_files_properties(cache.c cache-host.c cache-nomodules.c bank1-load.c
  PROPERTIES COMPILE_OPTIONS -Oz)
target_include_directories(c128-c BEFORE PUBLIC .)
# The platform library is built without the platform's clang config, so it does
# not get this from there; the headers it shares with user code insist on it.
target_compile_definitions(c128-c PRIVATE __C128__)
target_include_directories(c128-c BEFORE PUBLIC ../c64)
target_link_libraries(c128-c PRIVATE common-asminc)

# The same runtime built with 16-bit unit indices (MOS_CACHE_WIDE), as a separate
# library, libcache-wide.a: link it (-lcache-wide) ahead of the default library to
# use pools of more than 248 units, i.e. unit sizes below 256 bytes over all of
# bank 1. It is larger, which is why it is not the default (cache.h).
add_platform_library(c128-cache-wide cache.c)
# (Linking c128-c gets it the include paths of the libraries merged into that.)
target_link_libraries(c128-cache-wide PRIVATE c128-c)
target_compile_definitions(c128-cache-wide PRIVATE __C128__ MOS_CACHE_WIDE)

