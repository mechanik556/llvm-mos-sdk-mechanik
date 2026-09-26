#include <cache.h>
#include <stdint.h>
#include <stdlib.h>

#include "../test-check.h"

/* Shared mode with a non-default bank-0 unit size (128 bytes): the pool block
 * taken from malloc is init_units * 128 bytes, and the pool gives it back to
 * malloc in units of 128 bytes when malloc runs out. */

int main(void) {
  size_t F0, cost;
  void *blk[16];
  unsigned char n = 0;

  __set_heap_limit(1000);
  free(malloc(1));
  F0 = __heap_bytes_free();

  CHECK(mos_cache_units(7, 8) == MOS_CACHE_OK);
  CHECK(mos_cache_shared(1, 4, 6, 200, 300) == MOS_CACHE_OK);
  cost = F0 - __heap_bytes_free();
  CHECK(cost >= 4 * 128 && cost < 4 * 128 + 8); /* the block, and its header */
  CHECK(mos_cache_pool_units(0) == 4);

  /* malloc runs out: the pool shrinks, one 128-byte unit at a time */
  for (n = 0; n < 16; n++) {
    blk[n] = malloc(100);
    if (!blk[n])
      break;
  }
  CHECK(n > 3 && n < 16);
  CHECK(mos_cache_stats.hook_calls > 0);
  CHECK(mos_cache_pool_units(0) == 1);
  CHECK(mos_cache_stats.yielded_units == 3);
  while (n)
    free(blk[--n]);
  cost = F0 - __heap_bytes_free();
  CHECK(cost >= 128 && cost < 128 + 8);
  return EXIT_SUCCESS;
}
