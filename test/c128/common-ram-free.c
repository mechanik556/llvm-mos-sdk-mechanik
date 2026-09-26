#include "../test-check.h"
#include <bank1.h>
#include <stdio.h>
#include <stdlib.h>

/* bank1.h lists $0B00-$0FFF (tape buffer, RS-232 buffers, default sprite
 * definitions) as Common RAM a program can have if it uses none of those
 * features. Fill the area with a pattern, run the KERNAL console for a while
 * (screen scrolling, IRQs, keyboard scanning, bank-1 calls), and check that
 * nothing touched it. A control area the system does use ($0A00-$0AFF, editor
 * variables) is deliberately not tested: writing to it corrupts the display.
 */

#define FIRST 0x0B00
#define LAST 0x0FFF

static unsigned char pattern(unsigned addr) {
  return (unsigned char)(addr * 13 + 0x5A);
}

MOS_C128_BANK1_DATA static volatile unsigned char table[4];
static volatile unsigned char __attribute__((section(".zp.bss"))) result;
MOS_C128_BANK1_CODE static void read_table(void) { result = table[1]; }

int main(void) {
  volatile unsigned char *area = (volatile unsigned char *)FIRST;
  for (unsigned a = FIRST; a <= LAST; ++a)
    area[a - FIRST] = pattern(a);

  /* 100 lines scroll the 25-line screen four times. The libretro runner gives a
   * test only 1000 frames, boot included, so this cannot be much longer. */
  for (unsigned i = 0; i < 100; ++i) {
    printf("line %u\n", i);
    c128_bank1_call(read_table);
  }

  for (unsigned a = FIRST; a <= LAST; ++a)
    CHECK(area[a - FIRST] == pattern(a));
  return EXIT_SUCCESS;
}
