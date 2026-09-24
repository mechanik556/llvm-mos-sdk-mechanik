#include <bank1.h>
#include <stdint.h>
#include <test-lib-emutest.h>

/* Checks the claim in bank1.h that while bank 1 is mapped an ordinary bank-0
 * global at address X is NOT visible: the same address reads and writes bank
 * 1's own memory. So bank-1 code must be self-contained.
 *  - bank-1 code writes g0 = $22 and reads it back: it sees its own write;
 *  - bank 0's copy of g0 is untouched ($11);
 *  - locals in bank-1 code work (they land in bank-1 memory at the address of
 *    bank 0's static stack).
 */

#define ZP __attribute__((section(".zp.bss")))
static volatile uint8_t g0 = 0x11; /* ordinary bank-0 .data */
static volatile uint8_t ZP r_read1, r_local;

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
  uint8_t g0_after;
  c128_bank1_call(poke);
  g0_after = g0;
  c128_bank1_call(local);
  test_set_result(r_read1 == 0x22 && g0_after == 0x11 && r_local == 21);
  return 0;
}
