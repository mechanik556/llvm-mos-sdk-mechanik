// Bank 0 has less than 48 KiB for the program: this must not link.
volatile char big[0xC000];

int main(void) {
  big[0] = 1;
  return big[0xBFFF];
}
