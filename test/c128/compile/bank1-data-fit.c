#include <bank1.h>

// 32 KiB of initialized bank-1 data fits: bank 1 has room for it, and so does
// the program image, which carries a copy that startup moves into bank 1.
MOS_C128_BANK1_DATA volatile char big[0x8000] = {1};

MOS_C128_BANK1_CODE static void touch(void) { big[0x7FFF] = 2; }

int main(void) {
  c128_bank1_call(touch);
  return 0;
}
