#include <bank1.h>
#include <test-lib-emutest.h>

/* What a c128_bank1_call must leave undisturbed in the caller's world:
 *  - 200 calls persist (counter reaches 200);
 *  - $FF00 (the MMU configuration register) is back to the bank-0 value;
 *  - interrupts are re-enabled afterwards: the jiffy clock ($A2) advances over
 *    a delay AFTER the bank calls just as it does over the same delay BEFORE
 *    any call (the "before" reading is the control - if the clock does not
 *    advance in this environment the test would say nothing);
 *  - the hardware stack pointer is the same before and after the loop;
 *  - sentinels in zero page survive.
 */

MOS_C128_BANK1_DATA static volatile unsigned char counter;

#define ZP __attribute__((section(".zp.bss")))
static volatile unsigned char ZP r_counter;
static volatile unsigned char ZP s1, s2;

static inline unsigned char get_sp(void) {
  unsigned char v;
  __asm__ volatile("tsx" : "=x"(v));
  return v;
}

MOS_C128_BANK1_CODE static void inc_counter(void) { counter++; }
MOS_C128_BANK1_CODE static void read_counter(void) { r_counter = counter; }

static void delay(void) {
  volatile unsigned int d;
  for (d = 0; d < 20000; d++)
    ;
}

int main(void) {
  unsigned char i, j0, j1, j2, j3, cr, sp0, sp1;
  s1 = 0xA5;
  s2 = 0x5A;

  j0 = *(volatile unsigned char *)0xA2;
  delay();
  j1 = *(volatile unsigned char *)0xA2;

  c128_bank1_call(inc_counter);
  cr = *(volatile unsigned char *)0xFF00;

  sp0 = get_sp();
  for (i = 0; i < 199; i++)
    c128_bank1_call(inc_counter);

  sp1 = get_sp();
  j2 = *(volatile unsigned char *)0xA2;
  delay();
  j3 = *(volatile unsigned char *)0xA2;

  c128_bank1_call(read_counter);
  test_set_result(r_counter == 200 && cr == 0x0E && j1 != j0 && j3 != j2 &&
                  sp0 == sp1 && s1 == 0xA5 && s2 == 0x5A);
  return 0;
}
