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

#include "cache-internal.h"

#include <stdlib.h>
#include <string.h>

// A unit index is 16 bits and NO_UNIT means "none". Bank 1's pool is about 44
// KB, which is 1408 units of the smallest unit size (32 bytes).
typedef uint16_t unit_t;
#define NO_UNIT 0xFFFF
#define MAX_UNITS 1536      // in bank 1's pool
#define MAX_BANK0_UNITS 254 // in bank 0's pool (its API takes bytes)
#define NOBJ 32
_Static_assert(NOBJ <= 32, "spill_one tracks objects in a 32-bit mask");
// Unit sizes are powers of two, 1 << shift bytes, chosen per bank with
// mos_cache_units; see cache.h for why the defaults are what they are.
#define MIN_SHIFT 5
#define MAX_SHIFT 10
#define DEFAULT_BANK0_SHIFT 5 // 32 bytes
#define DEFAULT_BANK1_SHIFT 8 // 256 bytes: a page, as in the tiers below
#define BANK1_LOW 0x1000L     // first bank-1 address that is not Common RAM
#define BANK1_HIGH 0xC000L    // where KERNAL ROM and I/O begin

// ---- Module table (supplied by the program; see cache.h) -------------------
// Weak defaults describe "no modules", so a program without a module table
// links and the module code below is dead.
extern const uint8_t __mos_mt_count;
extern uint16_t __mos_mt_addr[];
extern uint8_t __mos_mt_cr[];
extern volatile uint8_t __mos_mt_active[];
extern volatile uint8_t __mos_mt_ref[];
extern uint16_t __mos_mt_stamp[];
extern const uint16_t __mos_mt_img[];
extern const uint16_t __mos_mt_size[];
extern const uint16_t __mos_mt_reloc[];

__attribute__((weak)) const uint8_t __mos_mt_count = 0;
__attribute__((weak)) uint16_t __mos_mt_addr[1];
__attribute__((weak)) uint8_t __mos_mt_cr[1];
__attribute__((weak)) volatile uint8_t __mos_mt_active[1];
__attribute__((weak)) volatile uint8_t __mos_mt_ref[1];
__attribute__((weak)) uint16_t __mos_mt_stamp[1];
__attribute__((weak)) const uint16_t __mos_mt_img[1];
__attribute__((weak)) const uint16_t __mos_mt_size[1];
__attribute__((weak)) const uint16_t __mos_mt_reloc[1];

#define NMODS __mos_mt_count
#define MOD_IN_BANK1(id) (__mos_mt_cr[id] == MOS_CACHE_CR_BANK1)
#define MOD_RESIDENT(id) (__mos_mt_img[id] && (__mos_mt_addr[id] >> 8))

// ---- Pools ------------------------------------------------------------------
static uint16_t pool_base[2];
static unit_t pool_units[2];
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

static uint8_t bit_used(uint8_t bank, unit_t u) {
  return bitmap[bank][u >> 3] & (1 << (u & 7));
}

// Units needed for `size` bytes in `bank`; NO_UNIT if it cannot fit any pool.
static unit_t units_of(uint8_t bank, uint16_t size) {
  uint16_t unit = (uint16_t)1 << pool_shift[bank];
  uint16_t units = (size >> pool_shift[bank]) + ((size & (unit - 1)) != 0);
  return units > MAX_UNITS ? NO_UNIT : units;
}

static uint16_t unit_addr(uint8_t bank, unit_t idx) {
  return pool_base[bank] + (idx << pool_shift[bank]);
}

static unit_t unit_of(uint8_t bank, uint16_t addr) {
  return (addr - pool_base[bank]) >> pool_shift[bank];
}

// The bitmap is scanned a byte at a time where it can be: a byte of 8 used
// units (0xFF) is skipped at once, which matters for a pool of over a thousand
// units on a 6502.

// First-fit run of `units` free units in `bank`'s pool; returns unit index or
// NO_UNIT.
static unit_t alloc(uint8_t bank, unit_t units) {
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
      unit_t start = u + 1 - units, k;
      for (k = start; k <= u; k++)
        bitmap[bank][k >> 3] |= 1 << (k & 7);
      return start;
    }
    u++;
  }
  return NO_UNIT;
}

static void mark(uint8_t bank, unit_t start, unit_t units) {
  unit_t k;
  for (k = start; k < start + units; k++)
    bitmap[bank][k >> 3] |= 1 << (k & 7);
}

static void release(uint8_t bank, unit_t start, unit_t units) {
  unit_t k;
  for (k = start; k < start + units; k++)
    bitmap[bank][k >> 3] &= ~(1 << (k & 7));
}

// Byte/word access to a resident item's memory in either bank.
static uint8_t get8(uint8_t bank, uint16_t a) {
  uint8_t v;
  if (bank)
    c128_bank1_read(&v, a, 1);
  else
    v = *(volatile uint8_t *)a;
  return v;
}
static void set8(uint8_t bank, uint16_t a, uint8_t v) {
  if (bank)
    c128_bank1_write(a, &v, 1);
  else
    *(volatile uint8_t *)a = v;
}
static uint16_t get16(uint8_t bank, uint16_t a) {
  return get8(bank, a) | ((uint16_t)get8(bank, a + 1) << 8);
}
static void set16(uint8_t bank, uint16_t a, uint16_t v) {
  set8(bank, a, (uint8_t)v);
  set8(bank, a + 1, (uint8_t)(v >> 8));
}

// Copy between arbitrary banks in 16-byte chunks via a local buffer.
static void xcopy(uint8_t dbank, uint16_t dst, uint8_t sbank, uint16_t src,
                  uint16_t size) {
  uint8_t buf[16];
  while (size) {
    uint8_t n = size > 16 ? 16 : (uint8_t)size;
    if (sbank)
      c128_bank1_read(buf, src, n);
    else
      memcpy(buf, (const void *)src, n);
    if (dbank)
      c128_bank1_write(dst, buf, n);
    else
      memcpy((void *)dst, buf, n);
    dst += n;
    src += n;
    size -= n;
  }
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
  pool_units[1] = units > MAX_UNITS ? MAX_UNITS : units;
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
  uint32_t end = (uint32_t)base + ((uint32_t)units << unit_shift);
  if (unit_shift < MIN_SHIFT || unit_shift > MAX_SHIFT || units > MAX_UNITS ||
      bank1_ready)
    return MOS_CACHE_BAD_ARGUMENT;
  // Bank 1's $0000-$0FFF is Common RAM, the same memory as bank 0's, and
  // KERNAL ROM and I/O start at $C000: the pool must lie between them.
  if (base < BANK1_LOW || end > BANK1_HIGH)
    return MOS_CACHE_BAD_ARGUMENT;
  pool_base[1] = base;
  pool_units[1] = units;
  pool_shift[1] = unit_shift;
  bank1_ready = 1;
  return MOS_CACHE_OK;
}

uint8_t mos_cache_static(void *pool, uint8_t units) {
  if (!pool || !units || units > MAX_BANK0_UNITS || pool_base[0] ||
      ((uint32_t)units << pool_shift[0]) > 0xFFFF)
    return MOS_CACHE_BAD_ARGUMENT;
  pool_base[0] = (uint16_t)pool;
  pool_units[0] = units;
  return MOS_CACHE_OK;
}

// ---- Modules ----------------------------------------------------------------

// Relocation table: repeated {1, off16} (FULL16) or {2, lo_off16, hi_off16}
// (paired LOW8/HIGH8), terminated by 0. Offsets are from the module base.
// Adds `delta` (mod 65536) to every listed value.
static void apply_relocs(uint8_t bank, uint16_t base, uint16_t table,
                         uint16_t delta) {
  const uint8_t *p = (const uint8_t *)table;
  for (;;) {
    uint8_t kind = *p++;
    if (!kind)
      break;
    if (kind == 1) {
      uint16_t a = base + (p[0] | ((uint16_t)p[1] << 8));
      p += 2;
      set16(bank, a, get16(bank, a) + delta);
    } else {
      uint16_t lo = base + (p[0] | ((uint16_t)p[1] << 8));
      uint16_t hi = base + (p[2] | ((uint16_t)p[3] << 8));
      uint16_t v;
      p += 4;
      v = get8(bank, lo) | ((uint16_t)get8(bank, hi) << 8);
      v += delta;
      set8(bank, lo, (uint8_t)v);
      set8(bank, hi, (uint8_t)(v >> 8));
    }
  }
}

// Called with the id of every module evicted (a diagnostic hook; weak, doing
// nothing by default). Evictions also happen inside malloc's reclaim path, so
// the hook may run there and must obey the same rules (cache.h).
__attribute__((weak)) void mos_cache_on_evict(uint8_t id) { (void)id; }

uint8_t mos_cache_module_evict(uint8_t id) {
  uint16_t addr, size;
  uint8_t bank;
  unit_t units;
  if (id >= NMODS)
    return MOS_CACHE_NO_SUCH_MODULE;
  addr = __mos_mt_addr[id];
  size = __mos_mt_size[id];
  bank = MOD_IN_BANK1(id);
  units = units_of(bank, size);
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
  release(bank, unit_of(bank, addr), units);
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
    if (!MOD_RESIDENT(id) || __mos_mt_active[id])
      continue;
    if (MOD_IN_BANK1(id) != bank)
      continue;
    if (cold_only && __mos_mt_ref[id])
      continue;
    if (best == 0xFF || older(__mos_mt_stamp[id], __mos_mt_stamp[best]))
      best = id;
  }
  return best;
}

// CLOCK-style victim choice: the oldest candidate not used since the last
// sweep; if every candidate was used, the sweep clears all reference bits
// (second chance) and the oldest is taken. 0xFF if there is no candidate.
static uint8_t find_victim(uint8_t bank) {
  uint8_t v = oldest(bank, 1), id;
  if (v != 0xFF)
    return v;
  for (id = 0; id < NMODS; id++)
    __mos_mt_ref[id] = 0;
  return oldest(bank, 0);
}

// ---- Objects ----------------------------------------------------------------

static struct {
  uint16_t addr, size, stamp;
  uint8_t bank, lock, used;
} ho[NOBJ];
static uint16_t ostamp; // last-lock stamp source (LRU for objects)

static uint8_t valid_handle(mos_cache_handle_t handle) {
  return handle && handle <= NOBJ && ho[handle - 1].used;
}

static void poll_point(void);

// Make room in `bank` without discarding anything: move the least recently
// locked unlocked object that fits into the OTHER bank's current free space
// over there. Returns 1 if an object was moved. (Only the other bank is a place
// an object can go until an REU/disk tier exists; if it has no room this
// returns 0 and the caller falls through to evicting modules, and finally to
// failure.)
static uint8_t spill_one(uint8_t bank) {
  uint32_t tried = 0;
  for (;;) {
    uint8_t h, best = 0xFF;
    unit_t units, idx;
    uint16_t dst;
    for (h = 0; h < NOBJ; h++) {
      if (!ho[h].used || ho[h].lock || ho[h].bank != bank ||
          (tried & ((uint32_t)1 << h)))
        continue;
      if (best == 0xFF || older(ho[h].stamp, ho[best].stamp))
        best = h;
    }
    if (best == 0xFF)
      return 0;
    tried |= (uint32_t)1 << best;
    units = units_of(!bank, ho[best].size);
    idx = alloc(!bank, units);
    if (idx == NO_UNIT)
      continue; // does not fit over there: try the next
    dst = unit_addr(!bank, idx);
    xcopy(!bank, dst, bank, ho[best].addr, ho[best].size);
    release(bank, unit_of(bank, ho[best].addr), units_of(bank, ho[best].size));
    ho[best].addr = dst;
    ho[best].bank = !bank;
    mos_cache_stats.obj_spills++;
    return 1;
  }
}

uint16_t mos_cache_free_units(uint8_t bank) {
  unit_t u = 0, n = 0;
  bank = bank != 0;
  if (bank)
    ensure_bank1();
  while (u < pool_units[bank]) {
    uint8_t byte = bitmap[bank][u >> 3];
    if (!(u & 7) && u + 8 <= pool_units[bank] && (byte == 0xFF || !byte)) {
      if (!byte)
        n += 8;
      u += 8;
      continue;
    }
    if (!bit_used(bank, u))
      n++;
    u++;
  }
  return n;
}

// Longest run of clear bits in the first `n` bits of `bm`, scanning a byte at
// a time where the byte is all clear or all set.
static unit_t longest_clear_run(const uint8_t *bm, unit_t n) {
  unit_t u = 0, run = 0, best = 0;
  while (u < n) {
    uint8_t byte = bm[u >> 3];
    if (!(u & 7) && u + 8 <= n && (byte == 0xFF || !byte)) {
      if (byte) {
        run = 0;
      } else {
        run += 8;
        if (run > best)
          best = run;
      }
      u += 8;
      continue;
    }
    if (byte & (1 << (u & 7)))
      run = 0;
    else if (++run > best)
      best = run;
    u++;
  }
  return best;
}

// Longest run of free units.
uint16_t mos_cache_max_run(uint8_t bank) {
  bank = bank != 0;
  if (bank)
    ensure_bank1();
  return longest_clear_run(bitmap[bank], pool_units[bank]);
}

// FEASIBILITY: the longest run of units in `bank` NOT held by a pinned item (an
// active module or a locked object). Pinned items can never move or be evicted,
// so no amount of spilling/eviction/defragmentation can produce a bigger
// contiguous run than this.
static uint8_t any_pinned(uint8_t bank) {
  uint8_t id;
  for (id = 0; id < NMODS; id++)
    if (MOD_RESIDENT(id) && __mos_mt_active[id] && MOD_IN_BANK1(id) == bank)
      return 1;
  for (id = 0; id < NOBJ; id++)
    if (ho[id].used && ho[id].lock && ho[id].bank == bank)
      return 1;
  return 0;
}

static unit_t max_gap(uint8_t bank) {
  uint8_t pin[(MAX_UNITS + 7) / 8], id;
  unit_t s, n;
  if (!any_pinned(bank)) // the common case: no need to build the pin map
    return pool_units[bank];
  memset(pin, 0, (pool_units[bank] + 7) / 8);
  for (id = 0; id < NMODS; id++) {
    if (!MOD_RESIDENT(id) || !__mos_mt_active[id] || MOD_IN_BANK1(id) != bank)
      continue;
    s = unit_of(bank, __mos_mt_addr[id]);
    for (n = 0; n < units_of(bank, __mos_mt_size[id]); n++)
      pin[(s + n) >> 3] |= 1 << ((s + n) & 7);
  }
  for (id = 0; id < NOBJ; id++) {
    if (!ho[id].used || !ho[id].lock || ho[id].bank != bank)
      continue;
    s = unit_of(bank, ho[id].addr);
    for (n = 0; n < units_of(bank, ho[id].size); n++)
      pin[(s + n) >> 3] |= 1 << ((s + n) & 7);
  }
  return longest_clear_run(pin, pool_units[bank]);
}

// DEFRAGMENTATION of one bank's pool: slide every unpinned resident (module or
// object) down to the end of the previous item, so free space collects into as
// few runs as the pinned items allow. Modules are relocated by the move delta
// (added to their current values, so self-modified operands stay correct);
// objects are plain copies (their handles do not change). Returns the number of
// items moved.
static uint8_t defrag(uint8_t bank) {
  unit_t cursor = 0;
  uint8_t moved = 0;
  for (;;) {
    uint8_t id, bkind = 0, bidx = 0, pinned;
    unit_t bs = NO_UNIT, n;
    uint16_t oldaddr, newaddr;
    for (id = 0; id < NMODS; id++) {
      unit_t s;
      if (!MOD_RESIDENT(id) || MOD_IN_BANK1(id) != bank)
        continue;
      s = unit_of(bank, __mos_mt_addr[id]);
      if (s >= cursor && (bs == NO_UNIT || s < bs)) {
        bs = s;
        bkind = 1;
        bidx = id;
      }
    }
    for (id = 0; id < NOBJ; id++) {
      unit_t s;
      if (!ho[id].used || ho[id].bank != bank)
        continue;
      s = unit_of(bank, ho[id].addr);
      if (s >= cursor && (bs == NO_UNIT || s < bs)) {
        bs = s;
        bkind = 2;
        bidx = id;
      }
    }
    if (bs == NO_UNIT)
      break;
    if (bkind == 1) {
      n = units_of(bank, __mos_mt_size[bidx]);
      pinned = __mos_mt_active[bidx] != 0;
    } else {
      n = units_of(bank, ho[bidx].size);
      pinned = ho[bidx].lock != 0;
    }
    if (pinned || bs == cursor) {
      cursor = bs + n;
      continue;
    }
    newaddr = unit_addr(bank, cursor);
    if (bkind == 1) {
      oldaddr = __mos_mt_addr[bidx];
      // Moving down: a forward copy is overlap-safe.
      xcopy(bank, newaddr, bank, oldaddr, __mos_mt_size[bidx]);
      apply_relocs(bank, newaddr, __mos_mt_reloc[bidx],
                   (uint16_t)(newaddr - oldaddr));
      __mos_mt_addr[bidx] = newaddr;
    } else {
      oldaddr = ho[bidx].addr;
      xcopy(bank, newaddr, bank, oldaddr, ho[bidx].size);
      ho[bidx].addr = newaddr;
    }
    release(bank, bs, n);
    mark(bank, cursor, n);
    cursor += n;
    moved++;
    mos_cache_stats.defrag_moves++;
  }
  return moved;
}

// alloc, and if the bank has enough free units in total but no contiguous run,
// defragment it first.
static unit_t alloc_defrag(uint8_t bank, unit_t units) {
  unit_t idx = alloc(bank, units);
  if (idx != NO_UNIT)
    return idx;
  if (mos_cache_free_units(bank) >= units && defrag(bank))
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
// spills, evictions or moves. Returns the unit index (or 0xFF) and sets
// *bank_out. `only` = 0xFF means either bank, else that bank only.
static unit_t place(uint8_t first, uint8_t only, uint16_t size,
                    uint8_t *bank_out) {
  uint8_t k, bank = 0, ok[2];
  unit_t idx = NO_UNIT, units[2];
  ensure_bank1();
  for (k = 0; k < 2; k++) {
    units[k] = units_of(k, size);
    ok[k] = (only == 0xFF || only == k) && units[k] <= pool_units[k] &&
            max_gap(k) >= units[k];
  }
  *bank_out = 0;
  if (!ok[0] && !ok[1]) {
    mos_cache_stats.place_refused++;
    return NO_UNIT;
  }
  for (k = 0; k < 2 && idx == NO_UNIT; k++) { // pass 0
    bank = k ? !first : first;
    if (ok[bank])
      idx = alloc(bank, units[bank]);
    if (idx == NO_UNIT && bank == 0)
      sh_want = 1; // bank 0 was full: wants to grow (shared mode)
  }
  for (k = 0; k < 2 && idx == NO_UNIT; k++) { // pass 1: defragment
    bank = k ? !first : first;
    if (ok[bank])
      idx = alloc_defrag(bank, units[bank]);
  }
  for (k = 0; k < 2 && idx == NO_UNIT; k++) { // pass 2: spill / evict
    bank = k ? !first : first;
    if (!ok[bank])
      continue;
    for (;;) {
      uint8_t victim;
      idx = alloc_defrag(bank, units[bank]);
      if (idx != NO_UNIT)
        break;
      if (spill_one(bank))
        continue; // lossless: move an object across
      victim = find_victim(bank);
      if (victim == 0xFF)
        break;
      mos_cache_module_evict(victim);
    }
  }
  *bank_out = bank;
  return idx;
}

uint8_t mos_cache_module_load(uint8_t id, uint8_t caller_bank) {
  uint16_t size;
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
  {
    uint16_t dest = unit_addr(bank, idx);
    if (bank)
      c128_bank1_write(dest, (const void *)__mos_mt_img[id], size);
    else
      memcpy((void *)dest, (const void *)__mos_mt_img[id], size);
    apply_relocs(bank, dest, __mos_mt_reloc[id],
                 (uint16_t)(dest - __mos_mt_img[id]));
    __mos_mt_addr[id] = dest;
    __mos_mt_cr[id] = bank ? MOS_CACHE_CR_BANK1 : MOS_CACHE_CR_BANK0;
  }
  __mos_mt_stamp[id] = ++stamp_clock;
  mos_cache_stats.mod_loads++;
  return MOS_CACHE_OK;
}

mos_cache_handle_t mos_cache_malloc(uint16_t size) {
  uint8_t h, bank = 0;
  unit_t idx;
  poll_point();
  for (h = 0; h < NOBJ && ho[h].used; h++) {
  }
  if (h == NOBJ || !size)
    return 0;
  idx = place(0, 0xFF, size, &bank);
  if (idx == NO_UNIT)
    return 0;
  ho[h].addr = unit_addr(bank, idx);
  ho[h].size = size;
  ho[h].bank = bank;
  ho[h].lock = 0;
  ho[h].used = 1;
  ho[h].stamp = ++ostamp;
  return h + 1;
}

uint8_t mos_cache_free(mos_cache_handle_t handle) {
  if (!valid_handle(handle))
    return MOS_CACHE_INVALID_HANDLE;
  if (ho[handle - 1].lock)
    return MOS_CACHE_LOCKED;
  release(ho[handle - 1].bank,
          unit_of(ho[handle - 1].bank, ho[handle - 1].addr),
          units_of(ho[handle - 1].bank, ho[handle - 1].size));
  ho[handle - 1].used = 0;
  return MOS_CACHE_OK;
}

// mos_cache_lock cannot switch $FF00 and hand back a bank-1 pointer: the
// caller's own code (ordinary bank-0 RAM, not Common RAM) would vanish. Instead
// a lock makes the object resident in the CALLER'S OWN bank, migrating it
// through the Common-RAM staging routines if it is in the other one; no bank
// switch is held across caller code. A lock held by one bank's code pins the
// object there, so the other bank's lock attempt fails (NULL).
void *mos_cache_lock_in(mos_cache_handle_t handle, uint8_t caller_bank) {
  uint8_t nb = 0;
  unit_t units, idx;
  uint16_t dst;
  caller_bank = caller_bank != 0;
  poll_point();
  if (!valid_handle(handle))
    return 0;
  if (ho[handle - 1].bank != caller_bank) {
    uint8_t from = ho[handle - 1].bank;
    if (ho[handle - 1].lock)
      return 0;
    idx = place(caller_bank, caller_bank, ho[handle - 1].size, &nb);
    if (idx == NO_UNIT)
      return 0;
    dst = unit_addr(caller_bank, idx);
    xcopy(caller_bank, dst, from, ho[handle - 1].addr, ho[handle - 1].size);
    units = units_of(from, ho[handle - 1].size);
    release(from, unit_of(from, ho[handle - 1].addr), units);
    ho[handle - 1].addr = dst;
    ho[handle - 1].bank = caller_bank;
  }
  ho[handle - 1].lock++;
  ho[handle - 1].stamp = ++ostamp;
  return (void *)ho[handle - 1].addr;
}

void *mos_cache_lock(mos_cache_handle_t handle) {
  return mos_cache_lock_in(handle, 0);
}

void mos_cache_unlock(mos_cache_handle_t handle) {
  if (valid_handle(handle) && ho[handle - 1].lock)
    ho[handle - 1].lock--;
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
  return valid_handle(handle) ? ho[handle - 1].bank : MOS_CACHE_INVALID_BANK;
}
uint8_t mos_cache_handle_locks(mos_cache_handle_t handle) {
  return valid_handle(handle) ? ho[handle - 1].lock : 0;
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
  uint8_t id;
  for (id = 0; id < NMODS; id++)
    if (MOD_RESIDENT(id) && __mos_mt_active[id] && !MOD_IN_BANK1(id))
      return 1;
  for (id = 0; id < NOBJ; id++)
    if (ho[id].used && ho[id].lock && !ho[id].bank)
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
  unit_t before = mos_cache_free_units(0);
  return mos_cache_tier_demote(0) && mos_cache_free_units(0) > before;
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
    if (!room &&
        mos_cache_free_units(0)) { // free units exist, but not at the end
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
  if (!add || (uint16_t)cur + add > sh_max || pool0_pinned())
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
      if (ho[id].used && !ho[id].bank)
        ho[id].addr += delta;
    pool_base[0] = (uint16_t)np;
    mos_cache_stats.pool_moves++;
  }
  pool_units[0] = cur + add;
  mos_cache_stats.grown_units += add;
  return add;
}

uint8_t mos_cache_shared(uint8_t min_units, uint8_t init_units,
                         uint8_t max_units, uint16_t low, uint16_t high) {
  uint8_t *p;
  if (pool_base[0] || !min_units || min_units > init_units ||
      init_units > max_units || max_units > MAX_BANK0_UNITS || low >= high ||
      ((uint32_t)max_units << pool_shift[0]) > 0xFFFF)
    return MOS_CACHE_BAD_ARGUMENT;
  sh_busy = 1;
  p = malloc((uint16_t)init_units << pool_shift[0]);
  sh_busy = 0;
  if (!p)
    return MOS_CACHE_NO_MEMORY;
  pool_base[0] = (uint16_t)p;
  pool_units[0] = init_units;
  sh_min = min_units;
  sh_max = max_units;
  sh_low = low;
  sh_high = high;
  shared = 1;
  return MOS_CACHE_OK;
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
    size_t units = ((sh_low - free) >> pool_shift[0]) + 1;
    changed = pool0_yield(units > 255 ? 255 : (uint8_t)units) != 0;
  } else if (sh_want && free > sh_high) {
    size_t spare = (free - sh_high) >> pool_shift[0];
    uint8_t add = spare > 4 ? 4 : (uint8_t)spare; // a few units per call
    if (add > sh_max - pool_units[0])
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
  size_t units = (needed >> pool_shift[0]) + 1;
  mos_cache_stats.hook_calls++;
  if (!shared || sh_busy)
    return 0;
  return pool0_yield(units > 255 ? 255 : (uint8_t)units) != 0;
}
