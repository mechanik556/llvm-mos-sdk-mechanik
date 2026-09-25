#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// M0.2: Module Table state, two-bank allocator, relocating loader, evictor.
// Tables are ordinary bank-0 RAM (the gate normalizes to bank 0 before
// touching them). mt_addr[id]'s high byte is 0 while a module is not resident.
//
// Canonical form: a module's stored image (mt_img[id], in the program image,
// standing in for the disk/REU tiers) is assembled at its own address; all
// self-referential 16-bit values in it are "canonical" relative to that base.
// Loading at `dest` adds delta = dest - mt_img[id] at every relocation-table
// location; evicting SUBTRACTS delta from whatever value is currently there
// (design 4.1b - correct even for self-modified operands) and writes the
// image back. Paired lo/hi fixups are combined into one 16-bit operation.

#define NMODS 10
#define UNIT 32           // allocation granularity, bytes
#define POOL0_UNITS 5     // deliberately small so modules spill into bank 1
#define POOL1_UNITS 32
#define HOST 6            // static, always-resident module (the "base program")

#ifdef COMMON_TABLES   /* variant gate (gate_ct.s): tables in Common RAM, reachable from any bank */
#define CT __attribute__((section(".c128commoncode.tables")))
#else
#define CT
#endif
CT uint16_t mt_addr[NMODS];
CT uint8_t mt_cr[NMODS];
CT volatile uint8_t mt_active[NMODS];
CT volatile uint8_t mt_ref[NMODS];    // CLOCK reference bit, set by the gate on every call
static uint16_t mt_stamp[NMODS];   // load order (tie-break among equally cold modules)
static uint16_t stamp_clock;
extern const uint16_t mt_img[NMODS];    // canonical image address (bank 0)
extern const uint16_t mt_size[NMODS];   // image size in bytes
extern const uint16_t mt_reloc[NMODS];  // relocation table address
extern volatile uint8_t gt_cr;          // caller's $FF00 (zero page, gate.s)

// Bank 0's pool. In static mode (the default) it is this array. In shared mode
// (mos_cache_shared, below) it is instead a block of the ordinary malloc heap,
// pool0_p, that grows and shrinks.
static uint8_t pool0_static[POOL0_UNITS * UNIT];
static uint8_t *pool0_p;
// Bank-1 pool: only its ADDRESS is used from bank-0 code (never dereferenced
// there); bank 1 is accessed via the __c128bank1_* helpers.
__attribute__((section(".c128bank1.bss"))) static uint8_t pool1[POOL1_UNITS * UNIT];

static uint8_t bitmap[2][(POOL1_UNITS + 7) / 8];
static uint8_t pool_units[2] = {POOL0_UNITS, POOL1_UNITS};   // bank 0's changes in shared mode

static uint8_t sh_want;             // bank 0 was too small for a request: it would like to grow (shared mode)
uint8_t mod_loads, mod_evictions;   // counters (for tests)
uint8_t evict_log[16], evict_n;     // ids of evicted modules, in order (for tests)

void proto_bank1_write(char *vma, const char *lma, unsigned short size);
void proto_bank1_read(char *dest, const char *src, unsigned short size);

static uint16_t pool_base(uint8_t bank) { return bank ? (uint16_t)pool1 : pool0_p ? (uint16_t)pool0_p : (uint16_t)pool0_static; }
static uint8_t units_of(uint16_t size) { return (uint8_t)((size + UNIT - 1) / UNIT); }
static uint8_t bit_used(uint8_t bank, uint8_t u) { return bitmap[bank][u >> 3] & (1 << (u & 7)); }

// First-fit run of `units` free units in `bank`'s pool; returns unit index or 0xFF.
static uint8_t alloc(uint8_t bank, uint8_t units) {
  uint8_t n = pool_units[bank], run = 0, u;
  for (u = 0; u < n; u++) {
    if (bit_used(bank, u)) { run = 0; continue; }
    if (++run == units) {
      uint8_t start = u + 1 - units, k;
      for (k = start; k <= u; k++) bitmap[bank][k >> 3] |= 1 << (k & 7);
      return start;
    }
  }
  return 0xFF;
}

static void release(uint8_t bank, uint8_t start, uint8_t units) {
  uint8_t k;
  for (k = start; k < start + units; k++) bitmap[bank][k >> 3] &= ~(1 << (k & 7));
}

// Byte/word access to a resident module's memory in either bank.
static uint8_t get8(uint8_t bank, uint16_t a) {
  uint8_t v;
  if (bank) proto_bank1_read((char *)&v, (const char *)a, 1);
  else v = *(volatile uint8_t *)a;
  return v;
}
static void set8(uint8_t bank, uint16_t a, uint8_t v) {
  if (bank) proto_bank1_write((char *)a, (const char *)&v, 1);
  else *(volatile uint8_t *)a = v;
}
static uint16_t get16(uint8_t bank, uint16_t a) { return get8(bank, a) | ((uint16_t)get8(bank, a + 1) << 8); }
static void set16(uint8_t bank, uint16_t a, uint16_t v) { set8(bank, a, (uint8_t)v); set8(bank, a + 1, (uint8_t)(v >> 8)); }

// Relocation table: repeated {1, off16} (FULL16) or {2, lo_off16, hi_off16}
// (paired LOW8/HIGH8), terminated by 0. Offsets are from the module base.
// Adds `delta` (mod 65536) to every listed value.
static void apply_relocs(uint8_t bank, uint16_t base, uint16_t table, uint16_t delta) {
  const uint8_t *p = (const uint8_t *)table;
  for (;;) {
    uint8_t kind = *p++;
    if (!kind) break;
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

// Returns 0 ok; 1 = pinned (active_entries != 0); 2 = not resident; 3 = static.
uint8_t mod_evict(uint8_t id) {
  uint16_t addr = mt_addr[id], size = mt_size[id];
  uint8_t bank = (mt_cr[id] == 0x4E), units = units_of(size);
  if (!mt_img[id]) return 3;
  if (!(addr >> 8)) return 2;
  if (mt_active[id]) return 1;
  // Un-relocate in place by delta subtraction, then write the canonical
  // image back to the backing store (the program image here).
  apply_relocs(bank, addr, mt_reloc[id], (uint16_t)(mt_img[id] - addr));
  if (bank) proto_bank1_read((char *)mt_img[id], (const char *)addr, size);
  else memcpy((void *)mt_img[id], (const void *)addr, size);
  release(bank, (uint8_t)((addr - pool_base(bank)) / UNIT), units);
  mt_addr[id] = 0;
  mt_cr[id] = 0;
  mod_evictions++;
  if (evict_n < 16) evict_log[evict_n++] = id;
  return 0;
}

// Oldest resident, non-static, inactive module in `bank` (optionally only
// those whose reference bit is clear), or 0xFF.
static uint8_t oldest(uint8_t bank, uint8_t cold_only) {
  uint8_t id, best = 0xFF;
  for (id = 0; id < NMODS; id++) {
    if (!mt_img[id] || !(mt_addr[id] >> 8) || mt_active[id]) continue;
    if ((mt_cr[id] == 0x4E) != bank) continue;
    if (cold_only && mt_ref[id]) continue;
    if (best == 0xFF || mt_stamp[id] < mt_stamp[best]) best = id;
  }
  return best;
}

// CLOCK-style victim choice: the oldest candidate not used since the last
// sweep; if every candidate was used, the sweep clears all reference bits
// (second chance) and the oldest is taken. 0xFF if there is no candidate.
static uint8_t find_victim(uint8_t bank) {
  uint8_t v = oldest(bank, 1), id;
  if (v != 0xFF) return v;
  for (id = 0; id < NMODS; id++) mt_ref[id] = 0;
  return oldest(bank, 0);
}
void mod_clear_refs(void) { uint8_t id; for (id = 0; id < NMODS; id++) mt_ref[id] = 0; }

#define NOBJ 32
typedef uint16_t mos_handle_t;
static struct { uint16_t addr, size, stamp; uint8_t bank, lock, used; } ho[NOBJ];
static uint16_t ostamp;             // last-lock stamp source (LRU for objects)
uint8_t defrag_moves, place_refused; // defragmentation moves; requests refused up front by the feasibility check (tests)
static void poll_point(void);       // safe point: maybe run mos_cache_service (shared mode, automatic polling)
uint8_t obj_spills;                 // objects moved to the other bank to make room (tests)

// Copy between arbitrary banks in 16-byte chunks via a local buffer.
static void xcopy(uint8_t dbank, uint16_t dst, uint8_t sbank, uint16_t src, uint16_t size) {
  uint8_t buf[16];
  while (size) {
    uint8_t n = size > 16 ? 16 : (uint8_t)size;
    if (sbank) proto_bank1_read((char *)buf, (const char *)src, n);
    else memcpy(buf, (const void *)src, n);
    if (dbank) proto_bank1_write((char *)dst, (const char *)buf, n);
    else memcpy((void *)dst, buf, n);
    dst += n; src += n; size -= n;
  }
}


// Make room in `bank` without discarding anything: move the least recently
// locked unlocked object that fits into the OTHER bank's current free space
// over there. Returns 1 if an object was moved. (No REU/disk tier exists yet,
// so the other bank is the only place an object can go; if it has no room
// this returns 0 and the caller falls through to evicting modules, and
// finally to an out-of-memory error.)
static uint8_t spill_one(uint8_t bank) {
  uint32_t tried = 0;
  for (;;) {
    uint8_t h, best = 0xFF, units, idx;
    uint16_t dst;
    for (h = 0; h < NOBJ; h++) {
      if (!ho[h].used || ho[h].lock || ho[h].bank != bank || (tried & ((uint32_t)1 << h))) continue;
      if (best == 0xFF || ho[h].stamp < ho[best].stamp) best = h;
    }
    if (best == 0xFF) return 0;
    tried |= (uint32_t)1 << best;
    units = units_of(ho[best].size);
    idx = alloc(!bank, units);
    if (idx == 0xFF) continue;                   /* does not fit over there: try the next */
    dst = pool_base(!bank) + (uint16_t)idx * UNIT;
    xcopy(!bank, dst, bank, ho[best].addr, ho[best].size);
    release(bank, (uint8_t)((ho[best].addr - pool_base(bank)) / UNIT), units);
    ho[best].addr = dst;
    ho[best].bank = !bank;
    obj_spills++;
    return 1;
  }
}

static void mark(uint8_t bank, uint8_t start, uint8_t units) {
  uint8_t k;
  for (k = start; k < start + units; k++) bitmap[bank][k >> 3] |= 1 << (k & 7);
}

uint8_t pool_free_units(uint8_t bank) {
  uint8_t u, n = 0;
  for (u = 0; u < pool_units[bank]; u++) if (!bit_used(bank, u)) n++;
  return n;
}
// Longest run of free units (test helper).
uint8_t pool_max_run(uint8_t bank) {
  uint8_t u, run = 0, best = 0;
  for (u = 0; u < pool_units[bank]; u++) {
    if (bit_used(bank, u)) run = 0; else if (++run > best) best = run;
  }
  return best;
}

static uint8_t unit_of(uint8_t bank, uint16_t addr) { return (uint8_t)((addr - pool_base(bank)) / UNIT); }

// FEASIBILITY: the longest run of units in `bank` NOT held by a pinned item
// (an active module or a locked object). Pinned items can never move or be
// evicted, so no amount of spilling/eviction/defragmentation can produce a
// bigger contiguous run than this.
static uint8_t max_gap(uint8_t bank) {
  uint8_t pin[(POOL1_UNITS + 7) / 8] = {0}, id, u, run = 0, best = 0, s, n;
  for (id = 0; id < NMODS; id++) {
    if (!mt_img[id] || !(mt_addr[id] >> 8) || !mt_active[id] || (mt_cr[id] == 0x4E) != bank) continue;
    s = unit_of(bank, mt_addr[id]);
    for (n = 0; n < units_of(mt_size[id]); n++) pin[(s + n) >> 3] |= 1 << ((s + n) & 7);
  }
  for (id = 0; id < NOBJ; id++) {
    if (!ho[id].used || !ho[id].lock || ho[id].bank != bank) continue;
    s = unit_of(bank, ho[id].addr);
    for (n = 0; n < units_of(ho[id].size); n++) pin[(s + n) >> 3] |= 1 << ((s + n) & 7);
  }
  for (u = 0; u < pool_units[bank]; u++) {
    if (pin[u >> 3] & (1 << (u & 7))) run = 0; else if (++run > best) best = run;
  }
  return best;
}

// DEFRAGMENTATION of one bank's pool: slide every unpinned resident (module
// or object) down to the end of the previous item, so free space collects
// into as few runs as the pinned items allow. Modules are relocated by the
// move delta (added to their current values, so self-modified operands stay
// correct); objects are plain copies (their handles do not change). Returns
// the number of items moved.
static uint8_t defrag(uint8_t bank) {
  uint8_t cursor = 0, moved = 0;
  for (;;) {
    uint8_t id, bs = 0xFF, bkind = 0, bidx = 0, n, pinned;
    uint16_t oldaddr, newaddr;
    for (id = 0; id < NMODS; id++) {
      uint8_t s;
      if (!mt_img[id] || !(mt_addr[id] >> 8) || (mt_cr[id] == 0x4E) != bank) continue;
      s = unit_of(bank, mt_addr[id]);
      if (s >= cursor && (bs == 0xFF || s < bs)) { bs = s; bkind = 1; bidx = id; }
    }
    for (id = 0; id < NOBJ; id++) {
      uint8_t s;
      if (!ho[id].used || ho[id].bank != bank) continue;
      s = unit_of(bank, ho[id].addr);
      if (s >= cursor && (bs == 0xFF || s < bs)) { bs = s; bkind = 2; bidx = id; }
    }
    if (bs == 0xFF) break;
    if (bkind == 1) { n = units_of(mt_size[bidx]); pinned = mt_active[bidx] != 0; }
    else            { n = units_of(ho[bidx].size); pinned = ho[bidx].lock != 0; }
    if (pinned || bs == cursor) { cursor = bs + n; continue; }
    newaddr = pool_base(bank) + (uint16_t)cursor * UNIT;
    if (bkind == 1) {
      oldaddr = mt_addr[bidx];
      xcopy(bank, newaddr, bank, oldaddr, mt_size[bidx]);     /* moving down: forward copy is overlap-safe */
      apply_relocs(bank, newaddr, mt_reloc[bidx], (uint16_t)(newaddr - oldaddr));
      mt_addr[bidx] = newaddr;
    } else {
      oldaddr = ho[bidx].addr;
      xcopy(bank, newaddr, bank, oldaddr, ho[bidx].size);
      ho[bidx].addr = newaddr;
    }
    release(bank, bs, n);
    mark(bank, cursor, n);
    cursor += n;
    moved++;
    defrag_moves++;
  }
  return moved;
}

// alloc, and if the bank has enough free units in total but no contiguous run,
// defragment it first.
static uint8_t alloc_defrag(uint8_t bank, uint8_t units) {
  uint8_t idx = alloc(bank, units);
  if (idx != 0xFF) return idx;
  if (pool_free_units(bank) >= units && defrag(bank)) idx = alloc(bank, units);
  return idx;
}

// Find room for `units` units, preferring `first` bank (design 11.6 pt 4):
//  0. free space in the preferred bank, then the other;
//  1. defragment a bank that has enough free units in total;
//  2. only then make room: spill an unlocked object to the other bank
//     (lossless) or evict the oldest cold module, defragmenting whenever the
//     freed total is enough, until it fits.
// A FEASIBILITY check runs first: a request larger than the longest run not
// held by pinned items in every allowed bank is refused up front, with no
// spills, evictions or moves. Returns the unit index (or 0xFF) and sets
// *bank_out. `only` = 0xFF means either bank, else that bank only.
static uint8_t place(uint8_t first, uint8_t only, uint8_t units, uint8_t *bank_out) {
  uint8_t k, bank = 0, idx = 0xFF, ok[2];
  for (k = 0; k < 2; k++)
    ok[k] = (only == 0xFF || only == k) && units <= pool_units[k] && max_gap(k) >= units;
  *bank_out = 0;
  if (!ok[0] && !ok[1]) { place_refused++; return 0xFF; }
  for (k = 0; k < 2 && idx == 0xFF; k++) {                 /* pass 0 */
    bank = k ? !first : first;
    if (ok[bank]) idx = alloc(bank, units);
    if (idx == 0xFF && bank == 0) sh_want = 1;   /* bank 0 was full: wants to grow (shared mode) */
  }
  for (k = 0; k < 2 && idx == 0xFF; k++) {                 /* pass 1: defragment */
    bank = k ? !first : first;
    if (ok[bank]) idx = alloc_defrag(bank, units);
  }
  for (k = 0; k < 2 && idx == 0xFF; k++) {                 /* pass 2: spill / evict */
    bank = k ? !first : first;
    if (!ok[bank]) continue;
    for (;;) {
      uint8_t victim;
      idx = alloc_defrag(bank, units);
      if (idx != 0xFF) break;
      if (spill_one(bank)) continue;     /* lossless: move an object across */
      victim = find_victim(bank);
      if (victim == 0xFF) break;
      mod_evict(victim);
    }
  }
  *bank_out = bank;
  return idx;
}

// Returns 0 on success, 1 = out of memory.
uint8_t mod_load(uint8_t id) {
  uint16_t size = mt_size[id];
  uint8_t units = units_of(size), bank = 0;
  uint8_t idx;
  poll_point();
  idx = place((gt_cr & 0x40) ? 1 : 0, 0xFF, units, &bank);
  if (idx == 0xFF) return 1;
  {
    uint16_t dest = pool_base(bank) + (uint16_t)idx * UNIT;
    if (bank) proto_bank1_write((char *)dest, (const char *)mt_img[id], size);
    else memcpy((void *)dest, (const void *)mt_img[id], size);
    apply_relocs(bank, dest, mt_reloc[id], (uint16_t)(dest - mt_img[id]));
    mt_addr[id] = dest;
    mt_cr[id] = bank ? 0x4E : 0x0E;
  }
  mt_stamp[id] = ++stamp_clock;
  mod_loads++;
  return 0;
}

// ---- Cacheable heap objects (design 4.1d) -------------------------------
// WHY THIS IS A SEPARATE, HANDLE-BASED API AND NOT malloc/new (full argument:
// work/M0_C128_BANKING_PLAN.md section 11.10):
//  - A pointer does not carry a bank, and bank-1 memory can only be touched by
//    Common-RAM code with interrupts off (~105 cycles per byte, measured by
//    prototypes/c128-dynamic/access_cost.sh, vs ~4 for a load), so ordinary
//    pointer dereferences cannot reach it.
//  - The compiler cannot tell at a dereference which pointers are bank-1 or
//    cacheable pointers (casts through void*/char*/uintptr_t, pointers copied as
//    bytes or stored in other objects, prebuilt libc, arbitrarily late use), so
//    "insert the gate/lock automatically" is not implementable for C/C++.
//  - malloc promises a pointer that stays valid and unmoved until free(), which
//    forbids what this heap does: migrating an object into its caller's bank,
//    spilling it to the other bank, defragmenting, and (later) evicting it to
//    REU/disk. A handle is never a pointer, so the runtime is free to move the
//    bytes; mos_handle_lock yields a pointer valid only until the matching
//    unlock.
//  - operator new/delete are thin wrappers over malloc/free returning T*; the
//    constructor, methods and vtable dispatch that follow are ordinary bank-0
//    code that dereferences `this`, so they cannot be redirected either.
// Ordinary malloc/free/new/delete are untouched: bank-0-only, zero cost.
// Handles are small integers (index+1; 0 = null). Objects share the module
// pools. DESIGN CORRECTION vs 11.6 pts 2-3: mos_handle_lock cannot switch
// $FF00 and hand back a bank-1 pointer - the caller's own code (ordinary
// bank-0 RAM, non-common) would vanish. Instead lock makes the object
// resident in the CALLER'S OWN bank, migrating it through the Common-RAM
// staging helpers if it is in the other one; no bank switch is held across
// caller code. A lock held by one bank's code pins the object there, so the
// other bank's lock attempt fails (NULL) - the remaining "cross-bank
// conflict" case.

mos_handle_t mos_cacheable_malloc(uint16_t size) {
  uint8_t h, bank = 0, idx;
  poll_point();
  for (h = 0; h < NOBJ && ho[h].used; h++) {}
  if (h == NOBJ || !size) return 0;
  idx = place(0, 0xFF, units_of(size), &bank);
  if (idx == 0xFF) return 0;
  ho[h].addr = pool_base(bank) + (uint16_t)idx * UNIT;
  ho[h].size = size; ho[h].bank = bank; ho[h].lock = 0; ho[h].used = 1; ho[h].stamp = ++ostamp;
  return h + 1;
}

// 0 ok, 1 = locked, 2 = invalid handle.
uint8_t mos_cacheable_free(mos_handle_t handle) {
  if (!handle || handle > NOBJ || !ho[handle - 1].used) return 2;
  if (ho[handle - 1].lock) return 1;
  release(ho[handle - 1].bank, (uint8_t)((ho[handle - 1].addr - pool_base(ho[handle - 1].bank)) / UNIT),
          units_of(ho[handle - 1].size));
  ho[handle - 1].used = 0;
  return 0;
}

// Lock for code executing in `caller_bank`; returns a pointer valid in that
// bank until the matching unlock, or 0 (invalid handle, locked from the other
// bank, or no room to migrate).
static void *lock_in(mos_handle_t handle, uint8_t caller_bank) {
  uint8_t units, idx, nb = 0;
  uint16_t dst;
  poll_point();
  if (!handle || handle > NOBJ || !ho[handle - 1].used) return 0;
  if (ho[handle - 1].bank != caller_bank) {
    if (ho[handle - 1].lock) return 0;
    units = units_of(ho[handle - 1].size);
    idx = place(caller_bank, caller_bank, units, &nb);
    if (idx == 0xFF) return 0;
    dst = pool_base(caller_bank) + (uint16_t)idx * UNIT;
    xcopy(caller_bank, dst, ho[handle - 1].bank, ho[handle - 1].addr, ho[handle - 1].size);
    release(ho[handle - 1].bank, (uint8_t)((ho[handle - 1].addr - pool_base(ho[handle - 1].bank)) / UNIT), units);
    ho[handle - 1].addr = dst;
    ho[handle - 1].bank = caller_bank;
  }
  ho[handle - 1].lock++;
  ho[handle - 1].stamp = ++ostamp;
  return (void *)ho[handle - 1].addr;
}
void *mos_handle_lock(mos_handle_t handle) { return lock_in(handle, 0); }   /* bank-0 callers */
void mos_handle_unlock(mos_handle_t handle) {
  if (handle && handle <= NOBJ && ho[handle - 1].lock) ho[handle - 1].lock--;
}
// Test helpers.
uint8_t obj_bank(mos_handle_t handle) { return ho[handle - 1].bank; }
uint8_t obj_lock(mos_handle_t handle) { return ho[handle - 1].lock; }
uint8_t pool0_size(void) { return pool_units[0]; }
uint16_t pool0_base(void) { return pool_base(0); }

// Host (static) module entry points, reached through the gate from module
// code in either bank (offsets 0, 3, 6 of the jump table host_tab in
// modules.s). The caller's bank comes from the gate's saved $FF00.
uint8_t host_try_evict(uint8_t id) { return mod_evict(id); }
// Returns the pointer as a 16-bit INTEGER: llvm-mos returns pointers in
// __rc2/__rc3 but integers in A/X, and module code reads A/X.
uint16_t host_lock(mos_handle_t handle) { return (uint16_t)lock_in(handle, (gt_cr & 0x40) ? 1 : 0); }
void host_unlock(mos_handle_t handle) { mos_handle_unlock(handle); }

// Defragment both banks' pools; returns the number of items moved. Active
// modules and locked objects are never moved.
uint8_t mos_defrag(void) { return defrag(0) + defrag(1); }
uint8_t host_defrag(void) { return mos_defrag(); }

// ---- Shared mode: bank 0's pool is a block of the ordinary malloc heap ------
// (design 11.11). Ordinary malloc/free/new/delete are unchanged and know
// nothing about this; the cache is the polite party:
//  Stage 1 (polling): mos_cache_service(), called at safe points, keeps the
//    heap's free bytes between two watermarks - it gives pool tail back when
//    the heap runs low and takes it again when there is plenty and the cache
//    is short of bank-0 room.
//  Stage 2 (reclaim): __malloc_low_memory, called by malloc itself when a
//    request cannot be satisfied, makes the cache give back what is needed
//    right now. It does NO I/O and calls no allocator entry other than
//    realloc-shrink, which never allocates: it drops clean modules, spills
//    objects to bank 1, and shrinks the pool block in place. Whatever is not
//    cheap to give up in that way (disk demotion of dirty items) is done at
//    the polling safe points, never here.
// Tiers that do not exist yet plug in at mos_tier_demote (an I/O-free faster
// tier such as the REU: room for the items evicted from bank 0/1 before they
// have to be dropped or reach disk) and mos_tier_writebehind (slow-tier
// write-behind, run only from the polling call so that no I/O ever happens
// inside malloc).

size_t __heap_bytes_free(void);

static uint8_t sh_min, sh_max, sh_busy;
static uint16_t sh_low, sh_high;                  // free-byte watermarks of the malloc heap
uint8_t sh_grows, sh_moves, sh_hook_calls, sh_yielded, sh_services;   // counters (for tests)

// Default (weak) tier extension points: nothing to demote to, nothing to write.
__attribute__((weak)) uint8_t mos_tier_demote(uint8_t bank) { (void)bank; return 0; }
__attribute__((weak)) void mos_tier_writebehind(void) {}

// True if any active module or locked object is in bank 0's pool: it cannot
// move, so the pool block must not be moved either.
static uint8_t pool0_pinned(void) {
  uint8_t id;
  for (id = 0; id < NMODS; id++)
    if (mt_img[id] && (mt_addr[id] >> 8) && mt_active[id] && mt_cr[id] != 0x4E) return 1;
  for (id = 0; id < NOBJ; id++)
    if (ho[id].used && ho[id].lock && !ho[id].bank) return 1;
  return 0;
}

static uint8_t used_top(void) {          // highest used unit of bank 0, plus one
  uint8_t u = pool_units[0];
  while (u && !bit_used(0, u - 1)) u--;
  return u;
}

// Give up to `want` units of bank 0's pool tail back to the heap; returns the
// number given. Never goes below sh_min units.
static uint8_t pool0_yield(uint8_t want) {
  uint8_t given = 0;
  sh_busy = 1;
  while (given < want && pool_units[0] > sh_min) {
    uint8_t top = used_top(), flo = top > sh_min ? top : sh_min, room, n, victim;
    room = pool_units[0] - flo;
    if (!room && pool_free_units(0)) {            /* free units exist, but not at the end */
      defrag(0);
      top = used_top(); flo = top > sh_min ? top : sh_min;
      room = pool_units[0] - flo;
    }
    if (!room) {                                  /* make some: cheapest first */
      if (spill_one(0)) continue;                 /* object -> bank 1 (lossless) */
      if (mos_tier_demote(0)) continue;           /* object -> REU (when it exists) */
      victim = find_victim(0);                    /* clean module: just drop it */
      if (victim == 0xFF) break;                  /* only pinned items are left */
      mod_evict(victim);
      continue;
    }
    n = (uint8_t)(want - given);
    if (n > room) n = room;
    if (realloc(pool0_p, (uint16_t)(pool_units[0] - n) * UNIT) != (void *)pool0_p) break;   /* shrinks in place */
    pool_units[0] -= n;
    given += n;
    sh_yielded += n;
  }
  sh_busy = 0;
  return given;
}

// Take `add` more units for bank 0's pool from the heap. The block may have to
// move, so this is refused while anything in bank 0 is pinned; when it does
// move, every resident module in it is relocated by the move delta and every
// object's address adjusted (handles do not change).
static uint8_t pool0_grow(uint8_t add) {
  uint8_t cur = pool_units[0], id;
  uint8_t *np;
  uint16_t old = (uint16_t)pool0_p, delta;
  if (!add || (uint16_t)cur + add > sh_max || pool0_pinned()) return 0;
  sh_busy = 1;
  np = realloc(pool0_p, (uint16_t)(cur + add) * UNIT);
  sh_busy = 0;
  if (!np) return 0;
  if ((uint16_t)np != old) {
    delta = (uint16_t)np - old;
    for (id = 0; id < NMODS; id++) {
      if (!mt_img[id] || !(mt_addr[id] >> 8) || mt_cr[id] == 0x4E) continue;
      mt_addr[id] += delta;
      apply_relocs(0, mt_addr[id], mt_reloc[id], delta);
    }
    for (id = 0; id < NOBJ; id++)
      if (ho[id].used && !ho[id].bank) ho[id].addr += delta;
    pool0_p = np;
    sh_moves++;
  }
  pool_units[0] = cur + add;
  sh_grows += add;
  return add;
}

// Switch to shared mode. Only while bank 0's pool is empty. The pool starts at
// `init_units`, never shrinks below `min_units` or grows beyond `max_units`
// (at most POOL1_UNITS: the bitmaps' size), and mos_cache_service keeps the
// heap's free bytes at least `low` (yielding) and, when growing, at least
// `high` afterwards. Returns 0, 1 = bank 0 in use or already shared, 2 = no room.
uint8_t mos_cache_shared(uint8_t min_units, uint8_t init_units, uint8_t max_units, uint16_t low, uint16_t high) {
  uint8_t *p;
  if (pool0_p || pool_free_units(0) != pool_units[0] || !min_units || min_units > init_units ||
      init_units > max_units || max_units > POOL1_UNITS)
    return 1;
  sh_busy = 1;
  p = malloc((uint16_t)init_units * UNIT);
  sh_busy = 0;
  if (!p) return 2;
  pool0_p = p;
  pool_units[0] = init_units;
  sh_min = min_units; sh_max = max_units; sh_low = low; sh_high = high;
  return 0;
}

// Stage 1, polite client: call at safe points (a main loop, between files or
// frames). Returns 1 if the pool changed size.
uint8_t mos_cache_service(void) {
  size_t free;
  uint8_t changed = 0;
  sh_services++;
  if (!pool0_p || sh_busy) return 0;
  free = __heap_bytes_free();
  if (free < sh_low) {
    size_t need = sh_low - free;
    changed = pool0_yield((uint8_t)(need / UNIT + 1)) != 0;
  } else if (sh_want && free > sh_high) {
    size_t spare = (free - sh_high) / UNIT;
    uint8_t add = spare > 4 ? 4 : (uint8_t)spare;      /* a few units per call */
    if (add > sh_max - pool_units[0]) add = sh_max - pool_units[0];
    changed = pool0_grow(add) != 0;
    if (!add || pool_units[0] == sh_max) sh_want = 0;
  }
  mos_tier_writebehind();
  return changed;
}

// Automatic polling: the safe points of the runtime (a module load, allocating
// an object, locking one) call poll_point(), which runs mos_cache_service every
// `every`th time. The gate's hit path is not a safe point and is unchanged.
// A module's own code being active only pins it: growth is refused, yielding
// is not. Off (0) by default.
static uint8_t sh_every, sh_tick;
void mos_cache_auto_poll(uint8_t every) { sh_every = every; sh_tick = 0; }
static void poll_point(void) {
  if (sh_every && ++sh_tick >= sh_every) {
    sh_tick = 0;
    mos_cache_service();
  }
}

// Stage 2: malloc found nothing that fits `needed` bytes (chunk size). Yield
// enough of the pool to fit it, if possible; no I/O (see above).
int __malloc_low_memory(size_t needed) {
  size_t units = needed / UNIT + 1;
  sh_hook_calls++;
  if (!pool0_p || sh_busy) return 0;
  return pool0_yield(units > 255 ? 255 : (uint8_t)units) != 0;
}

extern char host_tab[];
void mod_init(void) {
  mt_addr[HOST] = (uint16_t)host_tab;
  mt_cr[HOST] = 0x0E;
}
