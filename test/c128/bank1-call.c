#include <bank1.h>
#include <stdlib.h>

/* Static bank-1 placement, end to end:
 * - bank1_initial's non-zero initializer proves the load-time
 *   Common-RAM-staged copy really populated bank 1 (not zero/garbage).
 * - bank1_value read back by a separate, later call proves bank-1 RAM
 *   persists across c128_bank1_call invocations.
 * - result_initial/result_after live in zero page (Common RAM): that is how a
 *   bank-1 accessor hands a value to bank-0 code, since c128_bank1_call does
 *   not marshal a return value through registers.
 */

/* volatile so LTO cannot constant-fold the values away: the point is that real
 * bytes moved through real bank-1 memory. */
MOS_C128_BANK1_DATA static volatile unsigned char bank1_initial = 0x99;
MOS_C128_BANK1_DATA static volatile unsigned char bank1_value;

static volatile unsigned char
    __attribute__((section(".zp.bss"))) result_initial;
static volatile unsigned char __attribute__((section(".zp.bss"))) result_after;

MOS_C128_BANK1_CODE static void read_initial(void) {
  result_initial = bank1_initial;
}

MOS_C128_BANK1_CODE static void set_value(void) {
  bank1_value = bank1_initial + 1;
}

MOS_C128_BANK1_CODE static void read_value(void) { result_after = bank1_value; }

int main(void) {
  c128_bank1_call(read_initial);
  c128_bank1_call(set_value);
  c128_bank1_call(read_value);
  return (result_initial == 0x99 && result_after == 0x9A) ? EXIT_SUCCESS
                                                          : EXIT_FAILURE;
}
