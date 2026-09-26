#include <stdint.h>
#include <stdio.h>
#include "compat.h"

/* Object spilling / out-of-memory test. No modules are involved, so any
 * eviction count > 0 would be a bug. Expected values in comments. */

typedef uint16_t mos_cache_handle_t;
mos_cache_handle_t mos_cache_malloc(uint16_t size);
uint8_t mos_cache_free(mos_cache_handle_t h);
void *mos_cache_lock(mos_cache_handle_t h);
void mos_cache_unlock(mos_cache_handle_t h);
uint8_t obj_bank(mos_cache_handle_t h), pool_free_units(uint8_t bank);
void mod_init(void);

static volatile uint8_t f0_full;                         /* 0: five 1-unit objects fill bank 0 */
static volatile uint8_t big_bank0, big_bank1;            /* 1 (bank 0 was full), then 0 after lock */
static volatile uint8_t sp_a, ev_a;                      /* 3 spills, 0 module evictions */
static volatile uint8_t b0, b1, b2, b3, b4;              /* 1 1 1 0 0: oldest three moved to bank 1 */
static volatile uint8_t data_ok1;                        /* 1: every object's bytes intact after spills */
static volatile uint8_t n_big, oom_malloc, oom_lock_null;/* 11, 1 (malloc returned 0), 1 (lock returned NULL) */
static volatile uint8_t free0_end, free1_end, data_ok2;  /* 2 2 (fragmented: 4 free but no 3-unit run), 1 */
static volatile uint8_t spills_before_oom, spills_after_oom, freed_ok;

static mos_cache_handle_t o[5], big, g[16];

static uint8_t fill(mos_cache_handle_t h, uint8_t n, uint8_t seed) {
  uint8_t *p = mos_cache_lock(h), i;
  if (!p) return 0;
  for (i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7);
  mos_cache_unlock(h);
  return 1;
}
static uint8_t check(mos_cache_handle_t h, uint8_t n, uint8_t seed) {
  uint8_t *p = mos_cache_lock(h), i, ok = 1;
  if (!p) return 0;
  for (i = 0; i < n; i++) if (p[i] != (uint8_t)(seed + i * 7)) ok = 0;
  mos_cache_unlock(h);
  return ok;
}

int main(void) {
  uint8_t i, ok;
  mod_init();
  for (i = 0; i < 5; i++) { o[i] = mos_cache_malloc(20); fill(o[i], 20, 0x40 + i); }
  f0_full = pool_free_units(0);                /* 0 */
  big = mos_cache_malloc(70);              /* bank 0 full -> bank 1 */
  fill(big, 70, 0x90);                         /* locking from bank 0 migrates it here... */
  big_bank1 = obj_bank(big);
  mos_cache_unlock(big);
  /* fill() above already moved `big` into bank 0 by spilling the oldest objects: */
  big_bank0 = obj_bank(big);                   /* 0 */
  sp_a = obj_spills;                           /* 3 */
  ev_a = mod_evictions;                        /* 0 */
  b0 = obj_bank(o[0]); b1 = obj_bank(o[1]); b2 = obj_bank(o[2]);
  b3 = obj_bank(o[3]); b4 = obj_bank(o[4]);    /* 1 1 1 0 0 */
  ok = check(big, 70, 0x90);
  for (i = 0; i < 5; i++) ok &= check(o[i], 20, 0x40 + i);   /* moves things back and forth */
  data_ok1 = ok;                               /* 1 */

  /* out of memory: free everything, then fill both pools with 3-unit objects
   * WITHOUT touching them: g[0] lands in bank 0, g[1..10] in bank 1 */
  for (i = 0; i < 5; i++) mos_cache_free(o[i]);
  mos_cache_free(big);
  for (i = 0; i < 16; i++) {
    g[i] = mos_cache_malloc(70);
    if (!g[i]) break;
  }
  n_big = i;                                   /* 11 */
  oom_malloc = (i < 16 && g[i] == 0);          /* 1: no 3-unit run left in either bank */
  free0_end = pool_free_units(0);              /* 2 */
  free1_end = pool_free_units(1);              /* 2  (4 free in total, but split) */
  fill(g[0], 70, 0x10);                        /* bank 0: no migration needed */
  spills_before_oom = obj_spills;
  /* g[1] is in bank 1; locking it from bank 0 needs 3 units there: bank 0 has
   * 2 free and its only object (g[0]) cannot spill (bank 1 has 2 free) -> NULL,
   * nothing lost */
  oom_lock_null = (mos_cache_lock(g[1]) == 0);/* 1 */
  spills_after_oom = obj_spills;               /* == spills_before_oom */
  data_ok2 = check(g[0], 70, 0x10);            /* 1: g[0] intact */
  /* recovery: free one bank-1 object; now g[0] can spill and g[1] can come over */
  mos_cache_free(g[10]);
  freed_ok = fill(g[1], 70, 0x33) && check(g[1], 70, 0x33) && check(g[0], 70, 0x10); /* 1 */
  printf("spill: %d/%d ok=%d %d\n", sp_a, ev_a, data_ok1, freed_ok);
  return 0;
}
