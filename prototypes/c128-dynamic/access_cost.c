#include <bank1.h>

/* Cost of ONE byte read from bank 1, versus an ordinary bank-0 load. Supports
 * the design argument (work/M0_C128_BANKING_PLAN.md, section 11.10) that bank-1
 * memory cannot be reached through ordinary pointers, so a "transparent"
 * bank-aware malloc/new would turn every dereference into a call like this one.
 *
 * The only way to read bank 1 is code that is visible in both banks (Common
 * RAM), running with interrupts off: here c128_bank1_call invoking an accessor.
 * The index is passed and the result returned through zero page, as bank1.h
 * requires, so those stores/loads are part of the measured cost.
 *
 * t_* are totals for REPS repetitions, from CIA2 timer A with the display
 * blanked (no badline cycle stealing): per-access cost = (t - t_empty) / REPS.
 *
 * Build it STANDALONE and run it with access_cost.sh (linked together with the
 * module-loader objects, the compiler places these globals differently and the
 * absolute totals shift; the ~100-cycle vs ~4-cycle gap does not).
 * Measured 2026-09-24 (VICE x128): t_empty=167, t_direct=229, t_call=1845, ok=1,
 * i.e. about 3.9 cycles for a direct load and 104.9 for the bank-1 read (~27x). */

#define REPS 16
#define CIA2_TA_LO (*(volatile unsigned char *)0xDD04)
#define CIA2_TA_HI (*(volatile unsigned char *)0xDD05)
#define CIA2_CRA (*(volatile unsigned char *)0xDD0E)
#define VIC_D011 (*(volatile unsigned char *)0xD011)

MOS_C128_BANK1_DATA static volatile unsigned char tbl[16] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static volatile unsigned char __attribute__((section(".zp.bss"))) zr;
static volatile unsigned char __attribute__((section(".zp.bss"))) zi;
MOS_C128_BANK1_CODE static void get(void) { zr = tbl[zi]; }

static volatile unsigned char b0tbl[16] = {1, 2, 3,  4,  5,  6,  7,  8,
                                           9, 10, 11, 12, 13, 14, 15, 16};
volatile unsigned int t_empty, t_direct, t_call;
volatile unsigned char sink, ok;
static unsigned int dl;

#define START()                                                                \
  do {                                                                         \
    __asm__ volatile("sei");                                                   \
    CIA2_TA_LO = 0xFF;                                                         \
    CIA2_TA_HI = 0xFF;                                                         \
    CIA2_CRA = 0x10;                                                           \
    CIA2_CRA = 0x01;                                                           \
  } while (0)
#define STOP(dst)                                                              \
  do {                                                                         \
    unsigned int left;                                                         \
    CIA2_CRA = 0;                                                              \
    left = CIA2_TA_LO;                                                         \
    left |= (unsigned int)CIA2_TA_HI << 8;                                     \
    __asm__ volatile("cli");                                                   \
    dst = 0xFFFF - left;                                                       \
  } while (0)

int main(void) {
  unsigned char i;
  unsigned char d011 = VIC_D011;
  /* Blank the display once and wait many frames: the VIC samples DEN at raster
   * line $30, so blanking only takes effect on the next frame. */
  VIC_D011 = d011 & ~0x10;
  for (dl = 0; dl < 30000; dl++)
    sink = 0;
  START();
  for (i = 0; i < REPS; i++)
    sink = i;
  STOP(t_empty);
  START();
  for (i = 0; i < REPS; i++)
    sink = b0tbl[i];
  STOP(t_direct);
  START();
  for (i = 0; i < REPS; i++) {
    zi = i;
    c128_bank1_call(get);
    sink = zr;
  }
  STOP(t_call);
  ok = (sink == 16); /* the bank-1 reads returned the right data */
  VIC_D011 = d011;
  return 0;
}
