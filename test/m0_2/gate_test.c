#include <bank1.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* M0.2 test driver (stages 2.1-2.3). Results are ordinary globals, read from
 * a VICE memory dump via the link map. Expected values are in the comments
 * next to each assignment; a summary is printed on screen as well. */

unsigned char m_double(unsigned char);
unsigned char m_cb(unsigned char);
unsigned char m_chain(unsigned char);
unsigned char m_sum(unsigned char, unsigned char);
unsigned char m_calld(unsigned char);
unsigned char t_carry(void), t_cin(void), t_iflag(void), t_inest(void), t_fail(void);
unsigned char m_r_entry(unsigned char), m_r_viaptr(unsigned char), m_r_lohi(unsigned char);
unsigned char m_r_call_sm(void), m_r_sm_to_b(void), m_r_try_evict(void);
unsigned char m_f_add7(unsigned char), m_h_double(unsigned char);
uint8_t mod_evict(uint8_t);
void mod_init(void);

#define NMODS 9
extern volatile uint8_t mt_active[NMODS];
extern volatile uint8_t mt_cr[NMODS];
extern volatile uint16_t mt_addr[NMODS];
extern const uint16_t mt_img[NMODS];
extern const uint16_t r_info[3];
extern volatile uint8_t mod_loads, mod_evictions;
extern volatile uint8_t ams_top;

/* 2.1/2.2 */
static volatile uint8_t r1, r2, r3, r4, r_sum, r_carry, r_cin, r_if, r_inest, r_fail, loads0;
static volatile uint8_t cr_b, cr_c, cr_d;             /* 0E, 4E, 4E */
/* 2.3 relocation / self-modification / eviction */
static volatile uint8_t q1, q2, q3, q4, q5;           /* 13 15 15 1 2 */
static volatile uint8_t q_pinned, q_still, ev, q_gone; /* 1 1 0 0 */
static volatile uint16_t addr1, addr2, patched, expect_patched;
static volatile uint8_t cr1, q_f, q_sm2, q_entry2;    /* 4E, 10, 2, 13 */
static volatile uint8_t ndiff, diff_ok;               /* image write-back: 1-2 differing bytes, all in the patched operand */
static volatile uint8_t q_h, ev_after_h, res_c, res_d, res_f, res_r, res_h; /* 42, >=4, then 0 0 0 0, 1 */
static volatile uint8_t q_thrash, q_sm3, ev_end;      /* 8, 2 */
static volatile uint8_t ndiff2, diff_ok2;
/* common */
static volatile uint8_t r_cr, r_act, r_ams, err, sp0, sp1, j2, j3, loads1;

static uint8_t orig_r[96];

static inline uint8_t get_sp(void) {
  uint8_t v;
  __asm__ volatile("tsx" : "=x"(v));
  return v;
}
static void delay(void) {
  volatile unsigned int d;
  for (d = 0; d < 20000; d++)
    ;
}

/* Compare module R's stored image to the pristine copy; count differing
 * bytes and check every difference lies in the 2-byte jmp operand that
 * r_sm_to_b patches. */
static void compare_r(volatile uint8_t *ndiff_out, volatile uint8_t *ok_out) {
  const uint8_t *img = (const uint8_t *)mt_img[5];
  uint8_t i, n = 0, ok = 1;
  for (i = 0; i < 96; i++)
    if (img[i] != orig_r[i]) {
      n++;
      if (i != r_info[0] && i != r_info[0] + 1) ok = 0;
    }
  *ndiff_out = n;
  *ok_out = ok;
}

int main(void) {
  uint8_t i;
  mod_init();
  memcpy(orig_r, (const void *)mt_img[5], 96);

  r1 = m_double(5);        /* 10   loads B into bank 0 (caller's bank) */
  r2 = m_cb(7);            /* 108  B -> A: loads A into bank 0 (now full) */
  r3 = m_chain(3);         /* 4    B -> C: bank 0 full, C spills to bank 1 */
  r4 = m_calld(4);         /* 8    C (bank 1) -> D: D to bank 1 (caller's bank) */
  cr_b = mt_cr[1]; cr_c = mt_cr[2]; cr_d = mt_cr[3];  /* 0E 4E 4E */
  r_sum = m_sum(3, 4);     /* 7 */
  r_carry = t_carry();     /* 1 */
  r_cin = t_cin();         /* 0 */
  r_if = t_iflag();        /* 4 */
  r_inest = t_inest();     /* 4 */
  r_fail = t_fail();       /* 1 */
  loads0 = mod_loads;      /* 4 */

  /* relocation: R loads into bank 1 at some address; every internal absolute
   * reference works from there */
  q1 = m_r_entry(2);       /* 13 */
  q2 = m_r_viaptr(5);      /* 15 */
  q3 = m_r_lohi(5);        /* 15 */
  q4 = m_r_call_sm();      /* 1  */
  m_r_sm_to_b();           /* patch its own jmp operand with a CURRENT address */
  q5 = m_r_call_sm();      /* 2  */
  addr1 = mt_addr[5];
  cr1 = mt_cr[5];          /* 4E */
  q_pinned = m_r_try_evict();  /* 1: refused while R is active */
  q_still = (mt_addr[5] == addr1);  /* 1 */
  ev = mod_evict(5);       /* 0: evicted (self-modified operand un-relocated by delta) */
  q_gone = (mt_addr[5] != 0);       /* 0 */
  patched = *(const volatile uint16_t *)(mt_img[5] + r_info[0]);
  expect_patched = mt_img[5] + r_info[2];   /* canonical address of r_target_b */
  compare_r(&ndiff, &diff_ok);              /* only the patched operand differs */

  /* reload at a different address: F takes R's old spot first */
  q_f = m_f_add7(3);       /* 10 */
  q_sm2 = m_r_call_sm();   /* 2: self-modification survived evict + reload */
  addr2 = mt_addr[5];      /* != addr1 */
  q_entry2 = m_r_entry(2); /* 13 */

  /* automatic eviction: H needs 30 of bank 1's 32 units */
  q_h = m_h_double(21);    /* 42 */
  ev_after_h = mod_evictions;
  res_c = (mt_addr[2] != 0); res_d = (mt_addr[3] != 0);
  res_f = (mt_addr[7] != 0); res_r = (mt_addr[5] != 0);
  res_h = (mt_addr[8] != 0);   /* 0 0 0 0 1 */
  q_thrash = m_calld(4);   /* 8: reloads C, evicts H to make room for D */
  q_sm3 = m_r_call_sm();   /* 2: patch survived a further eviction cycle */
  ev_end = mod_evictions;

  r_cr = *(volatile uint8_t *)0xFF00;
  r_act = 0;
  for (i = 0; i < NMODS; i++) r_act += mt_active[i];
  r_ams = ams_top;
  sp0 = get_sp();
  for (i = 0; i < 30; i++) {
    if (m_cb(i) != (uint8_t)(i + 101)) err++;
    if (m_calld(i) != (uint8_t)(i * 2)) err++;
    if (m_r_entry(i & 3) != (uint8_t)((i & 3) + 1 + 10)) err++;
    if (m_h_double(i) != (uint8_t)(i * 2)) err++;   /* keeps thrashing bank 1 */
    if (m_r_call_sm() != 2) err++;
  }
  sp1 = get_sp();
  loads1 = mod_loads;
  mod_evict(5);
  compare_r(&ndiff2, &diff_ok2);   /* still only the patched operand */
  j2 = *(volatile uint8_t *)0xA2;
  delay();
  j3 = *(volatile uint8_t *)0xA2;
  printf("err=%d loads=%d evictions=%d\n", err, loads1, mod_evictions);
  return 0;
}
