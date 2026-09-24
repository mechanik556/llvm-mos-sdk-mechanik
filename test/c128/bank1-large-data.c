#include <bank1.h>
#include <stdlib.h>

/* The load-time copy of bank-1 data crosses many 255-byte chunks and page
 * boundaries. bank1_table holds 4096 initialized bytes; bank-1 code sums them
 * (16-bit), and bank-0 code computes the expected sum from the same formula.
 * A dropped, duplicated or misplaced chunk changes the sum. The first and
 * last bytes are also checked on their own. */

#define E1(n) ((unsigned char)((n) * 7 + 3))
#define E4(n) E1(n), E1((n) + 1), E1((n) + 2), E1((n) + 3)
#define E16(n) E4(n), E4((n) + 4), E4((n) + 8), E4((n) + 12)
#define E64(n) E16(n), E16((n) + 16), E16((n) + 32), E16((n) + 48)
#define E256(n) E64(n), E64((n) + 64), E64((n) + 128), E64((n) + 192)
#define E1024(n) E256(n), E256((n) + 256), E256((n) + 512), E256((n) + 768)
#define TABLE_SIZE 4096

MOS_C128_BANK1_DATA static volatile unsigned char bank1_table[TABLE_SIZE] = {
    E1024(0), E1024(1024), E1024(2048), E1024(3072)};

#define ZP __attribute__((section(".zp.bss")))
static volatile unsigned int ZP r_sum;
static volatile unsigned char ZP r_first, r_last;

MOS_C128_BANK1_CODE static void sum_table(void) {
  unsigned int i, s = 0;
  for (i = 0; i < TABLE_SIZE; i++) s += bank1_table[i];
  r_sum = s;
  r_first = bank1_table[0];
  r_last = bank1_table[TABLE_SIZE - 1];
}

int main(void) {
  unsigned int i, expect = 0;
  for (i = 0; i < TABLE_SIZE; i++) expect += (unsigned char)(i * 7 + 3);
  c128_bank1_call(sum_table);
  return (r_sum == expect && r_first == E1(0) && r_last == E1(TABLE_SIZE - 1)) ? EXIT_SUCCESS : EXIT_FAILURE;
}
