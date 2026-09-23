#include <stdint.h>
#include <string.h>

// M0.2.2: Module Table state, two-bank allocator, and module loader.
// Tables are ordinary bank-0 RAM (the gate normalizes to bank 0 before
// touching them). mt_addr[id]'s high byte is 0 while a module is not resident.

#define NMODS 5
#define UNIT 32           // allocation granularity, bytes
#define POOL0_UNITS 5     // deliberately small so modules spill into bank 1
#define POOL1_UNITS 32

uint16_t mt_addr[NMODS];
uint8_t mt_cr[NMODS];
volatile uint8_t mt_active[NMODS];
extern const uint16_t mt_img[NMODS];   // canonical image address (bank 0)
extern const uint16_t mt_size[NMODS];  // image size in bytes
extern volatile uint8_t gt_cr;         // caller's $FF00 (zero page, gate.s)

static uint8_t pool0[POOL0_UNITS * UNIT];
// Bank-1 pool: only its ADDRESS is used from bank-0 code (never dereferenced
// there); bank 1 is written via __c128bank1_copy_region.
__attribute__((section(".c128bank1.bss"))) static uint8_t pool1[POOL1_UNITS * UNIT];

static uint8_t bitmap[2][(POOL1_UNITS + 7) / 8];
static const uint8_t pool_units[2] = {POOL0_UNITS, POOL1_UNITS};

uint8_t mod_loads;   // number of loads performed (for tests)

void __c128bank1_copy_region(char *vma, const char *lma, unsigned short size);

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

// Returns 0 on success, 1 = out of memory. Placement policy (design 11.6 pt 4):
// prefer the bank the triggering caller is executing in, else the other.
uint8_t mod_load(uint8_t id) {
  uint16_t size = mt_size[id];
  uint8_t units = (uint8_t)((size + UNIT - 1) / UNIT);
  uint8_t first = (gt_cr & 0x40) ? 1 : 0, k, bank = 0, idx = 0xFF;
  for (k = 0; k < 2; k++) {
    bank = k ? !first : first;
    idx = alloc(bank, units);
    if (idx != 0xFF) break;
  }
  if (idx == 0xFF) return 1;
  {
    uint16_t dest = (uint16_t)(bank ? pool1 : pool0) + (uint16_t)idx * UNIT;
    if (bank)
      __c128bank1_copy_region((char *)dest, (const char *)mt_img[id], size);
    else
      memcpy((void *)dest, (const void *)mt_img[id], size);
    mt_addr[id] = dest;
    mt_cr[id] = bank ? 0x4E : 0x0E;
  }
  mod_loads++;
  return 0;
}
