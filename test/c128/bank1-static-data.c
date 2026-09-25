#include <bank1.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Startup initialization when every kind of static data is present at once,
 * together with bank-1 support and the heap. The platform's own sections (the
 * bank-1 load images and the Common-RAM save buffer) sit between .data and .bss
 * (see link.ld), so this checks that ordinary .data, .zp.data, .bss and .zp.bss
 * are still initialized correctly, that bank-1 data arrives, and that heap
 * allocations do not land on any of it. */

static volatile int data_tab[32] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                    12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                    23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
static volatile unsigned char __attribute__((section(".zp.data"))) zpv = 0x5A;
static volatile char bss_tab[200];
static volatile unsigned char __attribute__((section(".zp.bss"))) zbss;
MOS_C128_BANK1_DATA static volatile unsigned char b1 = 0x77;
static volatile unsigned char __attribute__((section(".zp.bss"))) rd;
MOS_C128_BANK1_CODE static void get(void) { rd = b1; }

int main(void) {
  int i, sum = 0;
  char *p;
  for (i = 0; i < 32; i++)
    sum += data_tab[i];
  if (sum != 528 || zpv != 0x5A || zbss != 0)
    return EXIT_FAILURE;
  for (i = 0; i < 200; i++)
    if (bss_tab[i] != 0)
      return EXIT_FAILURE;
  c128_bank1_call(get);
  if (rd != 0x77)
    return EXIT_FAILURE;
  p = malloc(300);
  if (!p)
    return EXIT_FAILURE;
  memset(p, 0x33, 300);
  for (i = 0; i < 32; i++)
    if (data_tab[i] != i + 1)
      return EXIT_FAILURE; /* the heap did not hit .data */
  if (zpv != 0x5A)
    return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
