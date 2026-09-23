#include <bank1.h>
#include <stdio.h>

/* Second M0.1 test: the criteria test 1 (m0_bank1_test.c) didn't check.
 * Results are left in zero page for a VICE monitor dump:
 *   r_counter  expect 200 (0xC8): 200 c128_bank1_call invocations persisted
 *   r_cr       expect 0x0E:       $FF00 restored to bank-0 config after return
 *   j0/j1      jiffy clock ($A2) around a delay BEFORE any bank call
 *              (control - proves the clock advances in this environment)
 *   j2/j3      same, AFTER the bank calls - differing proves CLI really
 *              re-enabled the IRQ that advances the clock
 *   s1/s2      sentinels set before the loop, expect 0xA5/0x5A unchanged
 * printf output ("counter=200 cr=14") should also appear in screen RAM.
 */

MOS_C128_BANK1_DATA static volatile unsigned char counter;

#define ZP __attribute__((section(".zp.bss")))
static volatile unsigned char ZP r_counter;
static volatile unsigned char ZP r_cr;
static volatile unsigned char ZP j0, j1, j2, j3;
static volatile unsigned char ZP s1, s2;

MOS_C128_BANK1_CODE static void inc_counter(void) { counter++; }
MOS_C128_BANK1_CODE static void read_counter(void) { r_counter = counter; }

static void delay(void) {
  volatile unsigned int d;
  for (d = 0; d < 20000; d++)
    ;
}

int main(void) {
  unsigned char i;
  s1 = 0xA5;
  s2 = 0x5A;

  j0 = *(volatile unsigned char *)0xA2;
  delay();
  j1 = *(volatile unsigned char *)0xA2;

  c128_bank1_call(inc_counter);
  r_cr = *(volatile unsigned char *)0xFF00;

  for (i = 0; i < 199; i++)
    c128_bank1_call(inc_counter);

  j2 = *(volatile unsigned char *)0xA2;
  delay();
  j3 = *(volatile unsigned char *)0xA2;

  c128_bank1_call(read_counter);
  printf("counter=%d cr=%d\n", r_counter, r_cr);
  return 0;
}
