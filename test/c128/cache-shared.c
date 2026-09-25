#include <cache.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Shared mode: bank 0's pool is one block of the ordinary malloc heap and the
 * two heaps collaborate. Objects only (no code modules). Checks: malloc
 * exhaustion makes the pool give space back through __malloc_low_memory
 * (objects spill to bank 1, the pool shrinks in place); exhausting the object
 * heap does not disturb malloc's blocks; the pool grows when there is room and
 * the block moves if it must, with objects following; a locked object pins the
 * pool; polling yields before any allocation fails; automatic polling from the
 * runtime's own safe points; and every byte is accounted for. */

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c))                                                                  \
      return EXIT_FAILURE;                                                     \
  } while (0)

extern char __c128bank1_free_start[];
size_t __set_heap_limit(size_t limit);
size_t __heap_bytes_free(void);

#define MIN 2
#define INIT 6
#define MAX 12
#define LOW 200
#define HIGH 300

static void *blk[48];
static unsigned char nblk;
static mos_handle_t o1, o2, g[16];

static int fill(mos_handle_t h, unsigned n, unsigned char seed) {
  unsigned char *p = mos_handle_lock(h);
  unsigned i;
  if (!p)
    return 0;
  for (i = 0; i < n; i++)
    p[i] = (unsigned char)(seed + i * 7);
  mos_handle_unlock(h);
  return 1;
}
static int check(mos_handle_t h, unsigned n, unsigned char seed) {
  unsigned char *p = mos_handle_lock(h);
  unsigned i;
  int ok = 1;
  if (!p)
    return 0;
  for (i = 0; i < n; i++)
    if (p[i] != (unsigned char)(seed + i * 7))
      ok = 0;
  mos_handle_unlock(h);
  return ok;
}
static int tags_ok(void) {
  unsigned char i, j;
  for (i = 0; i < nblk; i++)
    for (j = 0; j < 60; j++)
      if (((unsigned char *)blk[i])[j] != (unsigned char)(0x30 + i))
        return 0;
  return 1;
}
static int outside_pool(void) { /* no ordinary block overlaps the pool */
  uint16_t lo = mos_cache_pool_base(0),
           hi = lo + (uint16_t)mos_cache_pool_units(0) * 32;
  unsigned char i;
  for (i = 0; i < nblk; i++) {
    uint16_t b = (uint16_t)blk[i];
    if (b + 60 > lo && b < hi)
      return 0;
  }
  return 1;
}
static void free_blocks(void) {
  while (nblk)
    free(blk[--nblk]);
}
static int fill_heap_until(size_t free_bytes) {
  while (nblk < 48 && __heap_bytes_free() >= free_bytes) {
    void *p = malloc(60);
    if (!p)
      return 0;
    memset(p, 0x30 + nblk, 60);
    blk[nblk++] = p;
  }
  return 1;
}

int main(void) {
  size_t F0, cost_init, n_static, fr, cost;
  unsigned char i, base_moves, before, n;
  uint16_t base0;
  void *adj;

  __set_heap_limit(1200);
  free(malloc(1)); /* initialize the heap: only then is its free space known */
  F0 = __heap_bytes_free();

  /* a small bank-1 pool (10 units of 32 bytes) so that it can be exhausted */
  CHECK(mos_cache_bank1((uint16_t)__c128bank1_free_start, 10, 5) == 0);
  CHECK(mos_cache_shared(0, INIT, MAX, LOW, HIGH) == 1); /* bad arguments */
  CHECK(mos_cache_shared(MIN, INIT, MAX, LOW, HIGH) == 0);
  CHECK(mos_cache_shared(MIN, INIT, MAX, LOW, HIGH) == 1); /* only once */
  cost_init = F0 - __heap_bytes_free(); /* the pool block's cost */
  CHECK(mos_cache_pool_units(0) == INIT);

  /* bank 0's pool: six 1-unit objects = full */
  for (i = 0; i < 6; i++) {
    g[i] = mos_cacheable_malloc(20);
    CHECK(g[i] && fill(g[i], 20, 0x11 * (i + 1)));
  }
  CHECK(mos_cache_free_units(0) == 0);

  /* ---- malloc exhaustion makes the cache give up bank 0 ---- */
  n_static = (F0 - cost_init) / 62; /* blocks that fit with a fixed-size pool */
  CHECK(!fill_heap_until(0));       /* until malloc fails */
  CHECK(nblk > n_static);
  CHECK(mos_cache_stats.hook_calls > 0);
  CHECK(mos_cache_pool_units(0) == MIN);
  CHECK(mos_cache_stats.yielded_units == INIT - MIN);
  CHECK(tags_ok() && outside_pool());
  CHECK(mos_cache_stats.obj_spills >= 4); /* objects went to bank 1 */
  for (i = 0; i < 6; i++)
    CHECK(check(g[i], 20, 0x11 * (i + 1)));

  /* ---- the other direction: exhausting the cache leaves malloc alone ---- */
  for (n = 6; n < 16; n++) {
    g[n] = mos_cacheable_malloc(70);
    if (!g[n])
      break;
  }
  CHECK(n > 6 && n < 16); /* bank 1 ran out first */
  CHECK(tags_ok() && outside_pool());
  free(blk[--nblk]);
  {
    void *p = malloc(60);
    CHECK(p);
    memset(p, 0x30 + nblk, 60);
    blk[nblk++] = p;
  }
  for (i = 6; i < n; i++)
    mos_cacheable_free(g[i]);
  free_blocks();
  fr = __heap_bytes_free();
  CHECK((F0 - fr) == cost_init - (size_t)(INIT - MIN) * 32);

  /* ---- polling grows the pool; when the block must move, objects follow ----
   */
  adj = malloc(20); /* sits right behind the pool block */
  CHECK((uint16_t)adj == mos_cache_pool_base(0) + MIN * 32 + 2);
  o1 = mos_cacheable_malloc(50);
  CHECK(o1 && fill(o1, 50, 0x55)); /* bank 0 is full: wants to grow */
  base0 = mos_cache_pool_base(0);
  base_moves = mos_cache_stats.pool_moves;
  CHECK(mos_cache_service() == 1);
  CHECK(mos_cache_pool_units(0) == MIN + 4);
  CHECK(mos_cache_stats.pool_moves == base_moves + 1);
  CHECK(mos_cache_pool_base(0) != base0);
  for (i = 0; i < 6; i++)
    CHECK(check(g[i], 20, 0x11 * (i + 1)));
  CHECK(check(o1, 50, 0x55));

  /* a locked object pins the pool: growing is refused rather than moving it */
  {
    unsigned char *p = mos_handle_lock(g[0]);
    unsigned char sz = mos_cache_pool_units(0), mv = mos_cache_stats.pool_moves;
    CHECK(p && mos_handle_bank(g[0]) == 0);
    CHECK(mos_cache_service() == 0);
    CHECK(mos_cache_pool_units(0) == sz && mos_cache_stats.pool_moves == mv);
    mos_handle_unlock(g[0]);
  }
  CHECK(mos_cache_service() == 1);
  CHECK(mos_cache_pool_units(0) == MIN + 8); /* four units per call */
  CHECK(mos_cache_service() == 1);
  CHECK(mos_cache_pool_units(0) == MAX);
  CHECK(mos_cache_service() == 0); /* at the maximum: nothing more to do */

  /* ---- the heap runs low: yield before any allocation fails ---- */
  free(adj);
  n = mos_cache_pool_units(0);
  before = mos_cache_stats.hook_calls;
  CHECK(fill_heap_until(LOW) && __heap_bytes_free() < LOW);
  CHECK(mos_cache_service() == 1);
  CHECK(mos_cache_pool_units(0) < n);
  CHECK(mos_cache_stats.hook_calls == before); /* the hook was not needed */
  CHECK(tags_ok() && outside_pool() && __heap_bytes_free() >= LOW);
  free_blocks();

  /* ---- automatic polling from the runtime's own safe points ---- */
  {
    unsigned char a = mos_cache_pool_units(0);
    unsigned char svc = mos_cache_stats.services;
    CHECK(fill_heap_until(LOW) && __heap_bytes_free() < LOW);
    (void)mos_cacheable_free(mos_cacheable_malloc(20)); /* polling is off */
    CHECK(mos_cache_pool_units(0) == a && mos_cache_stats.services == svc);
    mos_cache_auto_poll(1);
    (void)mos_cacheable_free(
        mos_cacheable_malloc(20)); /* a safe point: yields */
    CHECK(mos_cache_pool_units(0) < a);
    CHECK(__heap_bytes_free() >= LOW);
    mos_cache_auto_poll(0);
    free_blocks();
  }

  /* every byte is accounted for; the objects survived it all */
  fr = __heap_bytes_free();
  cost = F0 - fr;
  CHECK(cost >= (size_t)mos_cache_pool_units(0) * 32 + 2 &&
        cost < (size_t)mos_cache_pool_units(0) * 32 + 2 + 8);
  for (i = 0; i < 6; i++)
    CHECK(check(g[i], 20, 0x11 * (i + 1)));
  CHECK(check(o1, 50, 0x55));
  return EXIT_SUCCESS;
}
