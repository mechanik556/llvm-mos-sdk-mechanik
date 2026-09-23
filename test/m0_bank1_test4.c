#include <bank1.h>
#include <stdint.h>

/* Tests the claim in bank1.h: while bank 1 is mapped, an ordinary bank-0
 * global at address X is NOT visible - the same address reads/writes bank 1's
 * memory. Expect (zero page dump): r_read1 = $22 (bank-1 code sees its own
 * write), r_g0 = $11 (bank-0 copy untouched), r_local = 21 (locals of bank-1
 * code work, but in bank-1 memory at bank 0's static-stack address). */

#define ZP __attribute__((section(".zp.bss")))
static volatile uint8_t g0 = 0x11;                 /* ordinary bank-0 .data */
static volatile uint8_t ZP r_read1, r_g0, r_local;

MOS_C128_BANK1_CODE static void poke(void) {
  g0 = 0x22;
  r_read1 = g0;
}
MOS_C128_BANK1_CODE static void local(void) {
  volatile uint8_t buf[6];
  uint8_t i, s = 0;
  for (i = 0; i < 6; i++) buf[i] = i + 1;
  for (i = 0; i < 6; i++) s += buf[i];
  r_local = s;
}

int main(void) {
  c128_bank1_call(poke);
  r_g0 = g0;
  c128_bank1_call(local);
  return 0;
}
