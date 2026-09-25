#include "compat.h"

#define HOST 6

extern volatile uint8_t __mos_mt_ref[];

static uint8_t pool0[5 * 32];
/* Only the ADDRESS of the bank-1 pool is used from bank-0 code (see bank1.h). */
__attribute__((section(".c128bank1.bss"))) static uint8_t pool1[32 * 32];

uint8_t evict_log[16], evict_n; /* ids of evicted modules, in order */
void mos_cache_on_evict(uint8_t id) {
  if (evict_n < 16)
    evict_log[evict_n++] = id;
}

void mod_init_shared(void) {
  mos_cache_bank1((uint16_t)pool1, 32, 5);
  mos_cache_set_host(HOST);
}

void mod_init(void) {
  mod_init_shared();
  mos_cache_static(pool0, 5);
}

void mod_clear_refs(void) {
  uint8_t id;
  for (id = 0; id < 10; id++)
    __mos_mt_ref[id] = 0;
}
