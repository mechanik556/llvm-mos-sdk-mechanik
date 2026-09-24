#include <stdint.h>
#include <stdio.h>

/* M0.2 edge-case test: (1) a pinned module blocks a load that would need its
 * space, (2) CLOCK victim selection prefers a cold module over a hot one even
 * when the hot one is older... here the hot one (B) is OLDER than the cold one
 * (A), so plain FIFO would have evicted B first. */

typedef uint16_t mos_handle_t;
mos_handle_t mos_cacheable_malloc(uint16_t size);
uint8_t pool_free_units(uint8_t bank);
void mod_init(void);
void mod_clear_refs(void);
unsigned char m_double(unsigned char), m_cb(unsigned char), m_f_add7(unsigned char), m_r_call_sm(void), m_calld(unsigned char);
uint16_t m_r_try_big(void);
extern volatile uint16_t mt_addr[10];
extern volatile uint8_t place_refused;
extern volatile uint8_t mt_cr[10], mt_active[10], evict_log[16], evict_n, ams_top;

static volatile uint8_t ev_before_big, ev_delta_big, cd_alive, refused_before, refused_delta;
static volatile uint16_t big;                     /* 0x0101: refused (A=1, X=1) */
static volatile uint8_t r_alive, r_bank;          /* 1 (unpatched R), 0x4E */
static volatile uint8_t nfill, free0, free1;      /* 29, 0, 0 */
static volatile uint8_t nev0, ev_a, ev_b, ev_n;   /* -, 0 (A), 1 (B), 2 */
static volatile uint8_t f_res, b_again, act, ams_end; /* 10, 10, 0, 0 */

int main(void) {
  uint8_t n = 0;
  mod_init();
  m_double(1);                         /* B -> bank 0 (older) */
  m_cb(1);                             /* A -> bank 0 (newer): bank 0 has 4 of 5 units used */
  mos_cacheable_malloc(20);            /* bank 0 full */

  m_calld(4);                          /* C, D -> bank 1 (bank 0 is full) */
  ev_before_big = evict_n;
  refused_before = place_refused;
  big = m_r_try_big();                 /* R loads into bank 1, calls H (30 units) */
  ev_delta_big = evict_n - ev_before_big;   /* 0: refused up front, nothing evicted */
  cd_alive = (mt_addr[2] != 0) && (mt_addr[3] != 0);   /* 1: C and D untouched */
  refused_delta = place_refused - refused_before;      /* 1 */
  r_alive = m_r_call_sm();
  r_bank = mt_cr[5];

  while (pool_free_units(1) && n < 30) { mos_cacheable_malloc(20); n++; }
  nfill = n;
  free0 = pool_free_units(0);
  free1 = pool_free_units(1);

  mod_clear_refs();
  m_double(1);                         /* B is hot now (and older); A is cold */
  nev0 = evict_n;
  f_res = m_f_add7(3);                 /* F needs 2 units in a full bank 0: evicts */
  ev_a = evict_log[nev0];              /* expect module 0 (A, cold) first */
  ev_b = evict_log[nev0 + 1];          /* then module 1 (B) */
  ev_n = evict_n - nev0;               /* 2 */
  b_again = m_double(5);               /* 10: B reloads fine */
  act = 0;
  for (n = 0; n < 10; n++) act += mt_active[n];
  ams_end = ams_top;
  printf("edge: big=%04x evict %d,%d\n", (unsigned)big, ev_a, ev_b);
  return 0;
}
