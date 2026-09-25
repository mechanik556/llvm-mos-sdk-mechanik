#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Automatic polling: with mos_cache_auto_poll(1), the runtime's own safe points
 * (object allocation, handle lock, module load) run mos_cache_service, so the
 * cache yields to a low heap and regrows into a spare one with no explicit
 * call. This program never calls mos_cache_service. Every flag must be 1. */

typedef uint16_t mos_handle_t;
mos_handle_t mos_cacheable_malloc(uint16_t size);
uint8_t mos_cacheable_free(mos_handle_t h);
void *mos_handle_lock(mos_handle_t h);
void mos_handle_unlock(mos_handle_t h);
uint8_t obj_bank(mos_handle_t h);
uint8_t pool0_size(void);
uint8_t mos_cache_shared(uint8_t min_units, uint8_t init_units, uint8_t max_units, uint16_t low, uint16_t high);
void mos_cache_auto_poll(uint8_t every);
void mod_init(void);
unsigned char m_add1(unsigned char);
size_t __set_heap_limit(size_t limit);
size_t __heap_bytes_free(void);
extern volatile uint8_t sh_grows, sh_hook_calls, sh_services;

#define LOW 200
#define HIGH 300

static volatile uint8_t en, off_no_poll;             /* 0, 1: disabled by default */
static volatile uint8_t low_ok, shrunk, no_hook, above_low, tags1; /* 1 each */
static volatile uint8_t grew, data_ok, mod_ok;                      /* 1 each */
static volatile uint8_t pool_a, pool_b, pool_c;

static void *blk[24];
static uint8_t nblk;
static mos_handle_t o1, big[8];

static uint8_t tags_ok(void) {
  uint8_t i, j, ok = 1;
  for (i = 0; i < nblk; i++)
    for (j = 0; j < 60; j++)
      if (((uint8_t *)blk[i])[j] != (uint8_t)(0x30 + i)) ok = 0;
  return ok;
}

int main(void) {
  uint8_t i, n, hooks;
  uint8_t *p;

  __set_heap_limit(1200);
  free(malloc(1));
  mod_init();
  en = mos_cache_shared(2, 6, 12, LOW, HIGH);
  o1 = mos_cacheable_malloc(20);
  p = mos_handle_lock(o1);
  for (i = 0; i < 20; i++) p[i] = (uint8_t)(0x40 + i);
  mos_handle_unlock(o1);

  /* automatic polling is off by default: a low heap does not make the cache yield */
  for (nblk = 0; nblk < 24 && __heap_bytes_free() >= LOW; ) {
    void *q = malloc(60);
    if (!q) break;
    memset(q, 0x30 + nblk, 60);
    blk[nblk++] = q;
  }
  low_ok = __heap_bytes_free() < LOW;
  pool_a = pool0_size();
  (void)mos_cacheable_free(mos_cacheable_malloc(20));   /* a safe point, polling off */
  off_no_poll = pool0_size() == pool_a && sh_services == 0;

  /* on: the next safe point yields, before any allocation has failed */
  mos_cache_auto_poll(1);
  hooks = sh_hook_calls;
  (void)mos_cacheable_free(mos_cacheable_malloc(20));
  pool_b = pool0_size();
  shrunk = pool_b < pool_a;
  no_hook = sh_hook_calls == hooks;
  above_low = __heap_bytes_free() >= LOW;
  tags1 = tags_ok();

  /* the heap becomes spare and the cache is short of room: the next safe point regrows it */
  while (nblk) free(blk[--nblk]);
  for (n = 0; n < 8; n++) {
    big[n] = mos_cacheable_malloc(70);      /* bank 0 fills, the rest goes to bank 1: wants to grow */
    if (!big[n]) break;
  }
  p = mos_handle_lock(o1);                  /* safe point: grows before locking */
  pool_c = pool0_size();
  grew = pool_c > pool_b;
  data_ok = 1;
  for (i = 0; i < 20; i++) if (p[i] != (uint8_t)(0x40 + i)) data_ok = 0;
  mos_handle_unlock(o1);
  mod_ok = m_add1(4) == 5;                  /* a module load is a safe point too */
  return 0;
}
