// Copyright 2026 LLVM-MOS Project
// Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
// See https://github.com/llvm-mos/llvm-mos-sdk/blob/main/LICENSE for license
// information.

// Bank-aware object heap and code-module cache; see cache.h for the model and
// the API. This file is the runtime; cache-gate.s is the call gate that
// dispatches into modules, and cache-host.{c,s} the services module code can
// call back into.
//
// Two pools, one per RAM bank, each a run of equal-sized units tracked by a
// bitmap. Objects and modules are placed in units; an unlocked object or an
// inactive module can be moved, spilled to the other bank, or (modules) dropped
// to make room. Bank 0's pool is either caller-provided memory or one block of
// the malloc heap that shrinks and grows (shared mode).
//
// Module canonical form: a module's stored image (__mos_mt_img[id]) is
// assembled at its own address; all self-referential 16-bit values in it are
// "canonical" relative to that base. Loading at `dest` adds delta = dest -
// image at every relocation-table location; evicting SUBTRACTS delta from
// whatever value is currently there and writes the image back. Paired lo/hi
// fixups are combined into one 16-bit operation.
//
// This code is written for size: it is the memory manager of a machine with
// very little memory. Helpers that several callers share are kept out of line
// (NOINLINE), data is in parallel byte and word arrays rather than arrays of
// structures (an 8-bit index reaches a byte array directly), and 32-bit
// arithmetic and variable shifts of wide values are avoided.

#include "cache-internal.h"

#include <stdlib.h>
#include <string.h>

#define NOINLINE __attribute__((noinline))

// A unit index is 16 bits and NO_UNIT means "none". Bank 1's pool is about 44
// KB, which is 1408 units of the smallest unit size (32 bytes).
typedef uint16_t unit_t;
#define NO_UNIT 0xFFFF
#define MAX_UNITS 1536      // in bank 1's pool
#define MAX_BANK0_UNITS 254 // in bank 0's pool (its API takes bytes)
#define NOBJ 32
// Unit sizes are powers of two, 1 << shift bytes, chosen per bank with
// mos_cache_units; see cache.h for why the defaults are what they are.
#define MIN_SHIFT 5
#define MAX_SHIFT 10
#define DEFAULT_BANK0_SHIFT 5 // 32 bytes
#define DEFAULT_BANK1_SHIFT 8 // 256 bytes: a page, as in the tiers below
#define BANK1_LOW 0x1000      // first bank-1 address that is not Common RAM
#define BANK1_HIGH 0xC000     // where KERNAL ROM and I/O begin

// ---- Module table (supplied by the program; see cache.h) -------------------
// A program without modules gets the empty table of cache-nomodules.c, which
// lets the optimizer drop the module code.
extern const uint8_t __mos_mt_count;
extern uint16_t __mos_mt_addr[];
extern uint8_t __mos_mt_cr[];
extern volatile uint8_t __mos_mt_active[];
extern volatile uint8_t __mos_mt_ref[];
extern uint16_t __mos_mt_stamp[];
extern const uint16_t __mos_mt_img[];
extern const uint16_t __mos_mt_size[];
extern const uint16_t __mos_mt_reloc[];

#define NMODS __mos_mt_count
#define MOD_IN_BANK1(id) (__mos_mt_cr[id] == MOS_CACHE_CR_BANK1)
#define MOD_RESIDENT(id) (__mos_mt_img[id] && (__mos_mt_addr[id] >> 8))

// ---- Pools ------------------------------------------------------------------
static uint16_t pool_base[2];
static unit_t pool_units[2];
static unit_t pool_free[2]; // free units in each pool, kept by mark()
static uint8_t pool_shift[2] = {DEFAULT_BANK0_SHIFT, DEFAULT_BANK1_SHIFT};
static uint8_t bitmap0[(MAX_BANK0_UNITS + 7) / 8];
static uint8_t bitmap1[(MAX_UNITS + 7) / 8];
static uint8_t *const bitmap[2] = {bitmap0, bitmap1};
static uint8_t bank1_ready;
static uint8_t shared; // bank 0's pool is a block of the malloc heap
static uint16_t stamp_clock;

// Load and lock stamps are 16-bit counters that wrap; two stamps are compared
// modulo 2^16, which orders them correctly as long as the items being compared
// were stamped within 32767 events of each other.
static uint8_t older(uint16_t a, uint16_t b) { return (int16_t)(a - b) < 0; }

struct mos_cache_stats mos_cache_stats;

static const uint8_t bit_of[8] = {1, 2, 4, 8, 16, 32, 64, 128};

static uint8_t bit_used(uint8_t bank, unit_t u) {
  return bitmap[bank][u >> 3] & bit_of[u & 7];
}

// Mark `units` units from `start` used (1) or free (0).
static void mark(uint8_t bank, unit_t start, unit_t units, uint8_t used) {
  unit_t k;
  for (k = start; k < start + units; k++) {
    if (used)
      bitmap[bank][k >> 3] |= bit_of[k & 7];
    else
      bitmap[bank][k >> 3] &= ~bit_of[k & 7];
  }
  if (used)
    pool_free[bank] -= units;
  else
    pool_free[bank] += units;
}

// Unit arithmetic. These are shared by many callers, so they stay out of line:
// the variable shifts of 16-bit values they contain are long.
static NOINLINE unit_t units_of(uint8_t bank, uint16_t size) {
  unit_t units = size >> pool_shift[bank];
  if (size & ((1 << pool_shift[bank]) - 1))
    units++;
  return units > MAX_UNITS ? NO_UNIT : units; // NO_UNIT: fits no pool
}

static NOINLINE uint16_t unit_addr(uint8_t bank, unit_t idx) {
  return pool_base[bank] + (idx << pool_shift[bank]);
}

static NOINLINE unit_t unit_of(uint8_t bank, uint16_t addr) {
  return (addr - pool_base[bank]) >> pool_shift[bank];
}

// The bitmap is scanned a byte at a time where it can be: a byte of 8 used
// units (0xFF) is skipped at once, which matters for a pool of over a thousand
// units on a 6502.

// First-fit run of `units` free units in `bank`'s pool; returns unit index or
// NO_UNIT.
static NOINLINE unit_t alloc(uint8_t bank, unit_t units) {
  unit_t n = pool_units[bank], run = 0, u = 0;
  if (units == NO_UNIT)
    return NO_UNIT;
  while (u < n) {
    if (!(u & 7) && bitmap[bank][u >> 3] == 0xFF) {
      run = 0;
      u += 8;
      continue;
    }
    if (bit_used(bank, u)) {
      run = 0;
    } else if (++run == units) {
      unit_t start = u + 1 - units;
      mark(bank, start, units, 1);
      return start;
    }
    u++;
  }
  return NO_UNIT;
}

// Read or write a byte of a resident item's memory in either bank.
static uint8_t tmp8; // staging for bank 1 (which is only reached through calls)
static uint8_t get8(uint8_t bank, uint16_t a) {
  if (!bank)
    return *(const uint8_t *)a;
  c128_bank1_read(&tmp8, a, 1);
  return tmp8;
}
static void set8(uint8_t bank, uint16_t a, uint8_t v) {
  if (!bank) {
    *(uint8_t *)a = v;
    return;
  }
  tmp8 = v;
  c128_bank1_write(a, &tmp8, 1);
}

// Copy `size` bytes between arbitrary banks. Moving within a bank is safe for
// the overlapping move down that defragmentation does.
static NOINLINE void xcopy(uint8_t dbank, uint16_t dst, uint8_t sbank,
                           uint16_t src, uint16_t size) {
  if (!dbank && !sbank)
    memmove((void *)dst, (const void *)src, size);
  else if (dbank && sbank)
    __c128bank1_move(dst, src, size);
  else if (dbank)
    c128_bank1_write(dst, (const void *)src, size);
  else
    c128_bank1_read((void *)dst, src, size);
}

// Bank 1's default pool: everything above the static bank-1 content.
extern char __c128bank1_free_start[];
extern char __c128bank1_free_end[];

static void ensure_bank1(void) {
  uint16_t start, end, units;
  uint8_t shift = pool_shift[1];
  if (bank1_ready)
    return;
  end = (uint16_t)__c128bank1_free_end;
  // The configured unit size, or the smallest larger one with which the whole
  // region fits in MAX_UNITS units.
  for (;; shift++) {
    uint16_t mask = ((uint16_t)1 << shift) - 1;
    start = ((uint16_t)__c128bank1_free_start + mask) & ~mask;
    units = start < end ? (end - start) >> shift : 0;
    if (units <= MAX_UNITS || shift == MAX_SHIFT)
      break;
  }
  pool_base[1] = start;
  pool_units[1] = pool_free[1] = units > MAX_UNITS ? MAX_UNITS : units;
  pool_shift[1] = shift;
  bank1_ready = 1;
}

uint8_t mos_cache_units(uint8_t bank0_shift, uint8_t bank1_shift) {
  if (bank0_shift < MIN_SHIFT || bank0_shift > MAX_SHIFT ||
      bank1_shift < MIN_SHIFT || bank1_shift > MAX_SHIFT || pool_base[0] ||
      bank1_ready)
    return MOS_CACHE_BAD_ARGUMENT;
  pool_shift[0] = bank0_shift;
  pool_shift[1] = bank1_shift;
  return MOS_CACHE_OK;
}

uint8_t mos_cache_bank1(uint16_t base, uint16_t units, uint8_t unit_shift) {
  if (unit_shift < MIN_SHIFT || unit_shift > MAX_SHIFT || !units ||
      units > MAX_UNITS || bank1_ready)
    return MOS_CACHE_BAD_ARGUMENT;
  // Bank 1's $0000-$0FFF is Common RAM, the same memory as bank 0's, and
  // KERNAL ROM and I/O start at $C000: the pool must lie between them.
  if (base < BANK1_LOW || base > BANK1_HIGH ||
      units > (uint16_t)((BANK1_HIGH - base) >> unit_shift))
    return MOS_CACHE_BAD_ARGUMENT;
  pool_base[1] = base;
  pool_units[1] = pool_free[1] = units;
  pool_shift[1] = unit_shift;
  bank1_ready = 1;
  return MOS_CACHE_OK;
}

uint8_t mos_cache_static(void *pool, uint8_t units) {
  // The pool must fit in the address space without wrapping past $FFFF.
  if (!pool || !units || units > MAX_BANK0_UNITS || pool_base[0] ||
      units > (uint16_t)(-(uint16_t)pool) >> pool_shift[0])
    return MOS_CACHE_BAD_ARGUMENT;
  pool_base[0] = (uint16_t)pool;
  pool_units[0] = pool_free[0] = units;
  return MOS_CACHE_OK;
}

// ---- Modules ----------------------------------------------------------------

// Relocation table: repeated {1, off16} (FULL16) or {2, lo_off16, hi_off16}
// (paired LOW8/HIGH8), terminated by 0. Offsets are from the module base.
// Adds `delta` (mod 65536) to every listed value.
static NOINLINE void apply_relocs(uint8_t bank, uint16_t base, uint16_t table,
                                  uint16_t delta) {
  const uint8_t *p = (const uint8_t *)table;
  uint8_t kind;
  while ((kind = *p++)) {
    // The value's low byte is at a, its high byte at b (a + 1 for a whole
    // 16-bit value, else a second operand).
    uint16_t a = base + (p[0] | ((uint16_t)p[1] << 8)), b = a + 1, v;
    p += 2;
    if (kind != 1) {
      b = base + (p[0] | ((uint16_t)p[1] << 8));
      p += 2;
    }
    v = (get8(bank, a) | ((uint16_t)get8(bank, b) << 8)) + delta;
    set8(bank, a, (uint8_t)v);
    set8(bank, b, (uint8_t)(v >> 8));
  }
}

// Called with the id of every module evicted (a diagnostic hook; weak, doing
// nothing by default). Evictions also happen inside malloc's reclaim path, so
// the hook may run there and must obey the same rules (cache.h).
__attribute__((weak)) void mos_cache_on_evict(uint8_t id) { (void)id; }

NOINLINE uint8_t mos_cache_module_evict(uint8_t id) {
  uint16_t addr, size;
  uint8_t bank;
  if (id >= NMODS)
    return MOS_CACHE_NO_SUCH_MODULE;
  addr = __mos_mt_addr[id];
  size = __mos_mt_size[id];
  bank = MOD_IN_BANK1(id);
  if (!__mos_mt_img[id])
    return MOS_CACHE_STATIC_MODULE;
  if (!(addr >> 8))
    return MOS_CACHE_NOT_RESIDENT;
  if (__mos_mt_active[id])
    return MOS_CACHE_PINNED;
  // Un-relocate in place by delta subtraction, then write the canonical image
  // back to the backing store (the program image here).
  apply_relocs(bank, addr, __mos_mt_reloc[id],
               (uint16_t)(__mos_mt_img[id] - addr));
  if (bank)
    c128_bank1_read((void *)__mos_mt_img[id], addr, size);
  else
    memcpy((void *)__mos_mt_img[id], (const void *)addr, size);
  mark(bank, unit_of(bank, addr), units_of(bank, size), 0);
  __mos_mt_addr[id] = 0;
  __mos_mt_cr[id] = 0;
  mos_cache_stats.mod_evictions++;
  mos_cache_on_evict(id);
  return MOS_CACHE_OK;
}

// Oldest resident, non-static, inactive module in `bank` (optionally only those
// whose reference bit is clear), or 0xFF.
static uint8_t oldest(uint8_t bank, uint8_t cold_only) {
  uint8_t id, best = 0xFF;
  for (id = 0; id < NMODS; id++) {
    if (!MOD_RESIDENT(id) || __mos_mt_active[id] || MOD_IN_BANK1(id) != bank ||
        (cold_only && __mos_mt_ref[id]))
      continue;
    if (best == 0xFF || older(__mos_mt_stamp[id], __mos_mt_stamp[best]))
      best = id;
  }
  return best;
}

// CLOCK-style victim choice: the oldest candidate not used since the last
// sweep; if every candidate was used, the sweep clears all reference bits
// (second chance) and the oldest is taken. 0xFF if there is no candidate.
static NOINLINE uint8_t find_victim(uint8_t bank) {
  uint8_t v = oldest(bank, 1), id;
  if (v != 0xFF)
    return v;
  for (id = 0; id < NMODS; id++)
    __mos_mt_ref[id] = 0;
  return oldest(bank, 0);
}

// ---- Objects ----------------------------------------------------------------

// The object table, in parallel arrays. An object is live if F_USED is set;
// F_BANK1 says which bank it is in; F_TRIED is scratch for spill_one.
static uint16_t ho_addr[NOBJ], ho_size[NOBJ], ho_stamp[NOBJ];
static uint8_t ho_lock[NOBJ], ho_flags[NOBJ];
#define F_USED 1
#define F_BANK1 2
#define F_TRIED 4
#define OBJ_BANK(i) ((ho_flags[i] & F_BANK1) != 0)
static uint16_t ostamp; // last-lock stamp source (LRU for objects)

static uint8_t valid_handle(mos_cache_handle_t handle) {
  return handle && handle <= NOBJ && (ho_flags[handle - 1] & F_USED);
}

static void poll_point(void);

// The items in a pool are the resident modules and the live objects. If module
// (or object) `i` is in `bank`, these return 1 and set it_start (its first
// unit), it_len (its length in units) and it_pinned (an active module or a
// locked object: it cannot move or be evicted). The results are in variables,
// not through pointers: the runtime is not re-entrant, and pointers to locals
// would force them onto the slow software stack. Modules and objects have
// separate functions so that a program with no modules carries none of the
// module code.
static unit_t it_start, it_len;
static uint8_t it_pinned;

static NOINLINE uint8_t mod_info(uint8_t i, uint8_t bank) {
  if (!MOD_RESIDENT(i) || MOD_IN_BANK1(i) != bank)
    return 0;
  it_pinned = __mos_mt_active[i] != 0;
  it_start = unit_of(bank, __mos_mt_addr[i]);
  it_len = units_of(bank, __mos_mt_size[i]);
  return 1;
}

static NOINLINE uint8_t obj_info(uint8_t i, uint8_t bank) {
  if (!(ho_flags[i] & F_USED) || OBJ_BANK(i) != bank)
    return 0;
  it_pinned = ho_lock[i] != 0;
  it_start = unit_of(bank, ho_addr[i]);
  it_len = units_of(bank, ho_size[i]);
  return 1;
}

// Make room in `bank` without discarding anything: move the least recently
// locked unlocked object that fits into the OTHER bank's current free space
// over there. Returns 1 if an object was moved. (Only the other bank is a place
// an object can go until an REU/disk tier exists; if it has no room this
// returns 0 and the caller falls through to evicting modules, and finally to
// failure.)
static NOINLINE uint8_t spill_one(uint8_t bank) {
  uint8_t h, best, moved = 0;
  for (;;) {
    unit_t idx;
    best = 0xFF;
    for (h = 0; h < NOBJ; h++) {
      if ((ho_flags[h] & (F_USED | F_BANK1 | F_TRIED)) !=
              (F_USED | (bank ? F_BANK1 : 0)) ||
          ho_lock[h])
        continue;
      if (best == 0xFF || older(ho_stamp[h], ho_stamp[best]))
        best = h;
    }
    if (best == 0xFF)
      break;
    ho_flags[best] |= F_TRIED;
    idx = alloc(!bank, units_of(!bank, ho_size[best]));
    if (idx == NO_UNIT)
      continue; // does not fit over there: try the next
    xcopy(!bank, unit_addr(!bank, idx), bank, ho_addr[best], ho_size[best]);
    mark(bank, unit_of(bank, ho_addr[best]), units_of(bank, ho_size[best]), 0);
    ho_addr[best] = unit_addr(!bank, idx);
    ho_flags[best] ^= F_BANK1;
    mos_cache_stats.obj_spills++;
    moved = 1;
    break;
  }
  for (h = 0; h < NOBJ; h++)
    ho_flags[h] &= ~F_TRIED;
  return moved;
}

uint16_t mos_cache_free_units(uint8_t bank) {
  bank = bank != 0;
  if (bank)
    ensure_bank1();
  return pool_free[bank];
}

// Longest run of free units.
uint16_t mos_cache_max_run(uint8_t bank) {
  unit_t u, run = 0, best = 0;
  bank = bank != 0;
  if (bank)
    ensure_bank1();
  for (u = 0; u < pool_units[bank]; u++) {
    if (bit_used(bank, u))
      run = 0;
    else if (++run > best)
      best = run;
  }
  return best;
}

// FEASIBILITY: the longest run of units in `bank` NOT held by a pinned item (an
// active module or a locked object). Pinned items can never move or be evicted,
// so no amount of spilling/eviction/defragmentation can produce a bigger
// contiguous run than this. Found by walking the pinned items in address order
// and taking the largest gap between them; no map of them is built.
#define GAP_PICK                                                               \
  if (it_pinned && it_start >= cursor && (bs == NO_UNIT || it_start < bs)) {   \
    bs = it_start;                                                             \
    bn = it_len;                                                               \
  }

static NOINLINE unit_t max_gap(uint8_t bank) {
  unit_t cursor = 0, best = 0;
  for (;;) {
    unit_t bs = NO_UNIT, bn = 0, gap;
    uint8_t i;
    for (i = 0; i < NMODS; i++)
      if (mod_info(i, bank))
        GAP_PICK
    for (i = 0; i < NOBJ; i++)
      if (obj_info(i, bank))
        GAP_PICK
    gap = (bs == NO_UNIT ? pool_units[bank] : bs) - cursor;
    if (gap > best)
      best = gap;
    if (bs == NO_UNIT)
      return best;
    cursor = bs + (bn ? bn : 1);
  }
}

// DEFRAGMENTATION of one bank's pool: slide every unpinned resident (module or
// object) down to the end of the previous item, so free space collects into as
// few runs as the pinned items allow. Modules are relocated by the move delta
// (added to their current values, so self-modified operands stay correct);
// objects are plain copies (their handles do not change). Returns the number of
// items moved.
#define DEFRAG_PICK(KIND)                                                      \
  if (it_start >= cursor && (bs == NO_UNIT || it_start < bs)) {                \
    bs = it_start;                                                             \
    bn = it_len;                                                               \
    bpinned = it_pinned;                                                       \
    bkind = KIND;                                                              \
    bidx = i;                                                                  \
  }

static NOINLINE uint8_t defrag(uint8_t bank) {
  unit_t cursor = 0;
  uint8_t moved = 0;
  for (;;) {
    unit_t bs = NO_UNIT, bn = 0;
    uint8_t i, bpinned = 0, bkind = 1, bidx = 0;
    uint16_t oldaddr, newaddr;
    for (i = 0; i < NMODS; i++)
      if (mod_info(i, bank))
        DEFRAG_PICK(0)
    for (i = 0; i < NOBJ; i++)
      if (obj_info(i, bank))
        DEFRAG_PICK(1)
    if (bs == NO_UNIT)
      break;
    if (bpinned || bs == cursor) {
      cursor = bs + (bn ? bn : 1);
      continue;
    }
    newaddr = unit_addr(bank, cursor);
    if (!bkind) {
      oldaddr = __mos_mt_addr[bidx];
      xcopy(bank, newaddr, bank, oldaddr, __mos_mt_size[bidx]);
      apply_relocs(bank, newaddr, __mos_mt_reloc[bidx],
                   (uint16_t)(newaddr - oldaddr));
      __mos_mt_addr[bidx] = newaddr;
    } else {
      oldaddr = ho_addr[bidx];
      xcopy(bank, newaddr, bank, oldaddr, ho_size[bidx]);
      ho_addr[bidx] = newaddr;
    }
    mark(bank, bs, bn, 0);
    mark(bank, cursor, bn, 1);
    cursor += bn;
    moved++;
    mos_cache_stats.defrag_moves++;
  }
  return moved;
}

// alloc, and if the bank has enough free units in total but no contiguous run,
// defragment it first.
static NOINLINE unit_t alloc_defrag(uint8_t bank, unit_t units) {
  unit_t idx = alloc(bank, units);
  if (idx != NO_UNIT)
    return idx;
  if (pool_free[bank] >= units && defrag(bank))
    idx = alloc(bank, units);
  return idx;
}

static uint8_t sh_want; // bank 0 was too small for a request (shared mode)

// Find room for `size` bytes, preferring `first` bank:
//  0. free space in the preferred bank, then the other;
//  1. defragment a bank that has enough free units in total;
//  2. only then make room: spill an unlocked object to the other bank
//     (lossless) or evict the oldest cold module, defragmenting whenever the
//     freed total is enough, until it fits.
// A FEASIBILITY check runs first: a request larger than the longest run not
// held by pinned items in every allowed bank is refused up front, with no
// spills, evictions or moves. Returns the unit index (or NO_UNIT) and sets
// *bank_out. `only` = 0xFF means either bank, else that bank only.
static uint8_t feasible(uint8_t bank, uint8_t only, uint16_t size) {
  unit_t units = units_of(bank, size);
  return (only == 0xFF || only == bank) && units <= pool_units[bank] &&
         max_gap(bank) >= units;
}

static unit_t place(uint8_t first, uint8_t only, uint16_t size,
                    uint8_t *bank_out) {
  uint8_t k, pass, bank = 0, ok0, ok1;
  unit_t idx = NO_UNIT, units;
  ensure_bank1();
  ok0 = feasible(0, only, size);
  ok1 = feasible(1, only, size);
  *bank_out = 0;
  if (!ok0 && !ok1) {
    mos_cache_stats.place_refused++;
    return NO_UNIT;
  }
  for (pass = 0; pass < 3; pass++) {
    for (k = 0; k < 2 && idx == NO_UNIT; k++) {
      bank = k ? !first : first;
      if (!(bank ? ok1 : ok0)) {
        if (pass == 0 && !bank)
          sh_want = 1; // bank 0 cannot take it now: wants to grow (shared mode)
        continue;
      }
      units = units_of(bank, size);
      if (pass == 0) {
        idx = alloc(bank, units);
        if (idx == NO_UNIT && !bank)
          sh_want = 1; // bank 0 was full: wants to grow (shared mode)
      } else if (pass == 1) {
        idx = alloc_defrag(bank, units);
      } else {
        // Spill / evict, defragmenting whenever the freed total is enough.
        for (;;) {
          uint8_t victim;
          idx = alloc_defrag(bank, units);
          if (idx != NO_UNIT)
            break;
          if (spill_one(bank))
            continue; // lossless: an object moved across
          victim = find_victim(bank);
          if (victim == 0xFF)
            break;
          mos_cache_module_evict(victim);
        }
      }
    }
  }
  *bank_out = bank;
  return idx;
}

uint8_t mos_cache_module_load(uint8_t id, uint8_t caller_bank) {
  uint16_t size, dest;
  uint8_t bank = 0;
  unit_t idx;
  if (id >= NMODS)
    return MOS_CACHE_NO_SUCH_MODULE;
  if (!__mos_mt_img[id])
    return MOS_CACHE_STATIC_MODULE;
  if (MOD_RESIDENT(id))
    return MOS_CACHE_OK;
  size = __mos_mt_size[id];
  idx = place(caller_bank ? 1 : 0, 0xFF, size, &bank);
  if (idx == NO_UNIT)
    return MOS_CACHE_NO_ROOM;
  dest = unit_addr(bank, idx);
  if (bank)
    c128_bank1_write(dest, (const void *)__mos_mt_img[id], size);
  else
    memcpy((void *)dest, (const void *)__mos_mt_img[id], size);
  apply_relocs(bank, dest, __mos_mt_reloc[id],
               (uint16_t)(dest - __mos_mt_img[id]));
  __mos_mt_addr[id] = dest;
  __mos_mt_cr[id] = bank ? MOS_CACHE_CR_BANK1 : MOS_CACHE_CR_BANK0;
  __mos_mt_stamp[id] = ++stamp_clock;
  mos_cache_stats.mod_loads++;
  return MOS_CACHE_OK;
}

mos_cache_handle_t mos_cache_malloc(uint16_t size) {
  uint8_t h, bank = 0;
  unit_t idx;
  poll_point();
  for (h = 0; h < NOBJ && (ho_flags[h] & F_USED); h++) {
  }
  if (h == NOBJ || !size)
    return 0;
  idx = place(0, 0xFF, size, &bank);
  if (idx == NO_UNIT)
    return 0;
  ho_addr[h] = unit_addr(bank, idx);
  ho_size[h] = size;
  ho_flags[h] = F_USED | (bank ? F_BANK1 : 0);
  ho_lock[h] = 0;
  ho_stamp[h] = ++ostamp;
  return h + 1;
}

uint8_t mos_cache_free(mos_cache_handle_t handle) {
  uint8_t i = handle - 1;
  if (!valid_handle(handle))
    return MOS_CACHE_INVALID_HANDLE;
  if (ho_lock[i])
    return MOS_CACHE_LOCKED;
  mark(OBJ_BANK(i), unit_of(OBJ_BANK(i), ho_addr[i]),
       units_of(OBJ_BANK(i), ho_size[i]), 0);
  ho_flags[i] = 0;
  return MOS_CACHE_OK;
}

// mos_cache_lock cannot switch $FF00 and hand back a bank-1 pointer: the
// caller's own code (ordinary bank-0 RAM, not Common RAM) would vanish. Instead
// a lock makes the object resident in the CALLER'S OWN bank, migrating it
// through the Common-RAM staging routines if it is in the other one; no bank
// switch is held across caller code. A lock held by one bank's code pins the
// object there, so the other bank's lock attempt fails (NULL).
void *mos_cache_lock_in(mos_cache_handle_t handle, uint8_t caller_bank) {
  uint8_t i = handle - 1, nb = 0, from;
  unit_t idx;
  uint16_t dst;
  caller_bank = caller_bank != 0;
  poll_point();
  if (!valid_handle(handle))
    return 0;
  from = OBJ_BANK(i);
  if (from != caller_bank) {
    if (ho_lock[i])
      return 0;
    idx = place(caller_bank, caller_bank, ho_size[i], &nb);
    if (idx == NO_UNIT)
      return 0;
    dst = unit_addr(caller_bank, idx);
    xcopy(caller_bank, dst, from, ho_addr[i], ho_size[i]);
    mark(from, unit_of(from, ho_addr[i]), units_of(from, ho_size[i]), 0);
    ho_addr[i] = dst;
    ho_flags[i] ^= F_BANK1;
  }
  ho_lock[i]++;
  ho_stamp[i] = ++ostamp;
  return (void *)ho_addr[i];
}

void *mos_cache_lock(mos_cache_handle_t handle) {
  return mos_cache_lock_in(handle, 0);
}

void mos_cache_unlock(mos_cache_handle_t handle) {
  if (valid_handle(handle) && ho_lock[handle - 1])
    ho_lock[handle - 1]--;
}

// Defragment both banks' pools; returns the number of items moved. Active
// modules and locked objects are never moved.
uint8_t mos_cache_defrag(void) {
  ensure_bank1();
  return defrag(0) + defrag(1);
}

uint16_t mos_cache_pool_units(uint8_t bank) {
  bank = bank != 0;
  if (bank)
    ensure_bank1();
  return pool_units[bank];
}
uint16_t mos_cache_pool_base(uint8_t bank) {
  bank = bank != 0;
  if (bank)
    ensure_bank1();
  return pool_base[bank];
}
uint8_t mos_cache_handle_bank(mos_cache_handle_t handle) {
  return valid_handle(handle) ? OBJ_BANK(handle - 1) : MOS_CACHE_INVALID_BANK;
}
uint8_t mos_cache_handle_locks(mos_cache_handle_t handle) {
  return valid_handle(handle) ? ho_lock[handle - 1] : 0;
}

// ---- Shared mode: bank 0's pool is a block of the ordinary malloc heap ------
// The cache is the polite party; malloc knows nothing about it.
//  Polling: mos_cache_service(), called at safe points, keeps the heap's free
//    bytes between two watermarks - it gives pool tail back when the heap runs
//    low and takes it again when there is plenty and the cache is short of
//    bank-0 room.
//  Reclaim: __malloc_low_memory, called by malloc itself when a request cannot
//    be satisfied, makes the cache give back what is needed right now. It does
//    NO I/O and calls no allocator entry other than realloc-shrink, which never
//    allocates: it spills objects to bank 1, drops cold modules and shrinks the
//    pool block in place. Whatever is not cheap to give up in that way (disk
//    demotion of dirty items) is done at the polling safe points, never here.

size_t __heap_bytes_free(void);

static uint8_t sh_min, sh_max, sh_busy;
static uint16_t sh_low, sh_high; // free-byte watermarks of the malloc heap

// Default (weak) tier extension points: nothing to demote to, nothing to write.
__attribute__((weak)) uint8_t mos_cache_tier_demote(uint8_t bank) {
  (void)bank;
  return 0;
}
__attribute__((weak)) void mos_cache_tier_writebehind(void) {}

// True if any active module or locked object is in bank 0's pool: it cannot
// move, so the pool block must not be moved either.
static uint8_t pool0_pinned(void) {
  uint8_t i;
  for (i = 0; i < NMODS; i++)
    if (mod_info(i, 0) && it_pinned)
      return 1;
  for (i = 0; i < NOBJ; i++)
    if (obj_info(i, 0) && it_pinned)
      return 1;
  return 0;
}

static unit_t used_top(void) { // highest used unit of bank 0, plus one
  unit_t u = pool_units[0];
  while (u && !bit_used(0, u - 1))
    u--;
  return u;
}

// Ask the tier extension point to free room in bank 0. A tier that claims to
// have done so without freeing any unit is ignored, so that a faulty one
// cannot make the caller loop forever.
static uint8_t demoted(void) {
  unit_t before = pool_free[0];
  return mos_cache_tier_demote(0) && pool_free[0] > before;
}

// Give up to `want` units of bank 0's pool tail back to the heap; returns the
// number given. Never goes below sh_min units.
static uint8_t pool0_yield(uint8_t want) {
  uint8_t given = 0;
  sh_busy = 1;
  while (given < want && pool_units[0] > sh_min) {
    unit_t top = used_top(), lowest = top > sh_min ? top : sh_min, room;
    uint8_t n, victim;
    room = pool_units[0] - lowest;
    if (!room && pool_free[0]) { // free units, but not at the end
      defrag(0);
      top = used_top();
      lowest = top > sh_min ? top : sh_min;
      room = pool_units[0] - lowest;
    }
    if (!room) {        // make some: cheapest first
      if (spill_one(0)) // object -> bank 1 (lossless)
        continue;
      if (demoted()) // object -> REU (when it exists)
        continue;
      victim = find_victim(0); // clean module: just drop it
      if (victim == 0xFF)
        break; // only pinned items are left
      mos_cache_module_evict(victim);
      continue;
    }
    n = (uint8_t)(want - given);
    if (n > room)
      n = (uint8_t)room;
    // Shrinking never moves the block.
    if (realloc((void *)pool_base[0], (uint16_t)(pool_units[0] - n)
                                          << pool_shift[0]) !=
        (void *)pool_base[0])
      break;
    pool_units[0] -= n;
    pool_free[0] -= n;
    given += n;
    mos_cache_stats.yielded_units += n;
  }
  sh_busy = 0;
  return given;
}

// Take `add` more units for bank 0's pool from the heap. The block may have to
// move, so this is refused while anything in bank 0 is pinned; when it does
// move, every resident module in it is relocated by the move delta and every
// object's address adjusted (handles do not change).
static uint8_t pool0_grow(uint8_t add) {
  unit_t cur = pool_units[0];
  uint8_t id;
  uint8_t *np;
  uint16_t old = pool_base[0], delta;
  if (!add || cur + add > sh_max || pool0_pinned())
    return 0;
  sh_busy = 1;
  np = realloc((void *)old, (uint16_t)(cur + add) << pool_shift[0]);
  sh_busy = 0;
  if (!np)
    return 0;
  if ((uint16_t)np != old) {
    delta = (uint16_t)np - old;
    for (id = 0; id < NMODS; id++) {
      if (!MOD_RESIDENT(id) || MOD_IN_BANK1(id))
        continue;
      __mos_mt_addr[id] += delta;
      apply_relocs(0, __mos_mt_addr[id], __mos_mt_reloc[id], delta);
    }
    for (id = 0; id < NOBJ; id++)
      if ((ho_flags[id] & (F_USED | F_BANK1)) == F_USED)
        ho_addr[id] += delta;
    pool_base[0] = (uint16_t)np;
    mos_cache_stats.pool_moves++;
  }
  pool_units[0] = cur + add;
  pool_free[0] += add;
  mos_cache_stats.grown_units += add;
  return add;
}

uint8_t mos_cache_shared(uint8_t min_units, uint8_t init_units,
                         uint8_t max_units, uint16_t low, uint16_t high) {
  uint8_t *p;
  // The largest pool must fit in 64 KB (its size must not wrap).
  if (pool_base[0] || !min_units || min_units > init_units ||
      init_units > max_units || max_units > MAX_BANK0_UNITS || low >= high ||
      max_units > 0xFFFF >> pool_shift[0])
    return MOS_CACHE_BAD_ARGUMENT;
  sh_busy = 1;
  p = malloc((uint16_t)init_units << pool_shift[0]);
  sh_busy = 0;
  if (!p)
    return MOS_CACHE_NO_MEMORY;
  pool_base[0] = (uint16_t)p;
  pool_units[0] = pool_free[0] = init_units;
  sh_min = min_units;
  sh_max = max_units;
  sh_low = low;
  sh_high = high;
  shared = 1;
  return MOS_CACHE_OK;
}

// Units needed to give back or take: at most `cap`, and 255 at the very most.
static uint8_t clamp_units(size_t units, uint8_t cap) {
  return units > cap ? cap : (uint8_t)units;
}

uint8_t mos_cache_service(void) {
  size_t free;
  uint8_t changed = 0;
  mos_cache_stats.services++;
  if (sh_busy)
    return 0;
  if (!shared) { // static mode: only the tier's write-behind has work to do
    mos_cache_tier_writebehind();
    return 0;
  }
  free = __heap_bytes_free();
  if (free < sh_low) {
    changed = pool0_yield(clamp_units(((sh_low - free) >> pool_shift[0]) + 1,
                                      255)) != 0;
  } else if (sh_want && free > sh_high) {
    uint8_t add = clamp_units((free - sh_high) >> pool_shift[0], 4);
    if (add > sh_max - pool_units[0]) // a few units per call, up to the maximum
      add = sh_max - pool_units[0];
    changed = pool0_grow(add) != 0;
    if (!add || pool_units[0] == sh_max)
      sh_want = 0;
  }
  mos_cache_tier_writebehind();
  return changed;
}

// Automatic polling: the safe points of the runtime (allocating an object,
// locking one) call poll_point(), which runs mos_cache_service every `every`th
// time. Module loads are deliberately not safe points: they run inside the call
// gate with interrupts disabled, where a tier's write-behind must not run. A
// module's own code being active only pins the pool: growth is refused,
// yielding is not.
static uint8_t sh_every, sh_tick;
void mos_cache_auto_poll(uint8_t every) {
  sh_every = every;
  sh_tick = 0;
}
static void poll_point(void) {
  if (sh_every && ++sh_tick >= sh_every) {
    sh_tick = 0;
    mos_cache_service();
  }
}

// Reclaim: malloc found nothing that fits a chunk of `needed` bytes. Yield
// enough of the pool to fit it, if possible; no I/O (see above).
int __malloc_low_memory(size_t needed) {
  mos_cache_stats.hook_calls++;
  if (!shared || sh_busy)
    return 0;
  return pool0_yield(clamp_units((needed >> pool_shift[0]) + 1, 255)) != 0;
}
