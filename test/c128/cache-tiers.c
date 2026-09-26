#include "../test-check.h"
#include <cache.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The extension points for tiers this library does not implement. This program
 * defines both (they are weak in the library) as test doubles and checks when
 * they are called: mos_cache_tier_demote from the malloc-reclaim path when
 * spilling to bank 1 is impossible (here bank 1 is full), and
 * mos_cache_tier_writebehind only from mos_cache_service - never from inside
 * malloc, where slow-tier I/O must not happen. The demote double "moves" an
 * object to a fictitious tier by freeing it, so the reclaim path makes progress
 * with it. */

extern char __c128bank1_free_start[];

static unsigned demote_calls, wb_calls;
static unsigned char
    lie; /* mos_cache_tier_demote claims success, frees nothing */
static mos_cache_handle_t g[8];
static unsigned char ng;

uint8_t mos_cache_tier_demote(uint8_t bank) {
  unsigned char i;
  demote_calls++;
  if (lie)
    return 1;
  if (bank != 0)
    return 0;
  for (i = 0; i < ng; i++)
    if (g[i] && mos_cache_handle_locks(g[i]) == 0 &&
        mos_cache_handle_bank(g[i]) == 0) {
      mos_cache_free(g[i]);
      g[i] = 0;
      return 1;
    }
  return 0;
}

void mos_cache_tier_writebehind(void) { wb_calls++; }

int main(void) {
  void *blk[40];
  unsigned char nblk = 0;

  __set_heap_limit(1000);
  free(malloc(1));
  /* a tiny bank-1 pool (1 unit), so that nothing can spill there */
  CHECK(mos_cache_bank1((uint16_t)__c128bank1_free_start, 1, 5) ==
        MOS_CACHE_OK);
  CHECK(mos_cache_shared(1, 4, 4, 100, 200) == MOS_CACHE_OK);
  for (ng = 0; ng < 5; ng++) {
    g[ng] = mos_cache_malloc(20);
    if (!g[ng])
      break;
  }
  CHECK(ng == 5 &&
        mos_cache_handle_bank(g[4]) == 1); /* 4 in bank 0, 1 in bank 1 */
  ng = 4;

  /* a faulty tier that claims to have freed room but has not must not make
   * malloc loop: the claim is ignored, the pool stays, malloc fails */
  lie = 1;
  for (;;) {
    void *p = malloc(60);
    if (!p)
      break;
    blk[nblk++] = p;
    if (nblk == 40)
      break;
  }
  lie = 0;
  CHECK(nblk < 40);
  CHECK(demote_calls >= 1 && mos_cache_pool_units(0) == 4);

  /* exhaust malloc: the hook cannot spill (bank 1 is full), so it asks the
   * tier to demote; the double frees objects, the pool shrinks, malloc goes on
   */
  demote_calls = 0;
  for (;;) {
    void *p = malloc(60);
    if (!p)
      break;
    blk[nblk++] = p;
    if (nblk == 40)
      break;
  }
  CHECK(demote_calls >= 1);
  CHECK(mos_cache_stats.hook_calls >= 1);
  CHECK(wb_calls == 0); /* write-behind never ran, in malloc or anywhere */

  /* polling runs write-behind, and only polling does */
  CHECK(mos_cache_pool_units(0) == 1); /* everything was given back */
  free(blk[--nblk]);
  free(blk[--nblk]);
  (void)mos_cache_service();
  CHECK(wb_calls == 1);
  mos_cache_auto_poll(1);
  (void)mos_cache_free(mos_cache_malloc(20));
  CHECK(wb_calls >= 2);
  mos_cache_auto_poll(0);
  return EXIT_SUCCESS;
}
