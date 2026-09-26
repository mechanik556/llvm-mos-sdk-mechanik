#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

/* Shared mode: bank 0's pool is a block of the ordinary malloc heap, and the
 * two heaps collaborate (design 11.11). Flags are dumped from VICE; every
 * "ok" flag must be 1, counters are noted in the comments. */

typedef uint16_t mos_cache_handle_t;
mos_cache_handle_t mos_cache_malloc(uint16_t size);
uint8_t mos_cache_free(mos_cache_handle_t h);
void *mos_cache_lock(mos_cache_handle_t h);
void mos_cache_unlock(mos_cache_handle_t h);
uint8_t mos_cache_service(void);
void mod_init(void);
unsigned char m_add1(unsigned char), m_r_call_sm(void), m_r_sm_to_b(void), m_r_entry(unsigned char),
    m_r_viaptr(unsigned char), m_r_lohi(unsigned char);
size_t __set_heap_limit(size_t limit);
size_t __heap_bytes_free(void);
extern volatile uint16_t mt_addr[10];
extern volatile uint8_t mt_cr[10];

#define MIN 3
#define INIT 6
#define MAX 12
#define LOW 200
#define HIGH 300

static volatile uint8_t en, pool_sz0, a1, r1;                    /* 0, 6, 5, 1 */
static volatile uint8_t bank0_full;                              /* 1: 6 of 6 units used */
static volatile uint8_t more_than_static, all_tags_ok1;          /* 1, 1: yielded pool bytes were usable */
static volatile uint8_t hooked, pool_min, yielded3, oom_end;     /* 1, 3 (min), 3, 1: malloc failed at the end */
static volatile uint8_t disjoint1, r_still, a_dropped, spilled;  /* 1, 1, 1: A dropped, 1: objects in bank 1 */
static volatile uint8_t r_reloc_ok1;                             /* 1: R relocated by the in-hook defragmentation */
static volatile uint8_t cache_full, tags_ok2, malloc_still;      /* 1, 1, 1 */
static volatile uint8_t exact_free1;                             /* 1: every byte accounted for after all frees */
static volatile uint8_t adjacent, moved, r_reloc_ok2, r_sm_ok;   /* 1, 1, 1, 1 */
static volatile uint8_t objs_ok, a_reload, pool_after1;          /* 1, 1, 7 */
static volatile uint8_t pin_refused, pin_size, pin_moves_same;   /* 1, 7, 1 */
static volatile uint8_t after_unpin, at_max, want_cleared;       /* 11, 12, 1 */
static volatile uint8_t polite_shrunk, no_hook_used, low_ok, no_failed; /* 1, 1, 1, 1 */
static volatile uint8_t exact_free2, end_ok;                     /* 1, 1 */
static volatile uint8_t d_moves, d_evictions, d_yielded, d_grows, d_hooks;

static void *blk[48];
static uint8_t nblk;
static mos_cache_handle_t o1, o2, g[16];

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
static uint8_t tags_ok(void) {
  uint8_t i, j, ok = 1;
  for (i = 0; i < nblk; i++)
    for (j = 0; j < 60; j++)
      if (((uint8_t *)blk[i])[j] != (uint8_t)(0x30 + i)) ok = 0;
  return ok;
}
static uint8_t outside_pool(void) {     /* no ordinary block overlaps the cache's pool */
  uint8_t i, ok = 1;
  uint16_t lo = pool0_base(), hi = lo + (uint16_t)pool0_size() * 32;
  for (i = 0; i < nblk; i++) {
    uint16_t b = (uint16_t)blk[i];
    if (b + 60 > lo && b < hi) ok = 0;
  }
  return ok;
}
static void free_blocks(void) {
  while (nblk) free(blk[--nblk]);
}
static uint8_t r_works(void) {
  return m_r_entry(2) == 13 && m_r_viaptr(5) == 15 && m_r_lohi(5) == 15;
}

int main(void) {
  size_t F0, cost_init, n_static, cost, fr;
  uint8_t i, n0, base_moves;
  uint16_t base0;
  void *adj;

  __set_heap_limit(1200);
  free(malloc(1));                                           /* initialize the heap: only then is its free space known */
  mod_init_shared();
  F0 = __heap_bytes_free();
  en = mos_cache_shared(MIN, INIT, MAX, LOW, HIGH);
  cost_init = F0 - __heap_bytes_free();                      /* the pool block's cost */
  pool_sz0 = pool0_size();

  /* bank 0's pool: A (1 unit), R (3), two objects (1 each) = full */
  a1 = m_add1(4);
  r1 = m_r_call_sm();
  o1 = mos_cache_malloc(20);
  o2 = mos_cache_malloc(20);
  fill(o1, 20, 0x11);
  fill(o2, 20, 0x22);
  bank0_full = pool_free_units(0) == 0 && obj_bank(o1) == 0 && obj_bank(o2) == 0;

  /* ---- Stage 2: malloc exhaustion makes the cache give up bank 0 ---- */
  n_static = (F0 - cost_init) / 62;                          /* blocks that fit with a static-size pool */
  for (nblk = 0; nblk < 48; ) {
    void *p = malloc(60);
    if (!p) break;
    memset(p, 0x30 + nblk, 60);
    blk[nblk++] = p;
  }
  more_than_static = nblk > n_static;
  all_tags_ok1 = tags_ok();
  hooked = sh_hook_calls > 0;
  pool_min = pool0_size();
  yielded3 = sh_yielded;
  oom_end = nblk < 48;
  disjoint1 = outside_pool();
  a_dropped = !(mt_addr[0] >> 8);                            /* dropped (clean) */
  spilled = obj_bank(o1) == 1 && obj_bank(o2) == 1;
  r_still = (mt_addr[5] >> 8) != 0 && mt_cr[5] == 0x0E;
  r_reloc_ok1 = r_works() && m_r_call_sm() != 0;            /* slid down by the defragmentation in the hook */
  d_evictions = mod_evictions; d_yielded = sh_yielded; d_hooks = sh_hook_calls;

  /* ---- the other direction: exhausting the cache leaves malloc's data alone ---- */
  for (i = 0; i < 16; i++) {
    g[i] = mos_cache_malloc(70);
    if (!g[i]) break;
  }
  cache_full = i < 16;                                       /* bank 1 ran out (10 objects) */
  tags_ok2 = tags_ok() && outside_pool();
  free(blk[--nblk]);                                         /* and malloc keeps working */
  {
    void *p = malloc(60);
    malloc_still = p != 0;
    if (p) { memset(p, 0x30 + nblk, 60); blk[nblk++] = p; }
  }
  for (i = 0; i < 16; i++) if (g[i]) mos_cache_free(g[i]);
  free_blocks();
  fr = __heap_bytes_free();
  exact_free1 = (F0 - fr) == cost_init - (size_t)(INIT - MIN) * 32;

  /* ---- Stage 1: polling grows the pool; when the block must move, modules
   * are relocated and objects follow ---- */
  adj = malloc(20);                                          /* sits right behind the pool block */
  adjacent = (uint16_t)adj == pool0_base() + MIN * 32 + 2;
  g[0] = mos_cache_malloc(70);                           /* bank 0 is full: wants to grow */
  base0 = pool0_base();
  base_moves = sh_moves;
  mos_cache_service();
  pool_after1 = pool0_size();                                /* 3 + 4 = 7 */
  moved = sh_moves == base_moves + 1 && pool0_base() != base0;
  r_reloc_ok2 = r_works();
  m_r_call_sm();
  m_r_sm_to_b();
  r_sm_ok = m_r_call_sm() == 2 && r_works();
  a_reload = m_add1(9) == 10;                                /* A reloads into the new space */
  objs_ok = check(o1, 20, 0x11) && check(o2, 20, 0x22) && g[0] && fill(g[0], 70, 0x55) && check(g[0], 70, 0x55);

  /* a locked object pins the pool: growing is refused rather than moving it */
  {
    uint8_t *p = mos_cache_lock(o1);
    uint8_t sz = pool0_size(), mv = sh_moves;
    (void)p;
    pin_refused = obj_bank(o1) == 0 && !mos_cache_service();
    pin_size = pool0_size();
    pin_moves_same = pool0_size() == sz && sh_moves == mv;
    mos_cache_unlock(o1);
  }
  mos_cache_service();
  after_unpin = pool0_size();                                /* 11 */
  mos_cache_service();
  at_max = pool0_size();                                     /* 12 */
  want_cleared = !mos_cache_service();                       /* nothing more to do */

  /* ---- Stage 1, the other way: the heap runs low, the cache yields before
   * any allocation fails (the reclaim hook is not used) ---- */
  free(adj);
  n0 = pool0_size();
  d_hooks = sh_hook_calls;
  for (nblk = 0; nblk < 48 && __heap_bytes_free() >= LOW; ) {
    void *p = malloc(60);
    if (!p) break;
    memset(p, 0x30 + nblk, 60);
    blk[nblk++] = p;
  }
  low_ok = __heap_bytes_free() < LOW;                          /* below the low watermark, nothing has failed yet */
  mos_cache_service();
  polite_shrunk = pool0_size() < n0;
  no_hook_used = sh_hook_calls == d_hooks;
  no_failed = tags_ok() && outside_pool() && __heap_bytes_free() >= LOW;
  d_moves = sh_moves; d_grows = sh_grows;

  free_blocks();
  fr = __heap_bytes_free();
  cost = F0 - fr;
  /* the pool block may carry a few bytes of slack: a grow that leaves a
   * remainder too small for a chunk of its own keeps the remainder */
  exact_free2 = cost >= (size_t)pool0_size() * 32 + 2 && cost < (size_t)pool0_size() * 32 + 2 + 8;
  end_ok = check(o1, 20, 0x11) && check(g[0], 70, 0x55) && r_works();
  return 0;
}
