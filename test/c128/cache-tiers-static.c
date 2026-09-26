#include <cache.h>
#include <stdint.h>
#include <stdlib.h>

#include "../test-check.h"

/* In static mode there is no pool to resize, but the tier extension point for
 * slow-tier write-behind still gets its turn from mos_cache_service (and from
 * automatic polling), and mos_cache_tier_demote is never asked for room: only
 * the reclaim path of shared mode does that. */

static unsigned wb_calls, demote_calls;
static unsigned char pool0[4 * 32];

uint8_t mos_cache_tier_demote(uint8_t bank) {
  (void)bank;
  demote_calls++;
  return 0;
}

void mos_cache_tier_writebehind(void) { wb_calls++; }

int main(void) {
  mos_cache_handle_t h;

  CHECK(mos_cache_static(pool0, 4) == MOS_CACHE_OK);
  CHECK(mos_cache_service() == 0); /* nothing to resize */
  CHECK(wb_calls == 1);
  mos_cache_auto_poll(1);
  h = mos_cache_malloc(20);
  CHECK(h && wb_calls == 2);
  mos_cache_free(h);
  mos_cache_auto_poll(0);
  CHECK(demote_calls == 0);
  return EXIT_SUCCESS;
}
