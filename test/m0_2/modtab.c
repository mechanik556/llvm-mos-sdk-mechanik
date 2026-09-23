#include <stdint.h>
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

#define NMODS 9
#define UNIT 32           // allocation granularity, bytes
#define POOL0_UNITS 5     // deliberately small so modules spill into bank 1
#define POOL1_UNITS 32
#define HOST 6            // static, always-resident module (the "base program")

uint16_t mt_addr[NMODS];
uint8_t mt_cr[NMODS];
volatile uint8_t mt_active[NMODS];
static uint16_t mt_stamp[NMODS];   // load order (FIFO-ish stand-in for LRU)
static uint16_t stamp_clock;
extern const uint16_t mt_img[NMODS];    // canonical image address (bank 0)
extern const uint16_t mt_size[NMODS];   // image size in bytes
extern const uint16_t mt_reloc[NMODS];  // relocation table address
extern volatile uint8_t gt_cr;          // caller's $FF00 (zero page, gate.s)

static uint8_t pool0[POOL0_UNITS * UNIT];
// Bank-1 pool: only its ADDRESS is used from bank-0 code (never dereferenced
// there); bank 1 is accessed via the __c128bank1_* helpers.
__attribute__((section(".c128bank1.bss"))) static uint8_t pool1[POOL1_UNITS * UNIT];

static uint8_t bitmap[2][(POOL1_UNITS + 7) / 8];
static const uint8_t pool_units[2] = {POOL0_UNITS, POOL1_UNITS};

uint8_t mod_loads, mod_evictions;   // counters (for tests)

void __c128bank1_copy_region(char *vma, const char *lma, unsigned short size);
void __c128bank1_read(char *dest, const char *src, unsigned short size);

static uint16_t pool_base(uint8_t bank) { return bank ? (uint16_t)pool1 : (uint16_t)pool0; }
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
  if (bank) __c128bank1_read((char *)&v, (const char *)a, 1);
  else v = *(volatile uint8_t *)a;
  return v;
}
static void set8(uint8_t bank, uint16_t a, uint8_t v) {
  if (bank) __c128bank1_copy_region((char *)a, (const char *)&v, 1);
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
  if (bank) __c128bank1_read((char *)mt_img[id], (const char *)addr, size);
  else memcpy((void *)mt_img[id], (const void *)addr, size);
  release(bank, (uint8_t)((addr - pool_base(bank)) / UNIT), units);
  mt_addr[id] = 0;
  mt_cr[id] = 0;
  mod_evictions++;
  return 0;
}

// Oldest resident, non-static, inactive module in `bank`, or 0xFF.
static uint8_t find_victim(uint8_t bank) {
  uint8_t id, best = 0xFF;
  for (id = 0; id < NMODS; id++) {
    if (!mt_img[id] || !(mt_addr[id] >> 8) || mt_active[id]) continue;
    if ((mt_cr[id] == 0x4E) != bank) continue;
    if (best == 0xFF || mt_stamp[id] < mt_stamp[best]) best = id;
  }
  return best;
}

// Returns 0 on success, 1 = out of memory. Placement (design 11.6 pt 4):
// prefer the bank the triggering caller runs in, else the other - and only if
// NEITHER bank has room, evict the oldest unpinned module from the preferred
// bank (then the other) until the module fits.
uint8_t mod_load(uint8_t id) {
  uint16_t size = mt_size[id];
  uint8_t units = units_of(size);
  uint8_t first = (gt_cr & 0x40) ? 1 : 0, pass, k, bank = 0, idx = 0xFF;
  for (pass = 0; pass < 2 && idx == 0xFF; pass++) {
    for (k = 0; k < 2 && idx == 0xFF; k++) {
      bank = k ? !first : first;
      if (units > pool_units[bank]) continue;
      for (;;) {
        uint8_t victim;
        idx = alloc(bank, units);
        if (idx != 0xFF || !pass) break;   /* pass 0: free space only */
        victim = find_victim(bank);
        if (victim == 0xFF) break;
        mod_evict(victim);
      }
    }
  }
  if (idx == 0xFF) return 1;
  {
    uint16_t dest = pool_base(bank) + (uint16_t)idx * UNIT;
    if (bank) __c128bank1_copy_region((char *)dest, (const char *)mt_img[id], size);
    else memcpy((void *)dest, (const void *)mt_img[id], size);
    apply_relocs(bank, dest, mt_reloc[id], (uint16_t)(dest - mt_img[id]));
    mt_addr[id] = dest;
    mt_cr[id] = bank ? 0x4E : 0x0E;
  }
  mt_stamp[id] = ++stamp_clock;
  mod_loads++;
  return 0;
}

// Host (static) module: base-program code exposed through the gate like any
// module, so module code can call back into ordinary bank-0 functions.
uint8_t host_try_evict(uint8_t id) { return mod_evict(id); }

void mod_init(void) {
  mt_addr[HOST] = (uint16_t)host_try_evict;
  mt_cr[HOST] = 0x0E;
}
