#include <bank1.h>
#include <stdio.h>

/* M0.2.1 test driver. Results are ordinary globals, read from a VICE
 * memory dump using addresses from the link map: expect r0=$55 r1=10 r2=108 r3=4 r4=5 sum=7 carry=1 cin=0 if=4 inest=4, cr=$0E, act=0,
 * amstop=0, err=0, sp0==sp1, jiffy clock advancing after the calls. */

unsigned char m_double(unsigned char);
unsigned char m_get(void);
unsigned char m_cb(unsigned char);
unsigned char m_chain(unsigned char);
unsigned char m_sum(unsigned char, unsigned char);
unsigned char t_carry(void), t_cin(void), t_iflag(void), t_inest(void);
extern volatile unsigned char mt_active[3];
extern volatile unsigned char ams_top;

static volatile unsigned char r0, r1, r2, r3, r4, r_cr, r_act, r_ams, r_sum, r_carry, r_cin, r_if, r_inest, err, sp0, sp1, j2, j3;

static inline unsigned char get_sp(void) {
  unsigned char v;
  __asm__ volatile("tsx" : "=x"(v));
  return v;
}
static void delay(void) {
  volatile unsigned int d;
  for (d = 0; d < 20000; d++)
    ;
}

int main(void) {
  unsigned char i;
  r0 = m_get();          /* initial bank-1 data, proves load-time copy */
  r1 = m_double(5);
  r2 = m_cb(7);          /* bank0 -> bank1 -> bank0 nested */
  r3 = m_chain(3);       /* bank0 -> bank1 -> bank1 */
  r4 = m_get();
  r_sum = m_sum(3, 4);
  r_carry = t_carry();
  r_cin = t_cin();
  r_if = t_iflag();
  r_inest = t_inest();
  r_cr = *(volatile unsigned char *)0xFF00;
  r_act = mt_active[0] + mt_active[1] + mt_active[2];
  r_ams = ams_top;
  sp0 = get_sp();
  for (i = 0; i < 200; i++) {
    if (m_cb(i) != (unsigned char)(i + 101)) err++;
    if (m_chain(i) != (unsigned char)(i + 1)) err++;
  }
  sp1 = get_sp();
  j2 = *(volatile unsigned char *)0xA2;
  delay();
  j3 = *(volatile unsigned char *)0xA2;
  printf("r=%d %d %d %d %d err=%d\n", r0, r1, r2, r3, r4, err);
  return 0;
}
