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
//   mos_handle_t h = mos_cacheable_malloc(100);
//   unsigned char *p = mos_handle_lock(h);   // valid until the matching unlock
//   p[0] = 1;
//   mos_handle_unlock(h);
//   mos_cacheable_free(h);
//
// Because a handle is never a pointer, the runtime is free to move the bytes
// while the object is unlocked: it keeps objects in bank 0 or bank 1, moves an
// unlocked object to the other bank to make room, defragments, and (with the
// later tiers) can evict it further. mos_handle_lock makes the object resident
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
//   any KERNAL or disk I/O.
//
// Bank 1's pool is by default the space above statically placed bank-1 content
// (__c128bank1_free_start/_end, link.ld), managed here; mos_cache_bank1
// chooses another region. Bank-1 memory managed here must not also be handed to
// anything else.
//
// Zero page. Using bank 1 costs 16 bytes of the zero-page pool (bank1.h); the
// call gate (linked only by programs that use call_gate) costs 13 more. The
// pool is ~102 bytes shared with the compiler's own use (-mlto-zp=102 in the
// platform configuration). A program that uses the gate and lets the compiler
// use all of it fails to link with "section '.zp.bss' will not fit in region
// 'zp'"; compile and link it with a smaller budget, e.g. -mlto-zp=70.
//
// Not thread-safe and not interrupt-safe: call from ordinary code, not from an
// IRQ or NMI handler.
//
// Code modules (optional). A program that supplies a module table (see
// "Module table ABI" below) lets the runtime load, relocate, evict and
// defragment relocatable code modules in the same pools; cache-gate.s provides
// the call_gate that dispatches into them. Without a table, none of that is
// linked.

#ifndef _C128_CACHE_H
#define _C128_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// A handle to a cacheable object. 0 is the null handle.
typedef uint16_t mos_handle_t;

// ---- Setup (call once, before any other function here) ---------------------

/// Bank 0's pool is `units` 32-byte units of memory at `pool` (which must stay
/// valid and unused by anything else). Returns 0, or 1 for a bad argument.
/// `units` is at most 254.
uint8_t mos_cache_static(void *pool, uint8_t units);

/// Bank 0's pool is a block of the malloc heap: `init_units` 32-byte units to
/// start with, never fewer than `min_units` or more than `max_units` (at most
/// 254), and mos_cache_service keeps the heap's free bytes at least `low`
/// (yielding pool space if not) and, when it grows the pool, at least `high`
/// afterwards. Use `low` < `high`. Only while bank 0's pool is unused.
/// Returns 0, 1 for a bad argument or if a pool is already set up, 2 if malloc
/// has no room for the initial block.
uint8_t mos_cache_shared(uint8_t min_units, uint8_t init_units,
                         uint8_t max_units, uint16_t low, uint16_t high);

/// Choose bank 1's pool: `units` units of (1 << unit_shift) bytes at the bank-1
/// address `base` (5 <= unit_shift <= 10; units at most 254). The default, used
/// if this is not called, is all of bank 1 above statically placed content, in
/// 128-byte units. Returns 0, or 1 for a bad argument.
uint8_t mos_cache_bank1(uint16_t base, uint8_t units, uint8_t unit_shift);

// ---- Objects ----------------------------------------------------------------

/// Allocate an object of `size` bytes, in bank 0 if there is room, else bank 1,
/// making room by moving unlocked objects and dropping modules if it must.
/// Returns 0 if it cannot be placed. The contents are not initialized.
mos_handle_t mos_cacheable_malloc(uint16_t size);

/// Free an object. Returns 0, 1 if it is locked, 2 for an invalid handle.
uint8_t mos_cacheable_free(mos_handle_t handle);

/// Make the object resident in bank 0 and return a pointer to it, valid until
/// the matching mos_handle_unlock; locks nest. Returns NULL for an invalid
/// handle or if there is no room to bring it into bank 0.
void *mos_handle_lock(mos_handle_t handle);

/// Release one lock.
void mos_handle_unlock(mos_handle_t handle);

/// Slide every unlocked object (and module) in both pools together so free
/// space forms as few runs as the locked items allow. Objects are copied;
/// handles do not change. Returns the number of items moved. (Allocation
/// defragments by itself when it has to.)
uint8_t mos_defrag(void);

// ---- Cooperation with malloc (shared mode) ----------------------------------

/// Keep the heap's free bytes between the watermarks: give pool space back when
/// they are below `low`, take it back (up to the maximum) when the pool was
/// short of room and they are above `high`. Growing may move the pool block,
/// which is refused while an object is locked or a module active in bank 0.
/// Returns 1 if the pool changed size. Does nothing in static mode.
uint8_t mos_cache_service(void);

/// Call mos_cache_service automatically every `every`th time the runtime
/// reaches one of its own safe points (allocating an object, locking one,
/// loading a module). 0 (the default) turns it off.
void mos_cache_auto_poll(uint8_t every);

/// Extension points for storage tiers this library does not implement, both
/// weak and by default doing nothing. mos_tier_demote is called when bank 0's
/// pool must give up space and spilling to bank 1 has failed: move some
/// unlocked object out of `bank` to an I/O-free faster tier (an REU) and return
/// non-zero if it freed room. It runs inside malloc, so it must not do KERNAL
/// or disk I/O and must not call malloc. mos_tier_writebehind is called only
/// from mos_cache_service, never from inside malloc: this is where slow-tier
/// (disk) writes belong.
uint8_t mos_tier_demote(uint8_t bank);
void mos_tier_writebehind(void);

// ---- Diagnostics
// -------------------------------------------------------------

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
uint8_t mos_cache_pool_units(uint8_t bank);
uint16_t mos_cache_pool_base(uint8_t bank);
/// Free units in a pool, and its longest run of free units.
uint8_t mos_cache_free_units(uint8_t bank);
uint8_t mos_cache_max_run(uint8_t bank);
/// Which bank an object is in now, and how many locks it holds.
uint8_t mos_handle_bank(mos_handle_t handle);
uint8_t mos_handle_locks(mos_handle_t handle);

// ---- Module table ABI (version 1)
// -----------------------------------------------
//
// A program that uses code modules defines these symbols (a hand-written module
// set today; the toolchain will generate them). Entry i describes module i;
// `count` entries each. All addresses are bank-0 addresses of the canonical
// image, which is assembled at its own address; the loader relocates it.
//
//   const uint8_t  __mos_mt_count;      number of entries
//   uint16_t       __mos_mt_addr[];     resident address, high byte 0 if not
//   resident uint8_t        __mos_mt_cr[];       $FF00 value to run it under
//   ($0E bank 0, $4E bank 1) volatile uint8_t __mos_mt_active[]; active-call
//   count (the gate maintains it) volatile uint8_t __mos_mt_ref[];    CLOCK
//   reference bit (the gate sets it) uint16_t       __mos_mt_stamp[];    load
//   order (the runtime maintains it) const uint16_t __mos_mt_img[]; canonical
//   image address; 0 = static (never loaded/evicted) const uint16_t
//   __mos_mt_size[];     image size in bytes const uint16_t __mos_mt_reloc[];
//   relocation table address
//
// Relocation table: repeated {1, off16} (a 16-bit value at base+off16) or
// {2, lo_off16, hi_off16} (a value split across a LOW8 and a HIGH8 operand),
// offsets from the module base, terminated by 0. The loader adds the load delta
// to each; eviction subtracts it from whatever value is there now (so
// self-modified operands stay correct) and writes the image back.

/// Load module `id` for code running in `caller_bank` (0 or 1: which bank to
/// prefer). Returns 0, or 1 if it cannot be placed.
uint8_t mos_cache_module_load(uint8_t id, uint8_t caller_bank);

/// Evict module `id`. Returns 0, 1 if it is active (pinned), 2 if it is not
/// resident, 3 if it is static.
uint8_t mos_cache_module_evict(uint8_t id);

/// mos_handle_lock for code that runs in `caller_bank`: the object is made
/// resident in that bank.
void *mos_cache_lock_in(mos_handle_t handle, uint8_t caller_bank);

#ifdef __cplusplus
}
#endif

#endif // _C128_CACHE_H
