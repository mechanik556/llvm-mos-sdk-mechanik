#include <bank1.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A pseudo-random mixture of malloc/free/realloc/calloc/aligned_alloc, with
 * bank-1 calls in between, on a small arena so blocks get reused and the arena
 * runs out. Every live block holds a pattern that must survive everything else;
 * no block may overlap the live Common-RAM save buffer or lie outside the
 * arena; bank-1 data must be intact throughout. The runner also checks
 * $0800-$09FF and RCR at exit (desktop VICE). */

extern char __heap_start;
extern char __c128commoncode_save_start[];
extern void __c128commoncode_size;
size_t __set_heap_limit(size_t limit);

MOS_C128_BANK1_DATA static volatile unsigned char counter;
static volatile unsigned char __attribute__((section(".zp.bss"))) seen;
MOS_C128_BANK1_CODE static void bump(void) { counter++; }
MOS_C128_BANK1_CODE static void look(void) { seen = counter; }

#define SLOTS 24
#define ARENA 2400
static struct {
  unsigned char *p;
  unsigned char n, tag;
} slot[SLOTS];

static uint16_t rng = 0xACE1;
static unsigned char next(void) {
  rng = rng * 25173u + 13849u;
  return (unsigned char)(rng >> 8);
}

static int fill_ok(unsigned char *p, unsigned char n, unsigned char tag) {
  unsigned char i;
  for (i = 0; i < n; i++)
    if (p[i] != tag)
      return 0;
  return 1;
}

static int placed_ok(unsigned char *p, unsigned char n) {
  uintptr_t a = (uintptr_t)p, save = (uintptr_t)__c128commoncode_save_start;
  uintptr_t save_end = save + (uintptr_t)&__c128commoncode_size;
  if (a < (uintptr_t)&__heap_start || a + n > (uintptr_t)&__heap_start + ARENA)
    return 0;
  return !(a < save_end && a + n > save);
}

int main(void) {
  unsigned char i, op, k, n, tag = 1, bumps = 0;
  unsigned int step;

  if (__set_heap_limit(ARENA) < ARENA - 2)
    return EXIT_FAILURE;
  for (step = 0; step < 600; step++) {
    k = next() % SLOTS;
    op = next() % 6;
    n = 1 + next() % 90;
    if (op == 5) {
      c128_bank1_call(bump);
      bumps++;
      continue;
    }
    if (slot[k].p && op == 0) { /* free */
      if (!fill_ok(slot[k].p, slot[k].n, slot[k].tag))
        return EXIT_FAILURE;
      free(slot[k].p);
      slot[k].p = 0;
    } else if (slot[k].p && op == 1) { /* realloc */
      unsigned char *q = realloc(slot[k].p, n);
      unsigned char keep = n < slot[k].n ? n : slot[k].n;
      if (q) {
        if (!placed_ok(q, n) || !fill_ok(q, keep, slot[k].tag))
          return EXIT_FAILURE;
        slot[k].p = q;
        slot[k].n = n;
        slot[k].tag = ++tag;
        memset(q, slot[k].tag, n);
      } else if (!fill_ok(slot[k].p, slot[k].n, slot[k].tag)) {
        return EXIT_FAILURE; /* a failed realloc must leave the block intact */
      }
    } else if (!slot[k].p) { /* allocate, three different ways */
      unsigned char *q;
      if (op == 2)
        q = calloc(1, n);
      else if (op == 3)
        q = aligned_alloc(4, n);
      else
        q = malloc(n);
      if (q) {
        if (!placed_ok(q, n))
          return EXIT_FAILURE;
        if (op == 2 && !fill_ok(q, n, 0))
          return EXIT_FAILURE;
        if (op == 3 && ((uintptr_t)q & 3))
          return EXIT_FAILURE;
        slot[k].p = q;
        slot[k].n = n;
        slot[k].tag = ++tag;
        memset(q, slot[k].tag, n);
      }
    }
  }
  for (i = 0; i < SLOTS; i++)
    if (slot[i].p && !fill_ok(slot[i].p, slot[i].n, slot[i].tag))
      return EXIT_FAILURE;
  c128_bank1_call(look);
  return seen == bumps ? EXIT_SUCCESS : EXIT_FAILURE;
}
