#include "../test-check.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* realloc in every branch, on a small arena. Each scenario checks that the
 * resized block keeps its contents, that its neighbours are untouched, and (at
 * the end) that the heap's accounting is consistent: after everything is freed
 * the free total is back to what it was, and one allocation of almost the whole
 * arena succeeds (the chunks coalesced and the heap can still be walked).
 *
 * Regression test for realloc's shrink path, which used to compute the freed
 * size as (new - old) instead of (old - new) and, for a shrink too small to
 * leave a chunk of its own, still reduced the chunk size and orphaned the tail.
 */

static void fill(void *p, unsigned char n, unsigned char tag) {
  memset(p, tag, n);
}
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

int main(void) {
  unsigned char *a, *b, *c, *d;
  size_t total0, before;

  CHECK(__set_heap_limit(1024) >= 1000);
  a = malloc(8);
  CHECK(a);
  free(a);
  total0 = __heap_bytes_free();

  /* 1. shrink by a few bytes, next chunk in use: the remainder is too small to
   *    be a chunk, so the block must keep its size and the heap stay walkable
   */
  a = malloc(74);
  b = malloc(30);
  CHECK(a && b);
  fill(a, 74, 0x11);
  fill(b, 30, 0x22);
  d = realloc(a, 69);
  CHECK(d == a && holds(d, 69, 0x11) && holds(b, 30, 0x22));
  free(d);
  CHECK(holds(b, 30, 0x22));
  free(b);
  CHECK(__heap_bytes_free() == total0);

  /* 2. shrink a lot, next chunk in use: a new free chunk appears */
  a = malloc(100);
  b = malloc(30);
  CHECK(a && b);
  fill(a, 100, 0x33);
  fill(b, 30, 0x44);
  before = __heap_bytes_free();
  d = realloc(a, 20);
  CHECK(d == a && holds(d, 20, 0x33) && holds(b, 30, 0x44));
  CHECK(__heap_bytes_free() >= before + 70);
  free(d);
  free(b);
  CHECK(__heap_bytes_free() == total0);

  /* 3. shrink with a free chunk after it: they coalesce */
  a = malloc(100);
  b = malloc(40);
  c = malloc(30);
  CHECK(a && b && c);
  fill(a, 100, 0x55);
  fill(c, 30, 0x66);
  free(b);
  d = realloc(a, 30);
  CHECK(d == a && holds(d, 30, 0x55) && holds(c, 30, 0x66));
  b = malloc(100); /* needs the coalesced space */
  CHECK(b && disjoint(b, 100, d, 30) && disjoint(b, 100, c, 30));
  fill(b, 100, 0x77);
  CHECK(holds(d, 30, 0x55) && holds(c, 30, 0x66));
  free(b);
  free(c);
  free(d);
  CHECK(__heap_bytes_free() == total0);

  /* 4. shrink the last chunk of the arena */
  a = malloc(60);
  CHECK(a);
  b = malloc(total0 - 60 - 8); /* takes (nearly) everything else */
  if (b) {
    fill(b, 20, 0x88);
    c = realloc(b, 10);
    CHECK(c == b && holds(c, 10, 0x88));
    free(c);
  }
  free(a);
  CHECK(__heap_bytes_free() == total0);

  /* 5. grow in place into a free chunk (large remainder, then absorbing all) */
  a = malloc(40);
  b = malloc(200);
  c = malloc(30);
  CHECK(a && b && c);
  fill(a, 40, 0x99);
  fill(c, 30, 0xAA);
  free(b);
  d = realloc(a, 100);
  CHECK(d == a && holds(d, 40, 0x99) && holds(c, 30, 0xAA));
  fill(d, 100, 0x9A);
  CHECK(holds(c, 30, 0xAA));
  d = realloc(d, 236); /* all of the free chunk (40+200+overhead) */
  CHECK(d && holds(d, 100, 0x9A) && holds(c, 30, 0xAA));
  free(d);
  free(c);
  CHECK(__heap_bytes_free() == total0);

  /* 6. grow by moving (next chunk in use) */
  a = malloc(30);
  b = malloc(30);
  CHECK(a && b);
  fill(a, 30, 0xBB);
  fill(b, 30, 0xCC);
  d = realloc(a, 120);
  CHECK(d && holds(d, 30, 0xBB) && holds(b, 30, 0xCC));
  CHECK(disjoint(d, 120, b, 30));
  free(d);
  free(b);
  CHECK(__heap_bytes_free() == total0);

  /* the whole arena is one free chunk again */
  a = malloc(total0);
  CHECK(a);
  free(a);
  return EXIT_SUCCESS;
}
