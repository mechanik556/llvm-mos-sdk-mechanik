#include <cache.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The bank-aware object heap in static mode: bank 0's pool is a caller-provided
 * array (5 units of 32 bytes), bank 1's pool the default (everything above the
 * statically placed bank-1 content). Objects fill bank 0, then go to bank 1;
 * locking one from bank 0 brings it into bank 0 by spilling the least recently
 * locked unlocked objects the other way; a locked object never moves; running
 * out of room returns 0/NULL and loses nothing; freeing recovers the space. */

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c))                                                                  \
      return EXIT_FAILURE;                                                     \
  } while (0)

static unsigned char pool0[5 * 32];

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

static mos_handle_t o[5], big, g[64];

int main(void) {
  unsigned i, n;
  unsigned char *p;

  CHECK(mos_cache_static(pool0, 5) == 0);
  CHECK(mos_cache_static(pool0, 5) == 1); /* only once */
  CHECK(mos_cache_pool_units(0) == 5);
  CHECK(mos_cache_pool_units(1) > 100); /* the default bank-1 pool is large */
  CHECK(mos_cache_pool_base(1) >= 0x1000);

  /* five 1-unit objects fill bank 0 */
  for (i = 0; i < 5; i++) {
    o[i] = mos_cacheable_malloc(20);
    CHECK(o[i]);
    CHECK(fill(o[i], 20, 0x40 + i));
  }
  CHECK(mos_cache_free_units(0) == 0);
  CHECK(mos_handle_bank(o[0]) == 0);

  /* a 70-byte object does not fit in bank 0: it goes to bank 1 ... */
  big = mos_cacheable_malloc(70);
  CHECK(big);
  CHECK(mos_handle_bank(big) == 1);
  /* ... and locking it (from bank 0) spills the three oldest objects across */
  CHECK(mos_cache_stats.obj_spills == 0);
  CHECK(fill(big, 70, 0x90));
  CHECK(mos_handle_bank(big) == 0);
  CHECK(mos_cache_stats.obj_spills >= 3);
  CHECK(mos_handle_bank(o[0]) == 1 && mos_handle_bank(o[1]) == 1 &&
        mos_handle_bank(o[2]) == 1);
  /* every object's bytes survived the moves, in both directions */
  CHECK(check(big, 70, 0x90));
  for (i = 0; i < 5; i++)
    CHECK(check(o[i], 20, 0x40 + i));

  /* a locked object is pinned: it stays where it is while others move */
  p = mos_handle_lock(big);
  CHECK(p && mos_handle_locks(big) == 1);
  CHECK(mos_cacheable_free(big) == 1); /* cannot free a locked object */
  for (i = 0; i < 5; i++)
    CHECK(check(o[i], 20, 0x40 + i)); /* bring each through bank 0 in turn */
  CHECK(mos_handle_bank(big) == 0 &&
        (unsigned char *)mos_handle_lock(big) == p);
  mos_handle_unlock(big);
  mos_handle_unlock(big);
  CHECK(mos_handle_locks(big) == 0);

  /* invalid handles are rejected everywhere, not just by free */
  CHECK(mos_cacheable_free(0) == 2);
  CHECK(mos_cacheable_free(33) == 2);
  CHECK(mos_handle_lock(0) == NULL);
  CHECK(mos_handle_lock(33) == NULL);
  CHECK(mos_handle_bank(0) == 0xFF && mos_handle_bank(33) == 0xFF);
  CHECK(mos_cacheable_malloc(0) == 0);
  mos_handle_unlock(0); /* ignored */
  mos_handle_unlock(33);
  CHECK(mos_handle_locks(0) == 0 && mos_handle_locks(33) == 0);

  /* no module table: every module id is invalid */
  CHECK(mos_cache_module_evict(0) == 4);
  CHECK(mos_cache_module_load(0, 0) == 3);

  /* out of memory: fill both pools with 3-unit objects, then one more fails
   * cleanly (0), the objects are intact, and freeing one makes room again */
  for (i = 0; i < 5; i++)
    mos_cacheable_free(o[i]);
  mos_cacheable_free(big);
  CHECK(mos_handle_bank(big) == 0xFF); /* a freed handle is invalid */
  for (n = 0; n < 64; n++) {
    g[n] = mos_cacheable_malloc(70);
    if (!g[n])
      break;
    CHECK(fill(g[n], 70, 0x10 + n));
  }
  CHECK(n >= 10 && n < 64);
  CHECK(mos_cacheable_malloc(70) == 0);
  for (i = 0; i < n; i++)
    CHECK(check(g[i], 70, 0x10 + i));
  CHECK(mos_cacheable_free(g[0]) == 0);
  g[0] = mos_cacheable_malloc(70);
  CHECK(g[0]);
  for (i = 0; i < n; i++)
    mos_cacheable_free(g[i]);
  CHECK(mos_cache_free_units(0) == 5);

  /* a request larger than either pool is refused up front */
  CHECK(mos_cacheable_malloc(60000) == 0);
  return EXIT_SUCCESS;
}
