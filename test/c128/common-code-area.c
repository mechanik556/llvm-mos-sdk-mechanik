#include <bank1.h>
#include <stdio.h>
#include <test-lib-emutest.h>

/* The default Common-RAM code area is the low end of BASIC's runtime stack
 * ($0800). Two properties matter:
 *  (a) nothing a C program does with the KERNAL touches the rest of that stack
 *      area ($0840-$09FF, above our code): checksum it before and after
 *      screen scrolling and an attempt to open a file on the disk drive
 *      (the checksum must not change);
 *  (b) the original contents of $0800-$09FF are restored at exit. That is
 *      checked by the runner (vice-runner.py), not here.
 * The fopen is expected to fail under VICE's autostart (no file of that name);
 * it is only there to run the OPEN/serial-bus KERNAL routines.
 */

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
  unsigned int before, after;
  before = checksum();
  c128_bank1_call(touch);
  for (i = 0; i < 40; i++) printf("line %d\n", i);
  {
    FILE *f = fopen("no-such-file", "r");
    if (f) {
      for (i = 0; i < 100; i++) fgetc(f);
      fclose(f);
    }
  }
  c128_bank1_call(touch);
  after = checksum();
  test_set_result(before == after);
  return 0;
}
