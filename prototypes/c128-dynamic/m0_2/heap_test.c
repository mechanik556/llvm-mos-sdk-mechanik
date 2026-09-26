#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "compat.h"

/* M0.2.4 test: cacheable heap objects with caller-bank locking. Results are
 * globals dumped from VICE (expected values in comments). */

typedef uint16_t mos_cache_handle_t;
mos_cache_handle_t mos_cache_malloc(uint16_t size);
uint8_t mos_cache_free(mos_cache_handle_t h);
void *mos_cache_lock(mos_cache_handle_t h);
void mos_cache_unlock(mos_cache_handle_t h);
void mod_init(void);
unsigned char m_double(unsigned char), m_cb(unsigned char);
uint8_t m_g_incr(mos_cache_handle_t h);
uint16_t m_g_sum(mos_cache_handle_t h);
extern volatile uint8_t mt_cr[10], mt_active[10];

static volatile uint8_t r_m1, r_m2, f0_after_mods;          /* 4 10x: 1, 1 */
static volatile uint8_t h1_ok, b_h1_0, v_init;              /* 1, 0, 5 */
static volatile uint8_t e1, b_after_incr;                   /* 0, 1 */
static volatile uint8_t v1, b_after_lock0, lk1;             /* 6, 0, 1 */
static volatile uint8_t e2_refused, e_free_locked;          /* 1, 1 */
static volatile uint8_t e3, v2, lk_nested;                  /* 0, 7, 2 */
static volatile uint8_t e_free, e_free2;                    /* 0, 2 */
static volatile uint8_t b_h2_init, b_h2_lock0, ev_delta;    /* 1, 0, 1 */
static volatile uint16_t sum_lo_hi, sum_expect;             /* equal */
static volatile uint8_t b_h2_after_sum, bytes_ok, b_h2_back;/* 1, 1, 0 */
static volatile uint8_t r_b_reload, cr_b_reload;            /* 10, 4E */
static volatile uint8_t leak_f0a, leak_f0b, leak_f1a, leak_f1b, leak_err; /* equal pairs, 0 */
static volatile uint8_t r_act, r_ams, sp0, sp1;

static inline uint8_t get_sp(void) {
  uint8_t v;
  __asm__ volatile("tsx" : "=x"(v));
  return v;
}

int main(void) {
  uint8_t i, *p, ev0;
  mos_cache_handle_t h1, h2, h;
  uint16_t s;
  mod_init();
  r_m1 = m_double(1);      /* loads B into bank 0 */
  r_m2 = m_cb(1);          /* loads A into bank 0: 4 of 5 units used */
  f0_after_mods = pool_free_units(0);   /* 1 */

  h1 = mos_cache_malloc(20);        /* fills bank 0 */
  h1_ok = (h1 != 0);
  b_h1_0 = obj_bank(h1);               /* 0 */
  p = mos_cache_lock(h1);
  p[0] = 5;
  v_init = p[0];
  mos_cache_unlock(h1);

  e1 = m_g_incr(h1);       /* G loads into bank 1; object migrates to bank 1 */
  b_after_incr = obj_bank(h1);          /* 1 */

  p = mos_cache_lock(h1); /* bank-0 lock: migrates back */
  v1 = p[0];               /* 6 */
  b_after_lock0 = obj_bank(h1);         /* 0 */
  lk1 = obj_lock(h1);      /* 1 */
  e2_refused = m_g_incr(h1);            /* 1: locked by bank-0 code */
  e_free_locked = mos_cache_free(h1); /* 1 */
  mos_cache_unlock(h1);
  e3 = m_g_incr(h1);       /* 0 */
  p = mos_cache_lock(h1);
  v2 = p[0];               /* 7 */
  (void)mos_cache_lock(h1);
  lk_nested = obj_lock(h1);/* 2 */
  mos_cache_unlock(h1);
  mos_cache_unlock(h1);
  e_free = mos_cache_free(h1);      /* 0 */
  e_free2 = mos_cache_free(h1);     /* 2 */

  /* a 70-byte object: spans several 16-byte staging chunks each way */
  h2 = mos_cache_malloc(70);
  b_h2_init = obj_bank(h2);             /* 1 (bank 0 has only 1 free unit) */
  ev0 = mod_evictions;
  p = mos_cache_lock(h2); /* needs 3 units in bank 0: evicts module B */
  b_h2_lock0 = obj_bank(h2);            /* 0 */
  ev_delta = mod_evictions - ev0;       /* 1 */
  s = 0;
  for (i = 0; i < 70; i++) { p[i] = (uint8_t)(i * 3 + 1); s += p[i]; }
  sum_expect = s & 0xFF;
  mos_cache_unlock(h2);
  s = m_g_sum(h2);         /* bank-1 code sums it: object migrates to bank 1 */
  sum_lo_hi = s;           /* lo = sum, hi = 0 (not refused) */
  b_h2_after_sum = obj_bank(h2);        /* 1 */
  p = mos_cache_lock(h2);
  b_h2_back = obj_bank(h2);             /* 0 */
  bytes_ok = 1;
  for (i = 0; i < 70; i++) if (p[i] != (uint8_t)(i * 3 + 1)) bytes_ok = 0;
  mos_cache_unlock(h2);
  r_b_reload = m_double(5);             /* 10: B reloads (bank 1 now) */
  cr_b_reload = mt_cr[1];               /* 4E */
  mos_cache_free(h2);

  /* leak check: malloc/lock-from-bank-1/free cycles leave the pools as found */
  leak_f0a = pool_free_units(0);
  leak_f1a = pool_free_units(1);
  sp0 = get_sp();
  for (i = 0; i < 20; i++) {
    h = mos_cache_malloc(70);
    if (!h) { leak_err++; continue; }
    if (m_g_sum(h) & 0xFF00) leak_err++;
    if (mos_cache_free(h)) leak_err++;
  }
  sp1 = get_sp();
  leak_f0b = pool_free_units(0);
  leak_f1b = pool_free_units(1);
  for (i = 0; i < 10; i++) r_act += mt_active[i];
  r_ams = ams_top;
  printf("heap: sum=%d bytes_ok=%d leak_err=%d\n", (int)sum_lo_hi, bytes_ok, leak_err);
  return 0;
}
