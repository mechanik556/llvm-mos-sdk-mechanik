#include <bank1.h>
#include <stdio.h>

/* Third M0.1 test: the default Common-RAM code area is the low end of
 * BASIC's runtime stack ($0800). Checks (a) no KERNAL routine used by a
 * C program touches the rest of that stack area ($0840-$09FF, above our
 * code) - checksum before/after screen scrolling and a directory read
 * from disk - and (b) via test/check_restore.sh that the original bytes at
 * $0800-$09FF are restored at exit. Build flags -DDO_PRINTF / -DDO_FOPEN
 * enable the activity variants. Results in zero page: c0 (16-bit) before,
 * c1 (16-bit) after, fo = 1 if the fopen succeeded (it never did under
 * VICE's autostart virtual drive, so the disk path is only partly
 * exercised - the OPEN/serial-bus KERNAL routines still run).
 */

#ifndef FNAME
#define FNAME "m0_bank1_test3"
#endif
#define ZP __attribute__((section(".zp.bss")))
static volatile unsigned int ZP c0, c1;
static volatile unsigned char ZP fo;

MOS_C128_BANK1_DATA static volatile unsigned char x;
MOS_C128_BANK1_CODE static void touch(void) { x++; }

static unsigned int checksum(void) {
  const volatile unsigned char *p = (const volatile unsigned char *)0x0840;
  unsigned int s = 0;
  unsigned int i;
  for (i = 0; i < 0x1C0; i++) s = s * 31 + p[i];
  return s;
}

int main(void) {
  unsigned char i;
  c0 = checksum();
  c128_bank1_call(touch);
  for (i = 0; i < 40; i++) printf("line %d\n", i);
  {
    FILE *f = fopen(FNAME, "r");
    if (f) {
      fo = 1;
      for (i = 0; i < 100; i++) fgetc(f);
      fclose(f);
    }
  }
#endif
  c128_bank1_call(touch);
  c1 = checksum();
  return 0;
}
