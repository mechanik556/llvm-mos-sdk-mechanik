// Copyright 2026 LLVM-MOS Project
// Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
// See https://github.com/llvm-mos/llvm-mos-sdk/blob/main/LICENSE for license
// information.

// The module table of a program that has no code modules: no entries. A
// program that has modules defines these symbols itself (cache.h, "Module table
// ABI"), and then this object is not linked. It is a separate library member,
// with ordinary (not weak) definitions, so that whole-program optimization sees
// that the table is empty and removes the module code from programs that do
// not use it.

#include <stdint.h>

const uint8_t __mos_mt_count = 0;
uint16_t __mos_mt_addr[1];
uint8_t __mos_mt_cr[1];
volatile uint8_t __mos_mt_active[1];
volatile uint8_t __mos_mt_ref[1];
uint16_t __mos_mt_stamp[1];
const uint16_t __mos_mt_img[1];
const uint16_t __mos_mt_size[1];
const uint16_t __mos_mt_reloc[1];
