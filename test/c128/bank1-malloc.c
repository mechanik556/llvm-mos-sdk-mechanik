#include <bank1.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Bank-1 support and the ordinary heap must not share memory. Everything the
 * platform keeps for itself in bank 0 has to lie below __heap_start (the start
 * of malloc's arena). Here that is the buffer that startup fills with the
 * original contents of the Common-RAM code area and that exit restores: it is
 * live for the whole run, and used to lie inside the arena, so a program that
 * used bank 1 and then allocated a few hundred bytes overwrote it (and exit
 * then restored garbage over $0800).
 *
 * The test allocates blocks, checks none overlaps the save buffer, and fills
 * them with distinct patterns that must survive. The runner additionally
 * verifies that $0800-$09FF is restored at exit (desktop VICE only). */

extern char __c128commoncode_save_start[];
extern void __c128commoncode_size;

MOS_C128_BANK1_DATA static volatile unsigned char x;
static volatile unsigned char __attribute__((section(".zp.bss"))) result;
MOS_C128_BANK1_CODE static void touch(void) { x++; }
/* Bank-0 code cannot read x (the same address is bank 0's memory): go through
 * an accessor and zero page, as bank1.h describes. */
MOS_C128_BANK1_CODE static void read_x(void) { result = x; }

#define NBLOCKS 8
#define BLOCK 100

int main(void) {
  char *p[NBLOCKS];
  uintptr_t save = (uintptr_t)__c128commoncode_save_start;
  uintptr_t save_end = save + (uintptr_t)&__c128commoncode_size;
  unsigned char i, j;

  c128_bank1_call(touch);
  for (i = 0; i < NBLOCKS; i++) {
    p[i] = malloc(BLOCK);
    if (!p[i])
      return EXIT_FAILURE;
    if ((uintptr_t)p[i] < save_end && (uintptr_t)p[i] + BLOCK > save)
      return EXIT_FAILURE; /* the block overlaps the live save buffer */
    memset(p[i], 0xA0 + i, BLOCK);
  }
  c128_bank1_call(touch);
  for (i = 0; i < NBLOCKS; i++)
    for (j = 0; j < BLOCK; j++)
      if ((unsigned char)p[i][j] != (unsigned char)(0xA0 + i))
        return EXIT_FAILURE;
  c128_bank1_call(read_x);
  return result == 2 ? EXIT_SUCCESS : EXIT_FAILURE;
}
