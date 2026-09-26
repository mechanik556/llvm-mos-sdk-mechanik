#include <cache.h>
#include <stdint.h>
#include <stdlib.h>

#include "../test-check.h"
#include "cache-test.h"

/* Unit sizes are configurable per bank (mos_cache_units): here bank 0 uses
 * 64-byte units and bank 1 uses 32-byte units, the opposite of the defaults.
 * Checks that the choice is validated and takes effect: how many units an
 * object needs in each bank, how many units the default bank-1 pool has, that
 * shared mode sizes the malloc block in bank-0 units, and that the choice
 * cannot be changed once pools exist. */

static unsigned char pool0[6 * 64];

int main(void) {
  mos_cache_handle_t a, b, c;
  uint16_t free1;

  /* out-of-range shifts are refused and change nothing */
  CHECK(mos_cache_units(4, 8) == MOS_CACHE_BAD_ARGUMENT);
  CHECK(mos_cache_units(5, 11) == MOS_CACHE_BAD_ARGUMENT);
  CHECK(mos_cache_units(0, 0) == MOS_CACHE_BAD_ARGUMENT);
  CHECK(mos_cache_units(6, 5) == MOS_CACHE_OK);
  CHECK(mos_cache_units(6, 5) == MOS_CACHE_OK); /* may be repeated until used */

  /* bank 1's default pool is now in 32-byte units: about 1400 of them */
  CHECK(mos_cache_pool_units(1) > 1000);
  CHECK(mos_cache_units(6, 8) == MOS_CACHE_BAD_ARGUMENT); /* bank 1 in use */

  /* a static pool of six 64-byte units */
  CHECK(mos_cache_static(pool0, 6) == MOS_CACHE_OK);
  CHECK(mos_cache_units(6, 5) == MOS_CACHE_BAD_ARGUMENT); /* bank 0 in use */
  CHECK(mos_cache_pool_units(0) == 6);

  /* a 100-byte object needs two 64-byte units in bank 0 ... */
  a = mos_cache_malloc(100);
  CHECK(a && mos_cache_handle_bank(a) == 0);
  CHECK(mos_cache_free_units(0) == 4);
  /* ... a 64-byte one exactly one, and 65 bytes two */
  b = mos_cache_malloc(64);
  CHECK(b && mos_cache_free_units(0) == 3);
  c = mos_cache_malloc(65);
  CHECK(c && mos_cache_free_units(0) == 1);
  CHECK(fill(a, 100, 1) && fill(b, 64, 2) && fill(c, 65, 3));
  CHECK(check(a, 100, 1) && check(b, 64, 2) && check(c, 65, 3));

  /* in bank 1 the unit is 32 bytes: a 40-byte object takes two */
  free1 = mos_cache_free_units(1);
  {
    mos_cache_handle_t d = mos_cache_malloc(200); /* does not fit bank 0 */
    CHECK(d && mos_cache_handle_bank(d) == 1);
    CHECK(mos_cache_free_units(1) == free1 - 7); /* 200 bytes = 7 units of 32 */
    CHECK(mos_cache_free(d) == MOS_CACHE_OK);
    CHECK(mos_cache_free_units(1) == free1);
  }
  mos_cache_free(a);
  mos_cache_free(b);
  mos_cache_free(c);
  CHECK(mos_cache_free_units(0) == 6);
  return EXIT_SUCCESS;
}
