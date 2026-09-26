#include <cache.h>
#include <stdint.h>
#include <stdlib.h>

#include "../test-check.h"

/* The default runtime indexes pool units with a byte, so a pool has at most 248
 * units. That is enough for all of bank 1 in 256-byte units (the default), and
 * asking for smaller bank-1 units over the whole region is refused (the wide
 * build, libcache-wide.a, allows it: see cache-units). Smaller units are still
 * possible in an explicit region small enough for 248 units. */

static unsigned char pool0[8 * 32];
extern char __c128bank1_free_start[];

int main(void) {
  mos_cache_handle_t h;
  uint16_t base = (uint16_t)__c128bank1_free_start;

  /* bank-1 units under 256 bytes would need over 248 units for the whole region
   */
  CHECK(mos_cache_units(5, 5) == MOS_CACHE_BAD_ARGUMENT);
  CHECK(mos_cache_units(5, 6) == MOS_CACHE_BAD_ARGUMENT);
  CHECK(mos_cache_units(5, 7) == MOS_CACHE_BAD_ARGUMENT);
  CHECK(mos_cache_units(6, 8) == MOS_CACHE_OK);
  CHECK(mos_cache_units(5, 9) == MOS_CACHE_OK);
  CHECK(mos_cache_units(5, 8) == MOS_CACHE_OK);

  /* bank 0 too: at most 248 units */
  CHECK(mos_cache_static(pool0, 249) == MOS_CACHE_BAD_ARGUMENT);
  CHECK(mos_cache_static(pool0, 8) == MOS_CACHE_OK);

  /* an explicit bank-1 region: 32-byte units are fine up to 248 of them */
  CHECK(mos_cache_bank1(base, 249, 5) == MOS_CACHE_BAD_ARGUMENT);
  CHECK(mos_cache_bank1(base, 248, 5) == MOS_CACHE_OK);
  CHECK(mos_cache_pool_units(1) == 248);

  h = mos_cache_malloc(20);
  CHECK(h && mos_cache_free(h) == MOS_CACHE_OK);
  CHECK(mos_cache_free_units(1) == 248 && mos_cache_free_units(0) == 8);
  return EXIT_SUCCESS;
}
