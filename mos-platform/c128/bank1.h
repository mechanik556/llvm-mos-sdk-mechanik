// Copyright 2026 LLVM-MOS Project
// Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
// See https://github.com/llvm-mos/llvm-mos-sdk/blob/main/LICENSE for license
// information.

// Support for statically placing code/data in the C128's second 64KB RAM
// bank ("bank 1") and calling into it from ordinary bank-0-resident code.
//
// Bank 1 is reached through c128_bank1_call, not through a raw jump/call -
// the CPU can only directly execute whichever bank is currently mapped in,
// so calling into bank-1-resident code requires switching the MMU's
// Configuration Register first and switching it back afterward.
//
// Data placed in bank 1 (MOS_C128_BANK1_DATA) can't be dereferenced
// directly from bank-0-resident code any more than the code can be called
// directly - write an accessor function (placed with MOS_C128_BANK1_CODE,
// invoked via c128_bank1_call) instead of taking a raw pointer to it.

#ifndef _C128_BANK1_H
#define _C128_BANK1_H

#if !defined(__C128__)
#  error This module may only be used when compiling for the C128!
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Switch to RAM bank 1 (KERNAL ROM/I-O stay mapped in), call method, then
// switch back to whatever bank was mapped before the call. Interrupts are
// disabled for the duration of the switched-away call.
//
// method must itself be placed in bank 1 via MOS_C128_BANK1_CODE (or be
// reachable from code that is) - calling a bank-0-resident function this
// way is safe but pointless, since no switch was actually needed.
__attribute__((leaf, callback(1))) void c128_bank1_call(void (*method)(void));

#ifdef __cplusplus
}
#endif

// Place a function or global in bank 1, reachable via c128_bank1_call
// (for functions) or a bank-1-placed accessor function (for data - see
// this header's own top comment).
#define MOS_C128_BANK1_CODE __attribute__((section(".c128bank1.text")))
#define MOS_C128_BANK1_DATA __attribute__((section(".c128bank1.data")))

#endif // _C128_BANK1_H
