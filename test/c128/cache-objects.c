#include "../test-check.h"
#include "cache-test.h"
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

static unsigned char pool0[5 * 32];

static mos_cache_handle_t o[5], big, g[64];

int main(void) {
  unsigned i, n;
  unsigned char *p;

  /* bank 1's pool: out-of-range choices are refused and change nothing */
  CHECK(mos_cache_bank1(0x0FF0, 4, 5) ==
        MOS_CACHE_BAD_ARGUMENT); /* Common RAM */
  CHECK(mos_cache_bank1(0xB000, 200, 5) ==
        MOS_CACHE_BAD_ARGUMENT); /* past $C000 */
  CHECK(mos_cache_bank1(0xFF00, 200, 10) == MOS_CACHE_BAD_ARGUMENT); /* wraps */
  CHECK(mos_cache_bank1(0x2000, 4, 4) ==
        MOS_CACHE_BAD_ARGUMENT);        /* unit size */
  CHECK(mos_cache_pool_units(1) > 100); /* still the default pool */

  CHECK(mos_cache_static(pool0, 5) == MOS_CACHE_OK);
  CHECK(mos_cache_static(pool0, 5) == MOS_CACHE_BAD_ARGUMENT); /* only once */
  CHECK(mos_cache_pool_units(0) == 5);
  /* any non-zero bank argument means bank 1 */
  CHECK(mos_cache_pool_units(2) == mos_cache_pool_units(1));
  CHECK(mos_cache_free_units(2) == mos_cache_free_units(1));
  CHECK(mos_cache_max_run(200) == mos_cache_max_run(1));
  CHECK(mos_cache_pool_base(2) == mos_cache_pool_base(1));
  CHECK(mos_cache_pool_units(1) > 100); /* the default bank-1 pool is large */
  CHECK(mos_cache_pool_base(1) >= 0x1000);

  /* five 1-unit objects fill bank 0 */
  for (i = 0; i < 5; i++) {
    o[i] = mos_cache_malloc(20);
    CHECK(o[i]);
    CHECK(fill(o[i], 20, 0x40 + i));
  }
  CHECK(mos_cache_free_units(0) == 0);
  CHECK(mos_cache_handle_bank(o[0]) == 0);

  /* a 70-byte object does not fit in bank 0: it goes to bank 1 ... */
  big = mos_cache_malloc(70);
  CHECK(big);
  CHECK(mos_cache_handle_bank(big) == 1);
  /* ... and locking it (from bank 0) spills the three oldest objects across */
  CHECK(mos_cache_stats.obj_spills == 0);
  CHECK(fill(big, 70, 0x90));
  CHECK(mos_cache_handle_bank(big) == 0);
  CHECK(mos_cache_stats.obj_spills >= 3);
  CHECK(mos_cache_handle_bank(o[0]) == 1 && mos_cache_handle_bank(o[1]) == 1 &&
        mos_cache_handle_bank(o[2]) == 1);
  /* every object's bytes survived the moves, in both directions */
  CHECK(check(big, 70, 0x90));
  for (i = 0; i < 5; i++)
    CHECK(check(o[i], 20, 0x40 + i));

  /* a locked object is pinned: it stays where it is while others move */
  p = mos_cache_lock(big);
  CHECK(p && mos_cache_handle_locks(big) == 1);
  CHECK(mos_cache_free(big) ==
        MOS_CACHE_LOCKED); /* cannot free a locked object */
  for (i = 0; i < 5; i++)
    CHECK(check(o[i], 20, 0x40 + i)); /* bring each through bank 0 in turn */
  CHECK(mos_cache_handle_bank(big) == 0 &&
        (unsigned char *)mos_cache_lock(big) == p);
  mos_cache_unlock(big);
  mos_cache_unlock(big);
  CHECK(mos_cache_handle_locks(big) == 0);

  /* invalid handles are rejected everywhere, not just by free */
  CHECK(mos_cache_free(0) == MOS_CACHE_INVALID_HANDLE);
  CHECK(mos_cache_free(33) == MOS_CACHE_INVALID_HANDLE);
  CHECK(mos_cache_lock(0) == NULL);
  CHECK(mos_cache_lock(33) == NULL);
  CHECK(mos_cache_handle_bank(0) == MOS_CACHE_INVALID_BANK &&
        mos_cache_handle_bank(33) == MOS_CACHE_INVALID_BANK);
  CHECK(mos_cache_malloc(0) == 0);
  mos_cache_unlock(0); /* ignored */
  mos_cache_unlock(33);
  CHECK(mos_cache_handle_locks(0) == 0 && mos_cache_handle_locks(33) == 0);

  /* no module table: every module id is invalid */
  CHECK(mos_cache_module_evict(0) == MOS_CACHE_NO_SUCH_MODULE);
  CHECK(mos_cache_module_load(0, 0) == MOS_CACHE_NO_SUCH_MODULE);

  /* out of memory: fill both pools with 3-unit objects, then one more fails
   * cleanly (0), the objects are intact, and freeing one makes room again */
  for (i = 0; i < 5; i++)
    mos_cache_free(o[i]);
  mos_cache_free(big);
  CHECK(mos_cache_handle_bank(big) ==
        MOS_CACHE_INVALID_BANK); /* a freed handle is invalid */
  for (n = 0; n < 64; n++) {
    g[n] = mos_cache_malloc(70);
    if (!g[n])
      break;
    CHECK(fill(g[n], 70, 0x10 + n));
  }
  CHECK(n >= 10 && n < 64);
  CHECK(mos_cache_malloc(70) == 0);
  for (i = 0; i < n; i++)
    CHECK(check(g[i], 70, 0x10 + i));
  CHECK(mos_cache_free(g[0]) == MOS_CACHE_OK);
  g[0] = mos_cache_malloc(70);
  CHECK(g[0]);
  for (i = 0; i < n; i++)
    mos_cache_free(g[i]);
  CHECK(mos_cache_free_units(0) == 5);

  /* a request larger than either pool is refused up front */
  CHECK(mos_cache_malloc(60000) == 0);
  return EXIT_SUCCESS;
}
