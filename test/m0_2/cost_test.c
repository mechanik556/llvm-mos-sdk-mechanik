#include <stdint.h>
#include <stdio.h>

/* Gate cost measurement (hit path only: every module is loaded before timing).
 * Each case runs REPS calls between a CIA2 timer A start and stop, with
 * interrupts off and the VIC display blanked (no badline cycle stealing), so
 * the counts are exact CPU cycles. t_* are TOTAL cycles for REPS calls,
 * including the C loop and the stub's own jsr/rts; subtract t_base (the same
 * loop calling an empty stub) and divide by REPS for the per-call figure.
 * Results are read from a VICE dump via the link map. */

unsigned char m_base(unsigned char);
unsigned char m_add1(unsigned char);    /* bank 0 -> gate -> A   (bank 0) */
unsigned char m_double(unsigned char);  /* bank 0 -> gate -> B   (bank 0) */
unsigned char m_c_inc(unsigned char);   /* bank 0 -> gate -> C   (bank 1) */
unsigned char m_c_calla(unsigned char); /* bank 0 -> C(bank 1) -> A(bank 0): two gates */
unsigned char m_calld(unsigned char);   /* bank 0 -> C(bank 1) -> D(bank 1): two gates */
void mod_init(void);
uint8_t mod_evict(uint8_t);
extern volatile uint8_t mt_cr[];
extern volatile uint8_t mod_loads;

#define REPS 16
#define CIA2_TA_LO (*(volatile uint8_t *)0xDD04)
#define CIA2_TA_HI (*(volatile uint8_t *)0xDD05)
#define CIA2_CRA (*(volatile uint8_t *)0xDD0E)
#define VIC_D011 (*(volatile uint8_t *)0xD011)

static volatile uint16_t t_miss_a, t_miss_b, t_miss_c, t_empty, t_base, t_00, t_b0, t_01, t_010, t_0111;
static volatile uint8_t cr_a, cr_b, cr_c, cr_d, loads;
static volatile uint8_t sink;
static uint8_t d011;
static uint16_t dl;

typedef unsigned char (*fn)(unsigned char);

static uint16_t time_it(fn f) {
  uint8_t i;
  uint16_t left;
  __asm__ volatile("sei");
  CIA2_TA_LO = 0xFF;
  CIA2_TA_HI = 0xFF;
  CIA2_CRA = 0x10; /* force load, stopped */
  CIA2_CRA = 0x01; /* start, continuous, phi2 */
  for (i = 0; i < REPS; i++) sink = f(i);
  CIA2_CRA = 0;
  left = CIA2_TA_LO;
  left |= (uint16_t)CIA2_TA_HI << 8;
  __asm__ volatile("cli");
  return 0xFFFF - left;
}

/* ONE call that misses: evict the module first (not timed), then time the call,
 * which loads it (canonical image -> pool, relocation walk) and dispatches. */
static uint16_t time_miss(fn f, uint8_t id) {
  uint16_t left;
  mod_evict(id);
  __asm__ volatile("sei");
  CIA2_TA_LO = 0xFF;
  CIA2_TA_HI = 0xFF;
  CIA2_CRA = 0x10;
  CIA2_CRA = 0x01;
  sink = f(1);
  CIA2_CRA = 0;
  left = CIA2_TA_LO;
  left |= (uint16_t)CIA2_TA_HI << 8;
  __asm__ volatile("cli");
  return 0xFFFF - left;
}

static uint16_t time_empty(void) {
  uint8_t i;
  uint16_t left;
  __asm__ volatile("sei");
  CIA2_TA_LO = 0xFF;
  CIA2_TA_HI = 0xFF;
  CIA2_CRA = 0x10;
  CIA2_CRA = 0x01;
  for (i = 0; i < REPS; i++) sink = i;
  CIA2_CRA = 0;
  left = CIA2_TA_LO;
  left |= (uint16_t)CIA2_TA_HI << 8;
  __asm__ volatile("cli");
  return 0xFFFF - left;
}

int main(void) {
  mod_init();
  /* warm-up loads the modules: A, B in bank 0 (4 of 5 units), then C doesn't
   * fit -> bank 1, then D loaded from C's bank -> bank 1 */
  m_add1(1);
  m_double(1);
  m_c_inc(1);
  m_calld(1);
  m_c_calla(1);
  cr_a = mt_cr[0];
  cr_b = mt_cr[1];
  cr_c = mt_cr[2];
  cr_d = mt_cr[3];
  loads = mod_loads; /* 4 */

  /* Blank the display ONCE and let a few frames pass: the VIC samples DEN at
   * raster line $30, so blanking mid-frame keeps badlines (stolen cycles) going
   * until the frame ends. Interrupts stay on here so the KERNAL keeps running. */
  d011 = VIC_D011;
  VIC_D011 = d011 & ~0x10;
  for (dl = 0; dl < 3000; dl++) sink = 0;

  t_empty = time_empty();
  t_base = time_it(m_base);
  t_00 = time_it(m_add1);     /* one gate, 0 -> 0 */
  t_b0 = time_it(m_double);   /* same, different module */
  t_01 = time_it(m_c_inc);    /* one gate, 0 -> 1 */
  t_010 = time_it(m_c_calla); /* two gates: 0 -> 1, then 1 -> 0 */
  t_0111 = time_it(m_calld);  /* two gates: 0 -> 1, then 1 -> 1 */
  t_miss_a = time_miss(m_add1, 0);   /* 1 unit, bank 0 */
  t_miss_b = time_miss(m_double, 1);  /* 3 units, bank 0 */
  t_miss_c = time_miss(m_c_inc, 2);   /* 2 units, ends in bank 1 (bank 0 has 1 free unit) */
  VIC_D011 = d011;
  printf("base=%u 00=%u 01=%u 010=%u 0111=%u\n", t_base, t_00, t_01, t_010, t_0111);
  return 0;
}
