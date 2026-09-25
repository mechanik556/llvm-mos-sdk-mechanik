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
// Usage pattern (see test/c128/bank1-*.c):
//
//   MOS_C128_BANK1_DATA static volatile unsigned char table[64];
//   static volatile unsigned char __attribute__((section(".zp.bss"))) result;
//
//   MOS_C128_BANK1_CODE static void get_entry(void) { result = table[3]; }
//
//   c128_bank1_call(get_entry);   // from ordinary code; then read `result`
//
// Rules - all follow from "while bank 1 is mapped, the CPU sees bank 1's
// RAM for everything except Common RAM ($0000-$0FFF, incl. zero page/stack)":
//
// * Accessors, not pointers. Bank-1 data can't be dereferenced from
//   bank-0 code (the same address reads bank 0's RAM). Write an accessor
//   function placed with MOS_C128_BANK1_CODE and invoke it via
//   c128_bank1_call; never pass or return raw pointers to bank-1 data.
//
// * Bank 1 is not part of the C heap, and cannot be made to look like it.
//   malloc/free/new/delete hand out ordinary bank-0 memory. No change to them
//   could return bank-1 memory transparently:
//   - A pointer is a flat 16-bit address and does not say which bank it
//     refers to; the same value reads different memory once bank 1 is mapped
//     (see test/c128/bank1-isolation.c).
//   - The only way to touch bank 1 is code that is visible in both banks
//     (Common RAM) running with interrupts off: about 105 cycles to read one
//     byte through c128_bank1_call, against about 4 for an ordinary load
//     (measured under VICE).
//   - At a given dereference the compiler cannot know whether a pointer
//     refers to bank-1 memory. C and C++ let pointers be converted (void *,
//     char *, uintptr_t, unions), copied as raw bytes, stored inside other
//     objects, compared, handed to prebuilt library code, and dereferenced
//     arbitrarily later. Every access through a pointer that might be a
//     bank-1 pointer would need a run-time check and a gate.
//   - Wide ("far") pointers or a bank-qualified address space would change
//     the ABI and every library, and still cost a gated call per access.
//   So bank-1 data is reached through accessor functions, as above. A
//   bank-aware allocator would have to be a separate, explicit API returning
//   a handle that yields a pointer only while locked, never a pointer that
//   stays valid forever.
//
// * Pass values through Common RAM. c128_bank1_call takes no arguments for
//   the callee and returns nothing (registers are not preserved across the
//   switch-back). Exchange data via zero-page variables
//   (__attribute__((section(".zp.bss")))) - the only ordinary variables
//   both banks see. The zero-page pool is small (~100 bytes, shared with
//   the compiler's own use).
//
// * Bank-1 code must be self-contained. Ordinary bank-0 code (libc such
//   as memcpy/printf, KERNAL wrappers, any function not marked
//   MOS_C128_BANK1_CODE, and compiler-generated runtime calls such as
//   multiply/divide helpers) is NOT reachable while bank 1 is mapped;
//   calling it executes whatever bank 1 holds at that address. Keep bank-1
//   functions to simple leaf code. Also avoid the software/static stack
//   (locals, spills, stack-passed arguments) and ordinary globals: their
//   memory is in bank 0 at a non-common address, so bank-1 code silently
//   reads and writes bank 1's own memory at that same address instead
//   (confirmed by test/c128/bank1-isolation.c). Locals work as private scratch,
//   but that address may fall inside bank-1 content and corrupt it. Not
//   enforced by the toolchain; use zero page or MOS_C128_BANK1_DATA.
//
// * Interrupts are disabled for the duration of each call; keep calls
//   short. Only IRQs are masked: an NMI (e.g. the RESTORE key) can still
//   occur. The KERNAL ROM stays mapped, so its handler works, but an NMI
//   handler in ordinary RAM would not be reachable while bank 1 is mapped.
//
// * Declare bank-1 globals volatile if you need to be sure they are really
//   loaded/stored: LTO may otherwise constant-fold a never-written global.
//
// * The small Common-RAM trampoline code lives by default at the low end of
//   BASIC's runtime stack ($0800, ~60 bytes of a 512-byte area). That
//   stack is only used by the BASIC interpreter, so bare-metal programs -
//   including ones using RS-232, tape, or sprites - are unaffected; the
//   original bytes are saved at startup and restored at exit. A program
//   that calls into the BASIC interpreter must move it, e.g.
//     -Wl,--defsym=__c128_common_code_origin=0x0C00   (RS-232 buffers)
//   (also 0x0B00 tape buffer, 0x0E00 sprite definitions; see link.ld for
//   the full list and what is NOT usable). The area must be inside
//   $0000-$0FFF.
//
// * Initialized and zero-initialized bank-1 data are populated at program
//   startup (bank1-load.c). Bank-1 memory is bank1's $1000-$BFFF.
//
// * Using bank 1 costs 16 bytes of the zero-page pool: the startup copy
//   stages its data through a zero-page buffer (the only ordinary variable
//   storage that is Common RAM).

#ifndef _C128_BANK1_H
#define _C128_BANK1_H

#if !defined(__C128__)
#error This module may only be used when compiling for the C128!
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Switch to RAM bank 1 (KERNAL ROM/I-O stay mapped in), call method, then
/// switch back to whatever bank was mapped before the call. Interrupts are
/// disabled for the duration of the switched-away call.
///
/// method must itself be placed in bank 1 via MOS_C128_BANK1_CODE. A function
/// in ordinary bank-0 memory is not reachable once bank 1 is mapped - the CPU
/// would run whatever bank 1 holds at that address. Only code in Common RAM
/// ($0000-$0FFF) is visible from both banks.
__attribute__((leaf, callback(1))) void c128_bank1_call(void (*method)(void));

#ifdef __cplusplus
}
#endif

/// Place a function or global in bank 1, reachable via c128_bank1_call
/// (for functions) or a bank-1-placed accessor function (for data - see
/// this header's own top comment).
#define MOS_C128_BANK1_CODE __attribute__((section(".c128bank1.text")))
#define MOS_C128_BANK1_DATA __attribute__((section(".c128bank1.data")))

#endif // _C128_BANK1_H
