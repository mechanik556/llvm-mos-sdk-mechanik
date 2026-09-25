#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* aligned_alloc with alignments above 2, mixed with ordinary malloc/free on a
 * small arena. Each block must be correctly aligned, must not overlap anything
 * else, and must keep its contents; after everything is freed the heap's free
 * total must be back to what it was and one allocation of the whole arena must
 * succeed (the free list is intact and everything coalesced).
 *
 * Regression test: aligned_alloc used to re-insert the free chunk it had found
 * without removing it from the free list first, split it without leaving room
 * for the free chunk in front, and allocate the inflated search size. */

size_t __set_heap_limit(size_t limit);
size_t __heap_bytes_free(void);

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c))                                                                  \
      return EXIT_FAILURE;                                                     \
  } while (0)

static int holds(const void *p, unsigned char n, unsigned char tag) {
  const unsigned char *q = p;
  unsigned char i;
  for (i = 0; i < n; i++)
    if (q[i] != tag)
      return 0;
  return 1;
}
static int disjoint(const void *a, unsigned char an, const void *b,
                    unsigned char bn) {
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
  return x + an <= y || y + bn <= x;
}

#define N 12
static unsigned char *blk[N];
static unsigned char len[N];

int main(void) {
  static const unsigned char al[] = {4, 8, 16, 32, 64};
  size_t total0;
  unsigned char r, i, j;
  unsigned char *p;

  CHECK(__set_heap_limit(1200) >= 1100);
  p = malloc(8);
  CHECK(p);
  free(p);
  total0 = __heap_bytes_free();

  for (r = 0; r < sizeof al; r++) {
    /* interleave ordinary and aligned blocks so the free list has holes */
    for (i = 0; i < N; i++) {
      len[i] = 10 + (unsigned char)(i * 7 + r * 3) % 40;
      blk[i] = (i & 1) ? aligned_alloc(al[r], len[i]) : malloc(len[i]);
      CHECK(blk[i]);
      if (i & 1)
        CHECK(((uintptr_t)blk[i] & (al[r] - 1)) == 0);
      memset(blk[i], (unsigned char)(0x40 + i), len[i]);
    }
    for (i = 0; i < N; i++) {
      CHECK(holds(blk[i], len[i], (unsigned char)(0x40 + i)));
      for (j = i + 1; j < N; j++)
        CHECK(disjoint(blk[i], len[i], blk[j], len[j]));
    }
    /* free every other block, then allocate aligned into the holes */
    for (i = 0; i < N; i += 2) {
      free(blk[i]);
      blk[i] = 0;
    }
    for (i = 0; i < N; i += 2) {
      blk[i] = aligned_alloc(al[r], len[i]);
      CHECK(blk[i]);
      CHECK(((uintptr_t)blk[i] & (al[r] - 1)) == 0);
      memset(blk[i], (unsigned char)(0x80 + i), len[i]);
    }
    for (i = 0; i < N; i++) {
      CHECK(holds(blk[i], len[i],
                  (unsigned char)((i & 1) ? 0x40 + i : 0x80 + i)));
      for (j = i + 1; j < N; j++)
        CHECK(disjoint(blk[i], len[i], blk[j], len[j]));
    }
    for (i = 0; i < N; i++)
      free(blk[i]);
    CHECK(__heap_bytes_free() == total0);
  }

  p = malloc(total0);
  CHECK(p);
  free(p);
  return EXIT_SUCCESS;
}
