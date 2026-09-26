// Copyright 2026 LLVM-MOS Project
// Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
// See https://github.com/llvm-mos/llvm-mos-sdk/blob/main/LICENSE for license
// information.

// Declarations shared between the C128 platform library's own sources (bank
// 1 support, the cache runtime, and their assembly). Not installed.

#ifndef _C128_CACHE_INTERNAL_H
#define _C128_CACHE_INTERNAL_H

#include <stdint.h>

// The platform library is built without __C128__ defined, so the public
// headers (which insist on it) are not used for its own prototypes.
#ifndef __C128__
#define __C128__ 1
#endif

#include "cache.h"

// Run-time copies between bank 0 and bank 1 (bank1.h, bank1-load.c).
void c128_bank1_write(unsigned short bank1_dest, const void *src,
                      unsigned short size);
void c128_bank1_read(void *dest, unsigned short bank1_src, unsigned short size);

// Entry points behind the host jump table (cache-host.s), reached through the
// call gate.
uint8_t __mos_host_evict(uint8_t id);
uint16_t __mos_host_lock(mos_cache_handle_t handle);
void __mos_host_unlock(mos_cache_handle_t handle);
uint8_t __mos_host_defrag(void);

// The caller's $FF00 as the call gate saved it on entry (zero page).
extern volatile uint8_t __mos_gate_cr;

// Values of $FF00 that select bank 0 or bank 1 with KERNAL ROM and I/O mapped
// in (MMU_CFG_RAM0_KERNAL and MMU_CFG_RAM1_KERNAL in c128.inc).
#define MOS_CACHE_CR_BANK0 0x0E
#define MOS_CACHE_CR_BANK1 0x4E

#endif // _C128_CACHE_INTERNAL_H
