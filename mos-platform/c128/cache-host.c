// Copyright 2026 LLVM-MOS Project
// Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
// See https://github.com/llvm-mos/llvm-mos-sdk/blob/main/LICENSE for license
// information.

// Host services for code modules (cache.h): C entry points behind the jump
// table in cache-host.s, reached through the call gate from module code in
// either bank. The caller's bank is the CR value the gate saved on entry.

#include "cache-internal.h"

extern const char __mos_host_tab[];
extern uint16_t __mos_mt_addr[];
extern uint8_t __mos_mt_cr[];
void mos_cache_set_host(uint8_t id) {
  __mos_mt_addr[id] = (uint16_t)__mos_host_tab;
  __mos_mt_cr[id] = MOS_CACHE_CR_BANK0;
}

uint8_t __mos_host_evict(uint8_t id) { return mos_cache_module_evict(id); }

// Returns the pointer as a 16-bit INTEGER: llvm-mos returns pointers in
// __rc2/__rc3 but integers in A/X, and module code reads A/X.
uint16_t __mos_host_lock(mos_handle_t handle) {
  return (uint16_t)mos_cache_lock_in(handle, (__mos_gate_cr & 0x40) ? 1 : 0);
}

void __mos_host_unlock(mos_handle_t handle) { mos_handle_unlock(handle); }

uint8_t __mos_host_defrag(void) { return mos_defrag(); }
