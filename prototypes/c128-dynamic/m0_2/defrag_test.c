#include <stdint.h>
#include <stdio.h>
#include "compat.h"

/* Defragmentation test. Expected values in comments. */

typedef uint16_t mos_handle_t;
mos_handle_t mos_cacheable_malloc(uint16_t size);
uint8_t mos_cacheable_free(mos_handle_t h);
void *mos_handle_lock(mos_handle_t h);
void mos_handle_unlock(mos_handle_t h);
uint8_t obj_bank(mos_handle_t h), pool_free_units(uint8_t bank), pool_max_run(uint8_t bank);
uint8_t mos_defrag(void), mod_evict(uint8_t id);
void mod_init(void);
unsigned char m_r_call_sm(void), m_r_sm_to_b(void), m_r_entry(unsigned char), m_r_viaptr(unsigned char), m_r_lohi(unsigned char);
unsigned char m_r_try_defrag(void);
extern volatile uint16_t mt_addr[10];

/* Phase B: module relocation during defragmentation */
static volatile uint8_t qb1, qb2;                         /* 1 (fresh R), 2 (patched) */
static volatile uint16_t addr_before, addr_pinned, addr_after, moved_pinned_w; /* pinned: same */
static volatile uint8_t moves_pinned, moves_b;            /* 0 (R active: pinned), 1 */
static volatile uint8_t qa1, qa2, qa3, qa4;               /* 2 13 15 15 after the move */
static volatile uint8_t delta_ok;                         /* addr_after == addr_before - 96 */
/* Phase E: a locked object is pinned */
static volatile uint8_t e_same, e_data, e_moved;          /* 1, 1, >=1 */
/* Phase A: automatic defragmentation on allocation */
static volatile uint8_t frag_ok, mr_before, free_before;  /* 1; max run < 6 <= free */
static volatile uint8_t big_ok, big_b, dm_delta, mr_after;/* 1, 1, >=1, run grew */
static volatile uint8_t a_data;                           /* 1 */

static mos_handle_t o[5], x[10], y, big;

static uint8_t fill(mos_handle_t h, uint8_t n, uint8_t seed) {
  uint8_t *p = mos_handle_lock(h), i;
  if (!p) return 0;
  for (i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7);
  mos_handle_unlock(h);
  return 1;
}
static uint8_t check(mos_handle_t h, uint8_t n, uint8_t seed) {
  uint8_t *p = mos_handle_lock(h), i, ok = 1;
  if (!p) return 0;
  for (i = 0; i < n; i++) if (p[i] != (uint8_t)(seed + i * 7)) ok = 0;
  mos_handle_unlock(h);
  return ok;
}

int main(void) {
  uint8_t i, ok, dm;
  mod_init();

  /* ---- Phase B: R relocates when defragmentation slides it down ---- */
  for (i = 0; i < 5; i++) { o[i] = mos_cacheable_malloc(20); fill(o[i], 20, 0x20 + i); }  /* bank 0 full */
  y = mos_cacheable_malloc(70);          /* bank 1, units 0-2 */
  qb1 = m_r_call_sm();                   /* 1: R loads into bank 1, units 3-5 */
  m_r_sm_to_b();
  qb2 = m_r_call_sm();                   /* 2 */
  addr_before = mt_addr[5];
  mos_cacheable_free(y);                 /* hole in front of R */
  moves_pinned = m_r_try_defrag();       /* R is active: pinned, nothing may move */
  addr_pinned = mt_addr[5];              /* == addr_before */
  moves_b = mos_defrag();                /* 1: R slides to unit 0, relocated by -96 */
  addr_after = mt_addr[5];
  delta_ok = (addr_after == (uint16_t)(addr_before - 96));
  qa1 = m_r_call_sm();                   /* 2: self-modified operand relocated too */
  qa2 = m_r_entry(2);                    /* 13 */
  qa3 = m_r_viaptr(5);                   /* 15 */
  qa4 = m_r_lohi(5);                     /* 15 */

  /* ---- Phase E: a locked object is pinned; the others still compact ---- */
  {
    uint8_t *p = mos_handle_lock(o[2]), *q;
    mos_cacheable_free(o[0]);            /* hole at bank-0 unit 0 */
    dm = defrag_moves;
    mos_defrag();                        /* o[1] slides down; locked o[2] stays put */
    e_moved = defrag_moves - dm;         /* >= 1 */
    q = mos_handle_lock(o[2]);           /* nested lock: same address if it did not move */
    e_same = (p == q);
    mos_handle_unlock(o[2]);
    mos_handle_unlock(o[2]);
    ok = check(o[1], 20, 0x21) & check(o[2], 20, 0x22) & check(o[3], 20, 0x23) & check(o[4], 20, 0x24);
    e_data = ok;                         /* 1 */
  }

  /* ---- Phase A: allocation defragments a fragmented bank ---- */
  for (i = 1; i < 5; i++) mos_cacheable_free(o[i]);
  mod_evict(5);
  for (i = 0; i < 10; i++) { x[i] = mos_cacheable_malloc(70); fill(x[i], 70, 0x50 + i); }
  /* free every other object that lives in bank 1 -> 3-unit holes */
  {
    uint8_t toggle = 0;
    for (i = 0; i < 10; i++) {
      if (obj_bank(x[i]) != 1) continue;
      if (toggle) { mos_cacheable_free(x[i]); x[i] = 0; }
      toggle = !toggle;
    }
  }
  mr_before = pool_max_run(1);
  free_before = pool_free_units(1);
  frag_ok = (mr_before < 6 && free_before >= 6);         /* 1 */
  dm = defrag_moves;
  big = mos_cacheable_malloc(190);       /* 6 units: only fits after defragmenting bank 1 */
  big_ok = (big != 0);
  big_b = big ? obj_bank(big) : 9;       /* 1 */
  dm_delta = defrag_moves - dm;          /* >= 1 */
  mr_after = pool_max_run(1);            /* >= 2 (2 free units at least remain contiguous) */
  ok = 1;
  for (i = 0; i < 10; i++) if (x[i]) ok &= check(x[i], 70, 0x50 + i);
  a_data = ok;                           /* 1 */
  printf("defrag: %d %d %d moves=%d\n", moves_b, e_moved, dm_delta, defrag_moves);
  return 0;
}
