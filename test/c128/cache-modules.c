#include "../test-check.h"
#include <cache.h>
#include <stdint.h>
#include <stdlib.h>

/* Code modules through the library's call gate: hand-written relocatable
 * modules (cache-modules.s) are loaded on first call into a 5-unit bank-0 pool
 * (bank 1 as overflow), relocated by the load delta, evicted cold-first when
 * the pool is full, and un-relocated on eviction so that a self-modified
 * operand comes back correct when the module is reloaded at another address. */

unsigned char m_add1(unsigned char), m_cb(unsigned char), m_add7(unsigned char),
    m_r_entry(unsigned char), m_r_viaptr(unsigned char),
    m_r_lohi(unsigned char), m_r_call_sm(void), m_r_sm_to_b(void),
    m_r_try_evict(void), m_g_incr(unsigned char), m_no_such_module(void);
extern uint16_t __mos_mt_addr[];
extern uint8_t __mos_mt_cr[];
extern volatile uint8_t __mos_mt_active[];

static unsigned char pool0[5 * 32];

static int r_works(void) {
  return m_r_entry(2) == 13 && m_r_viaptr(5) == 15 && m_r_lohi(5) == 15;
}

int main(void) {
  uint16_t r_at;
  mos_cache_handle_t h;
  unsigned char *p;

  CHECK(mos_cache_static(pool0, 5) == MOS_CACHE_OK);
  mos_cache_set_host(5); /* the runtime's jump table, for module code */

  /* load on first call */
  CHECK(!(__mos_mt_addr[0] >> 8));
  CHECK(m_add1(4) == 5);
  CHECK((__mos_mt_addr[0] >> 8) && __mos_mt_cr[0] == 0x0E);
  /* nested call through the gate: B calls A */
  CHECK(m_cb(1) == 102);
  CHECK(__mos_mt_active[0] == 0 && __mos_mt_active[1] == 0);

  /* relocation: R is loaded where it lands and every listed reference points
   * there (bank 0 has 1 free unit, so an older module is evicted or R goes to
   * bank 1) */
  CHECK(r_works());
  CHECK(m_r_call_sm() == 1);
  m_r_sm_to_b();
  CHECK(m_r_call_sm() == 2);
  r_at = __mos_mt_addr[2];

  /* fill the pools so that R is evicted, then reload it elsewhere */
  CHECK(mos_cache_module_evict(2) == MOS_CACHE_OK);
  CHECK(!(__mos_mt_addr[2] >> 8));
  CHECK(mos_cache_module_evict(2) == MOS_CACHE_NOT_RESIDENT); /* not resident */
  h = mos_cache_malloc(20);
  CHECK(h && (p = mos_cache_lock(h)) != 0);
  p[0] = 1;
  CHECK(m_add7(1) == 8); /* F loads into the space that is left */
  CHECK(r_works());
  CHECK(m_r_call_sm() == 2); /* the patched operand survived evict + reload */
  CHECK(__mos_mt_addr[2] != r_at || __mos_mt_cr[2] == 0x4E ||
        mos_cache_stats.mod_evictions >= 1);
  mos_cache_unlock(h);

  /* host services: module code locks an object (which comes to the module's
   * bank), changes it and unlocks it; a module cannot evict itself while it is
   * active (the host refuses: pinned) */
  CHECK(m_g_incr((unsigned char)h) == 0);
  CHECK(mos_cache_handle_locks(h) == 0);
  p = mos_cache_lock(h);
  CHECK(p && p[0] == 2);
  mos_cache_unlock(h);
  CHECK(m_r_try_evict() == MOS_CACHE_PINNED);
  CHECK(__mos_mt_addr[2] >> 8); /* R is still resident */

  /* a module id the table does not have: the gate fails, calling nothing */
  CHECK(m_no_such_module() == MOS_CACHE_NO_SUCH_MODULE);
  CHECK(mos_cache_module_load(99, 0) == MOS_CACHE_NO_SUCH_MODULE);
  CHECK(mos_cache_module_evict(99) == MOS_CACHE_NO_SUCH_MODULE);
  CHECK(mos_cache_module_evict(5) ==
        MOS_CACHE_STATIC_MODULE); /* the host is static */
  CHECK(mos_cache_module_load(0, 0) ==
        MOS_CACHE_OK); /* already resident: nothing to do */
  CHECK(mos_cache_module_load(5, 0) == MOS_CACHE_STATIC_MODULE);

  /* module loads are not automatic-polling safe points: they run inside the
   * gate with interrupts disabled */
  {
    unsigned char svc;
    mos_cache_auto_poll(1);
    svc = mos_cache_stats.services;
    CHECK(mos_cache_module_evict(0) == MOS_CACHE_OK);
    CHECK(m_add1(1) == 2); /* loads A again */
    CHECK(mos_cache_stats.services == svc);
    mos_cache_auto_poll(0);
  }

  /* defragmentation keeps modules working (they are relocated by the move) */
  (void)mos_cache_defrag();
  CHECK(r_works() && m_add1(1) == 2 && m_cb(1) == 102);
  CHECK(mos_cache_stats.mod_loads >= 4);
  return EXIT_SUCCESS;
}
