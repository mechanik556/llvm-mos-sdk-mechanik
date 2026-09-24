#include <bank1.h>
#include <stdint.h>
#include <stdlib.h>

/* The RAM Configuration Register ($D506) is changed while bank-1 support is in
 * use (Common RAM is enlarged to 4K) and must otherwise be left alone, and put
 * back at exit:
 *  - bits 7-6 (VIC bank select) and bit 3 (Shared-Hi enable) keep their value;
 *  - bits 2-0 read Shared-Lo enabled, 4K;
 *  - the original value is restored at exit (checked by the runner,
 *    vice-runner.py, which reads $D506 at startup and again after the exit
 *    handlers).
 * The test starts from a non-default value: an early init hook selects VIC bank
 * 1 (bit 6), which a blind overwrite of the register would lose. */

__asm__(".pushsection .init.010,\"ax\",@progbits\n"
        "  lda #$44\n"
        "  sta $D506\n"
        ".popsection\n");

MOS_C128_BANK1_DATA static volatile unsigned char x;
MOS_C128_BANK1_CODE static void touch(void) { x++; }

int main(void) {
  uint8_t rcr;
  c128_bank1_call(touch);
  rcr = *(volatile uint8_t *)0xD506;
  return ((rcr & 0xC8) == 0x40 && (rcr & 0x07) == 0x05) ? EXIT_SUCCESS : EXIT_FAILURE;
}
