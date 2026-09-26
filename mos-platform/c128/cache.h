// Copyright 2026 LLVM-MOS Project
// Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
// See https://github.com/llvm-mos/llvm-mos-sdk/blob/main/LICENSE for license
// information.

// A bank-aware object heap for the C128, and its cooperation with malloc.
//
// The C128's second 64KB RAM bank ("bank 1", see bank1.h) cannot be reached
// through ordinary pointers, and malloc/free/new/delete cannot be made to hand
// out bank-1 memory: a pointer does not say which bank it refers to, and bank-1
// memory can only be touched by Common-RAM code with interrupts off. This heap
// is therefore a separate, explicit API built on handles instead of pointers:
//
//   mos_cache_handle_t h = mos_cache_malloc(100);
//   unsigned char *p = mos_cache_lock(h);   // valid until the matching unlock
//   p[0] = 1;
//   mos_cache_unlock(h);
//   mos_cache_free(h);
//
// Because a handle is never a pointer, the runtime is free to move the bytes
// while the object is unlocked: it keeps objects in bank 0 or bank 1, moves an
// unlocked object to the other bank to make room, defragments, and (with the
// later tiers) can evict it further. mos_cache_lock makes the object resident
// in bank 0 and returns a pointer valid until the matching unlock; a locked
// object never moves.
//
// The runtime shares bank 0 with the ordinary heap. Ordinary malloc/free/new/
// delete are unchanged: their blocks never move and are never tracked. The
// runtime is the polite party - it yields to malloc, never the reverse, and it
// never does I/O inside malloc:
//
// * Static mode (mos_cache_static): bank 0's pool is a block of memory you
//   provide. Nothing else to know.
// * Shared mode (mos_cache_shared): bank 0's pool is one block of the malloc
//   heap that shrinks when the program needs memory and regrows when it is
//   spare. mos_cache_service() (or mos_cache_auto_poll) keeps free heap space
//   between two watermarks, and this library's __malloc_low_memory hook makes
//   the pool give back what a failing allocation needs (spill unlocked objects
//   to bank 1, drop modules, defragment, shrink in place). Neither path does
//   any KERNAL or disk I/O. The library defines __malloc_low_memory itself
//   (overriding the weak default in stdlib.h), so a program that links this
//   runtime cannot also define its own hook; it can call the runtime's
//   mos_cache_service instead.
//
// Bank 1's pool is by default the space above statically placed bank-1 content
// (__c128bank1_free_start/_end, link.ld), managed here; mos_cache_bank1
// chooses another region. Bank-1 memory managed here must not also be handed to
// anything else. C128 BASIC 7 keeps its variables in bank 1 (from $0400 up), so
// a program that writes bank 1 and then returns to BASIC must not expect the
// BASIC variables of the program that started it to survive.
//
// What using any of this costs. The runtime moves data to and from bank 1
// through the bank-1 support of bank1.h, so a program that calls it links that
// support even if it never places anything in bank 1 itself: the Common-RAM
// code area at $0800 (saved at startup and restored at exit, see bank1.h) and a
// 16-byte staging buffer in zero page. Programs that do not use the runtime pay
// nothing. The call gate adds about 240 bytes to the Common-RAM code area (512
// bytes by default, of which bank-1 support itself uses about 60).
//
// RAM. The runtime's own data is about 500 bytes: the object table (256), and
// the allocation maps (32 for bank 0 and 192 for bank 1, sized for the largest
// pool, though the default 256-byte units need 22) plus a little state.
//
// Zero page. The call gate (linked only by programs that use it) costs 13 more
// bytes. The zero-page pool is about 102 bytes shared with the compiler's own
// use (-mlto-zp=102 in the platform configuration), and the compiler does not
// know about zero page that assembly or library code takes. A program that uses
// the gate and lets the compiler use all of it fails to link with "section
// '.zp.bss' will not fit in region 'zp'". Tell the compiler to leave room when
// linking such a program:
//   -mreserve-zp=29     (16 for bank 1 + 13 for the gate; 16 without the gate)
//
// Unit sizes. Each pool is a run of equal units (a power of two, 32 to 1024
// bytes), and an object or module occupies whole units, so its size is rounded
// up. The unit size of each bank is chosen with mos_cache_units; the defaults
// are 32 bytes in bank 0 and 256 bytes in bank 1:
//  - Bank 0 is scarce and shares the malloc heap. 32 bytes wastes at most 31
//    bytes per item and lets the pool give memory back to malloc in small steps
//    (cc65's malloc, the usual C heap on the C64 and C128, is byte-granular
//    with a 6-byte minimum block; nothing on these machines needs bigger
//    units).
//  - Bank 1 is roomy (about 44 KB) and is the tier next to the REU and the
//    disk, whose natural units are pages and sectors: C64 OS allocates memory
//    in 256-byte pages, a 1541 sector holds 254 bytes of data (256 raw), and
//    the planned REU tier allocates 256-byte pages. 256-byte units keep the
//    allocation map at 22 bytes and make later demotion whole-page. A program
//    that keeps many small objects in bank 1 (less than a page each) can ask
//    for 32 or 64.
// This choice rests on how the C64 and C128 ecosystem lays out memory (cc65's
// heap, C64 OS's memory manager, the Commodore disk format), not on measured
// object-size histograms, which no open-source C64/C128 program is known to
// publish.
//
// Limits. At most 32 objects live at once; bank 0's pool has at most 254 units
// and bank 1's at most 1536; the gate nests at most 16 module calls deep; the
// counters in mos_cache_stats are 8 bits and wrap.
//
// Not thread-safe and not interrupt-safe: call from ordinary code, not from an
// IRQ or NMI handler.
//
// Code modules (optional). A program that supplies a module table (see "Module
// table ABI" below) lets the runtime load, relocate, evict and defragment
// relocatable code modules in the same pools; cache-gate.s provides the
// __mos_call_gate that dispatches into them. Without a table, none of that is
// linked.

#ifndef _C128_CACHE_H
#define _C128_CACHE_H

#if !defined(__C128__)
#error This module may only be used when compiling for the C128!
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// A handle to a cacheable object. 0 is the null handle.
typedef uint16_t mos_cache_handle_t;

// ---- Result codes -----------------------------------------------------------

/// Every function that reports a status returns MOS_CACHE_OK (0) on success.
#define MOS_CACHE_OK 0
/// mos_cache_static, mos_cache_shared, mos_cache_bank1: a bad argument, or the
/// pool was already set up (or, for bank 1, already used).
#define MOS_CACHE_BAD_ARGUMENT 1
/// mos_cache_shared: malloc has no room for the initial pool block.
#define MOS_CACHE_NO_MEMORY 2
/// mos_cache_free: the object is locked; or the handle is not valid.
#define MOS_CACHE_LOCKED 1
#define MOS_CACHE_INVALID_HANDLE 2
/// mos_cache_module_load and the call gate: no room for the module; the gate's
/// active-module stack is full (gate only); no such module.
#define MOS_CACHE_NO_ROOM 1
#define MOS_CACHE_TOO_DEEP 2
#define MOS_CACHE_NO_SUCH_MODULE 3
/// mos_cache_module_evict: the module is active; not resident; static. (No such
/// module is MOS_CACHE_NO_SUCH_MODULE. mos_cache_module_load also returns
/// MOS_CACHE_STATIC_MODULE for a static module.)
#define MOS_CACHE_PINNED 1
#define MOS_CACHE_NOT_RESIDENT 2
#define MOS_CACHE_STATIC_MODULE 4
/// mos_cache_handle_bank: the handle is not valid.
#define MOS_CACHE_INVALID_BANK 0xFF

// ---- Setup (call once, before any other function here) ---------------------

/// Choose the unit size of each bank's pool: 1 << bank0_shift and 1 <<
/// bank1_shift bytes, each shift from 5 to 10 (32 to 1024 bytes). The defaults,
/// used if this is not called, are 5 and 8 (see "Unit sizes" above). Only
/// before the pools are set up: before mos_cache_static or mos_cache_shared,
/// and before bank 1's pool is chosen (mos_cache_bank1) or first used. Returns
/// MOS_CACHE_OK or MOS_CACHE_BAD_ARGUMENT.
uint8_t mos_cache_units(uint8_t bank0_shift, uint8_t bank1_shift);

/// Bank 0's pool is `units` units (of bank 0's unit size) of memory at `pool`,
/// which must stay valid and unused by anything else. `units` is at most 254
/// and the pool at most 64 KB. Returns MOS_CACHE_OK or MOS_CACHE_BAD_ARGUMENT.
uint8_t mos_cache_static(void *pool, uint8_t units);

/// Bank 0's pool is a block of the malloc heap: `init_units` units (of bank 0's
/// unit size) to start with, never fewer than `min_units` or more than
/// `max_units` (at most 254, and at most 64 KB), and mos_cache_service keeps
/// the heap's free bytes at least `low` (yielding pool space if not) and, when
/// it grows the pool, at least `high` afterwards. Requires `low` < `high`. Only
/// while bank 0's pool is unused. Returns MOS_CACHE_OK, MOS_CACHE_BAD_ARGUMENT
/// (also if a pool is already set up) or MOS_CACHE_NO_MEMORY (malloc has no
/// room for the initial block).
uint8_t mos_cache_shared(uint8_t min_units, uint8_t init_units,
                         uint8_t max_units, uint16_t low, uint16_t high);

/// Choose bank 1's pool: `units` units of (1 << unit_shift) bytes at the bank-1
/// address `base` (5 <= unit_shift <= 10; units at most 1536). The default,
/// used if this is not called, is all of bank 1 above statically placed content
/// in bank 1's unit size (or a larger one if it would need more than 1536
/// units). The pool must lie in bank 1's
/// $1000-$BFFF (below $1000 is Common RAM, shared with bank 0; KERNAL ROM and
/// I/O begin at $C000). Returns MOS_CACHE_OK or MOS_CACHE_BAD_ARGUMENT (also if
/// the pool was already chosen or used).
uint8_t mos_cache_bank1(uint16_t base, uint16_t units, uint8_t unit_shift);

// ---- Objects ----------------------------------------------------------------

/// Allocate an object of `size` bytes, in bank 0 if there is room, else bank 1,
/// making room by moving unlocked objects and dropping modules if it must.
/// Returns 0 if it cannot be placed. The contents are not initialized. A safe
/// point for automatic polling (mos_cache_auto_poll).
mos_cache_handle_t mos_cache_malloc(uint16_t size);

/// Free an object. Returns MOS_CACHE_OK, MOS_CACHE_LOCKED or
/// MOS_CACHE_INVALID_HANDLE.
uint8_t mos_cache_free(mos_cache_handle_t handle);

/// Make the object resident in bank 0 and return a pointer to it, valid until
/// the matching mos_cache_unlock; locks nest. Returns NULL for an invalid
/// handle or if there is no room to bring it into bank 0. A safe point for
/// automatic polling.
void *mos_cache_lock(mos_cache_handle_t handle);

/// Release one lock. Invalid handles and unlocked objects are ignored.
void mos_cache_unlock(mos_cache_handle_t handle);

/// Slide every unlocked object (and module) in both pools together so free
/// space forms as few runs as the locked items allow. Objects are copied;
/// handles do not change. Returns the number of items moved. (Allocation
/// defragments by itself when it has to.)
uint8_t mos_cache_defrag(void);

// ---- Cooperation with malloc (shared mode) ----------------------------------

/// Keep the heap's free bytes between the watermarks: give pool space back when
/// they are below `low`, take it back (up to the maximum) when the pool was
/// short of room and they are above `high`. Growing may move the pool block,
/// which is refused while an object is locked or a module active in bank 0.
/// Returns 1 if the pool changed size. In static mode there is no pool to
/// resize, and it only gives mos_cache_tier_writebehind its turn.
uint8_t mos_cache_service(void);

/// Call mos_cache_service automatically every `every`th time the runtime
/// reaches one of its own safe points (allocating an object, locking one). 0
/// (the default) turns it off. Module loads are not safe points: they happen
/// inside the call gate with interrupts disabled.
void mos_cache_auto_poll(uint8_t every);

/// Extension points for storage tiers this library does not implement, both
/// weak and by default doing nothing. mos_cache_tier_demote is called when bank
/// 0's pool must give up space and spilling to bank 1 has failed: move some
/// unlocked object out of `bank` to an I/O-free faster tier (an REU) and return
/// non-zero if it freed room (a claim that no pool unit was freed is ignored).
/// It runs inside malloc, so it must not do KERNAL or disk I/O and must not
/// call malloc. mos_cache_tier_writebehind is called only from
/// mos_cache_service, never from inside malloc, and only from ordinary code:
/// this is where slow-tier (disk) writes belong.
uint8_t mos_cache_tier_demote(uint8_t bank);
void mos_cache_tier_writebehind(void);

// ---- Diagnostics ------------------------------------------------------------

struct mos_cache_stats {
  uint8_t mod_loads, mod_evictions; // modules
  uint8_t defrag_moves;             // items moved by defragmentation
  uint8_t place_refused;            // requests refused up front (cannot fit)
  uint8_t obj_spills;               // objects moved to the other bank
  uint8_t hook_calls;               // __malloc_low_memory calls
  uint8_t yielded_units;            // pool units given back to the heap
  uint8_t grown_units;              // pool units taken from the heap
  uint8_t pool_moves;               // pool block moves on growth
  uint8_t services;                 // mos_cache_service calls
};
extern struct mos_cache_stats mos_cache_stats;

/// Pool geometry: size in units, and the address of unit 0, of `bank` (0 or 1).
uint16_t mos_cache_pool_units(uint8_t bank);
uint16_t mos_cache_pool_base(uint8_t bank);
/// Free units in a pool, and its longest run of free units.
uint16_t mos_cache_free_units(uint8_t bank);
uint16_t mos_cache_max_run(uint8_t bank);
/// Which bank an object is in now (MOS_CACHE_INVALID_BANK for an invalid
/// handle), and how many locks it holds. Any non-zero `bank` argument of the
/// functions above means bank 1.
uint8_t mos_cache_handle_bank(mos_cache_handle_t handle);
uint8_t mos_cache_handle_locks(mos_cache_handle_t handle);

// ---- Module table ABI (version 1)
// --------------------------------------------
//
// A program that uses code modules defines these symbols (a hand-written module
// set today; the toolchain will generate them). Entry i describes module i;
// there are __mos_mt_count entries of each array. All addresses are bank-0
// addresses of the canonical image, which is assembled at its own address; the
// loader relocates it. Each image must be readable for `size` bytes, and lie in
// writable RAM: eviction writes the un-relocated image back to it.
//
// clang-format off
//   const uint8_t    __mos_mt_count;     number of entries
//   uint16_t         __mos_mt_addr[];    resident address; high byte 0 if not
//   uint8_t          __mos_mt_cr[];      $FF00 to run it under ($0E bank 0,
//                                        $4E bank 1)
//   volatile uint8_t __mos_mt_active[];  active-call count (gate maintains it)
//   volatile uint8_t __mos_mt_ref[];     CLOCK reference bit (gate sets it)
//   uint16_t         __mos_mt_stamp[];   load order (runtime maintains it)
//   const uint16_t   __mos_mt_img[];     canonical image address; 0 = static
//                                        (never loaded or evicted)
//   const uint16_t   __mos_mt_size[];    image size in bytes
//   const uint16_t   __mos_mt_reloc[];   relocation table address
// clang-format on
//
// Relocation table: repeated {1, off16} (a 16-bit value at base+off16) or
// {2, lo_off16, hi_off16} (a value split across a LOW8 and a HIGH8 operand),
// offsets from the module base, terminated by 0. The loader adds the load delta
// to each; eviction subtracts it from whatever value is there now (so
// self-modified operands stay correct) and writes the image back.

/// Load module `id` for code running in `caller_bank` (0 or 1: which bank to
/// prefer). Returns MOS_CACHE_OK (also if it is already resident),
/// MOS_CACHE_NO_ROOM, MOS_CACHE_NO_SUCH_MODULE or MOS_CACHE_STATIC_MODULE. (The
/// call gate reports the same codes, and MOS_CACHE_TOO_DEEP.)
uint8_t mos_cache_module_load(uint8_t id, uint8_t caller_bank);

/// Evict module `id`: un-relocate it and write its canonical image back to
/// __mos_mt_img[id], so that must be writable RAM. Returns MOS_CACHE_OK,
/// MOS_CACHE_PINNED (it is active), MOS_CACHE_NOT_RESIDENT,
/// MOS_CACHE_STATIC_MODULE or MOS_CACHE_NO_SUCH_MODULE.
uint8_t mos_cache_module_evict(uint8_t id);

/// Diagnostic hook, weak and doing nothing by default: called with the id of
/// each module the runtime evicts. Evictions also happen inside malloc's
/// reclaim path (see __malloc_low_memory), so the hook can run there and must
/// obey the same rules: no I/O, no allocation.
void mos_cache_on_evict(uint8_t id);

// ---- Host services for modules ----------------------------------------------
//
// Module code can call back into the runtime through the gate, like any module,
// by naming a *host* entry of the module table: a static entry (image address
// 0) whose "code" is the runtime's jump table. Reserve one table entry for it
// and call mos_cache_set_host(id) once at startup (this links the jump table,
// cache-host.s, only into programs that ask for it). From module code, with the
// host at index `id` (CALL is `jsr __mos_call_gate; .byte id; .word offset`):
//
// clang-format off
//   CALL id, 0   evict module A -> A = the mos_cache_module_evict code
//   CALL id, 3   lock the object A (handle low byte, X = high byte) in the
//                caller's bank -> pointer in A (low) / X (high), 0 if refused
//   CALL id, 6   unlock the object A/X
//   CALL id, 9   defragment both pools -> A = items moved
// clang-format on
//
// Arguments and results use the registers as the gate passes them; __rc2-__rc4
// may be used between calls.

/// Register table entry `id` as the host. It must be a static entry (image 0).
void mos_cache_set_host(uint8_t id);

/// mos_cache_lock for code that runs in `caller_bank`: the object is made
/// resident in that bank.
void *mos_cache_lock_in(mos_cache_handle_t handle, uint8_t caller_bank);

#ifdef __cplusplus
}
#endif

#endif // _C128_CACHE_H
