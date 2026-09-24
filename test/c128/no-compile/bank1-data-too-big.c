#include <bank1.h>

// Initialized bank-1 data larger than the program image can carry: this must
// not link.
MOS_C128_BANK1_DATA volatile char big[0xB001] = {1};

MOS_C128_BANK1_CODE static void touch(void) { big[0] = 2; }

int main(void) {
  c128_bank1_call(touch);
  return 0;
}
