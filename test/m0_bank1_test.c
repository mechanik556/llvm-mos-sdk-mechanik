#include <bank1.h>
#include <stdio.h>

/* Tests the M0.1 bank-1 static placement mechanism end to end:
 * - bank1_initial's non-zero initializer proves the load-time
 *   Common-RAM-staged copy actually populated bank 1 (not zero/garbage).
 * - bank1_value being correctly read back after a separate later call
 *   proves bank-1 RAM genuinely persists across multiple c128_bank1_call
 *   invocations, not reset/reloaded each time.
 * - result_initial/result_after (Common RAM, .zp.bss) are how a bank-1
 *   accessor passes a value back to bank-0 code, since c128_bank1_call
 *   doesn't marshal a return value through registers.
 */

/* volatile so LTO can't constant-fold these away and inline their value
 * directly - the whole point of this test is proving real bytes moved
 * through real bank-1 memory via the load-time copy mechanism, not just
 * that the accessor functions return the right number. */
MOS_C128_BANK1_DATA static volatile unsigned char bank1_initial = 0x99;
MOS_C128_BANK1_DATA static volatile unsigned char bank1_value;

static volatile unsigned char __attribute__((section(".zp.bss"))) result_initial;
static volatile unsigned char __attribute__((section(".zp.bss"))) result_after;

MOS_C128_BANK1_CODE static void read_initial(void) {
  result_initial = bank1_initial;
}

MOS_C128_BANK1_CODE static void set_value(void) {
  bank1_value = bank1_initial + 1;
}

MOS_C128_BANK1_CODE static void read_value(void) {
  result_after = bank1_value;
}

int main(void) {
  c128_bank1_call(read_initial);
  c128_bank1_call(set_value);
  c128_bank1_call(read_value);
  printf("initial=%d after=%d\n", result_initial, result_after);
  return 0;
}
