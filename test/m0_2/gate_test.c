#include <bank1.h>
#include <stdint.h>
#include <stdio.h>

/* M0.2.2 test driver: modules are loaded on first call into two bank-limited
 * pools. Results are ordinary globals, read from a VICE memory dump via the
 * link map. Expected: r1=10 r2=108 r3=4 r4=8 (m_calld(4)) sum=7 carry=1
 * cin=0 if=4 inest=4 fail=1, loads0=4 then unchanged after a 200-iteration
 * loop (loads1=4), mt_cr = 0E 0E 4E 4E 00 (A,B in bank 0; C,D in bank 1: C
 * spilled, D placed by the "prefer the caller's bank" rule even though bank 0
 * has a free unit), mt_active all 0, ams_top=0, err=0, sp0==sp1, cr=$0E. */

unsigned char m_double(unsigned char);
unsigned char m_cb(unsigned char);
unsigned char m_chain(unsigned char);
unsigned char m_sum(unsigned char, unsigned char);
unsigned char m_calld(unsigned char);
unsigned char t_carry(void), t_cin(void), t_iflag(void), t_inest(void), t_fail(void);
extern volatile uint8_t mt_active[5];
extern volatile uint8_t mt_cr[5];
extern volatile uint16_t mt_addr[5];
extern volatile uint8_t mod_loads;
extern volatile uint8_t ams_top;

static volatile uint8_t r1, r2, r3, r4, r_sum, r_carry, r_cin, r_if, r_inest, r_fail;
static volatile uint8_t loads0, loads1, r_cr, r_act, r_ams, err, sp0, sp1, j2, j3;

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

int main(void) {
  uint8_t i;
  r1 = m_double(5);        /* loads B into bank 0 (caller's bank) */
  r2 = m_cb(7);            /* B -> A: loads A into bank 0 (now full) */
  r3 = m_chain(3);         /* B -> C: bank 0 full, C spills to bank 1 */
  r4 = m_calld(4);         /* C (bank 1) -> D: D goes to bank 1, caller's bank */
  r_sum = m_sum(3, 4);
  r_carry = t_carry();
  r_cin = t_cin();
  r_if = t_iflag();
  r_inest = t_inest();
  r_fail = t_fail();
  loads0 = mod_loads;
  r_cr = *(volatile uint8_t *)0xFF00;
  r_act = mt_active[0] + mt_active[1] + mt_active[2] + mt_active[3] + mt_active[4];
  r_ams = ams_top;
  sp0 = get_sp();
  for (i = 0; i < 200; i++) {
    if (m_cb(i) != (uint8_t)(i + 101)) err++;
    if (m_chain(i) != (uint8_t)(i + 1)) err++;
    if (m_calld(i) != (uint8_t)(i * 2)) err++;
  }
  sp1 = get_sp();
  loads1 = mod_loads;
  j2 = *(volatile uint8_t *)0xA2;
  delay();
  j3 = *(volatile uint8_t *)0xA2;
  printf("r=%d %d %d %d err=%d loads=%d\n", r1, r2, r3, r4, err, loads1);
  return 0;
}
