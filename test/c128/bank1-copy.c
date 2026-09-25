#include <bank1.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* c128_bank1_write / c128_bank1_read: run-time copies to and from bank 1 at
 * any size and alignment (across 16-byte chunk boundaries), leaving bank 0's
 * memory at the same addresses untouched, and interoperating with statically
 * placed bank-1 data. */

extern char __c128bank1_free_start[];
extern char __c128bank1_free_end[];

MOS_C128_BANK1_DATA static volatile unsigned char placed[40] = {9, 8, 7, 6, 5,
                                                                4, 3, 2, 1, 0};

static unsigned char src[300], dst[320], probe[300];

int main(void) {
  uint16_t base = (uint16_t)__c128bank1_free_start;
  unsigned i, n;
  static const unsigned sizes[] = {1, 15, 16, 17, 31, 32, 33, 100, 255, 300};

  if (base < 0x1000 || (uint16_t)__c128bank1_free_end != 0xC000 ||
      base + 300 > 0xC000)
    return EXIT_FAILURE;
  /* the bank-0 bytes at the same addresses, to prove bank 0 is not written */
  memcpy(probe, (void *)base, 300);

  for (n = 0; n < sizeof sizes / sizeof sizes[0]; n++) {
    unsigned size = sizes[n];
    for (i = 0; i < 300; i++)
      src[i] = (unsigned char)(i * 7 + n);
    memset(dst, 0xEE, sizeof dst);
    c128_bank1_write(base + n, src, size);
    c128_bank1_read(dst, base + n, size);
    for (i = 0; i < size; i++)
      if (dst[i] != src[i])
        return EXIT_FAILURE;
    if (dst[size] != 0xEE) /* never writes past the end */
      return EXIT_FAILURE;
  }
  if (memcmp(probe, (void *)base, 300) != 0)
    return EXIT_FAILURE;

  /* statically placed bank-1 data is visible through the same routines */
  c128_bank1_read(dst, (uint16_t)placed, 10);
  for (i = 0; i < 10; i++)
    if (dst[i] != 9 - i)
      return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
