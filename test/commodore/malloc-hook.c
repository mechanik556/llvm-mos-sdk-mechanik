#include "../test-check.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The low-memory hook: this program defines __malloc_low_memory, which malloc
 * (and so calloc, realloc, aligned_alloc and operator new) calls when a request
 * cannot be satisfied, before failing. It may free memory and ask for a retry.
 * Programs that do not define it are unaffected.
 *
 * The hook here gives up one "reserve" block per call. The test checks that it
 * is called on exhaustion with nothing to give (the allocation fails), once per
 * block freed when it can help (with coalescing), for every allocator entry
 * point, that a request larger than the whole heap does not call it, and that
 * freeing memory from inside it leaves the heap consistent. */

size_t __heap_limit(void);

static void *reserve[8];
static unsigned char nres;
static unsigned calls;
static size_t last_needed;
static unsigned char give; /* 1: the hook frees a reserve block per call */
static unsigned char hook_bad;

/* Returns non-zero if it freed something (then malloc retries). */
int __malloc_low_memory(size_t needed) {
  size_t before;
  calls++;
  last_needed = needed;
  if (!give || !nres)
    return 0;
  before = __heap_bytes_free();
  free(reserve[--nres]); /* allowed: the hook may free */
  if (__heap_bytes_free() <= before)
    hook_bad = 1;
  return 1;
}

#define NF 40
static unsigned char *filler[NF];
static unsigned char nfill;

static int holds(const void *p, unsigned n, unsigned char tag) {
  const unsigned char *q = p;
  unsigned i;
  for (i = 0; i < n; i++)
    if (q[i] != tag)
      return 0;
  return 1;
}

/* Build the state: `nreserve` adjacent 100-byte reserve blocks, then small
 * filler blocks until the arena is full. Returns 0 on success. */
static int build(unsigned char nreserve) {
  unsigned char i;
  nres = 0;
  nfill = 0;
  for (i = 0; i < nreserve; i++) {
    reserve[i] = malloc(100);
    if (!reserve[i])
      return 1;
    memset(reserve[i], 0xEE, 100);
    nres++;
  }
  give = 0;
  while (nfill < NF) {
    unsigned char *p = malloc(30);
    if (!p)
      break;
    memset(p, 0x40 + nfill, 30);
    filler[nfill++] = p;
  }
  return nfill == 0 || nfill == NF;
}

static void teardown(void) {
  unsigned char i;
  for (i = 0; i < nres; i++)
    free(reserve[i]);
  nres = 0;
  for (i = 0; i < nfill; i++)
    free(filler[i]);
  nfill = 0;
}

static int fillers_ok(void) {
  unsigned char i;
  for (i = 0; i < nfill; i++)
    if (!holds(filler[i], 30, 0x40 + i))
      return 0;
  return 1;
}

int main(void) {
  size_t total0;
  unsigned char *p, *q;
  unsigned i;

  CHECK(__set_heap_limit(1000) >= 990);
  p = malloc(8);
  CHECK(p);
  free(p);
  total0 = __heap_bytes_free();

  /* 1. exhaustion, reclaim has nothing to give: it is called, the allocation
   *    fails as it always did */
  CHECK(!build(0));
  calls = 0;
  CHECK(malloc(30) == NULL);
  CHECK(calls >= 1 && last_needed == 32); /* 30 bytes + 2 header */
  teardown();
  CHECK(__heap_bytes_free() == total0);

  /* 2. reclaim frees a reserve block per call until the request fits */
  CHECK(!build(5));
  CHECK(fillers_ok());
  give = 1;
  calls = 0;
  p = malloc(180); /* two adjacent 100-byte blocks must be freed and coalesce */
  CHECK(p);
  CHECK(calls == 2 && nres == 3 && !hook_bad);
  memset(p, 0x77, 180);
  CHECK(fillers_ok());
  free(p);
  teardown();
  CHECK(__heap_bytes_free() == total0);

  /* 3. every allocator entry point is covered */
  CHECK(!build(4));
  give = 1;
  calls = 0;
  p = calloc(1, 90);
  CHECK(p && calls >= 1 && holds(p, 90, 0)); /* calloc zeroes what it got */
  free(p);
  teardown();

  CHECK(!build(4));
  give = 1;
  calls = 0;
  p = aligned_alloc(8, 90);
  CHECK(p && calls >= 1 && ((uintptr_t)p & 7) == 0);
  memset(p, 0x55, 90);
  CHECK(fillers_ok());
  free(p);
  teardown();

  CHECK(!build(4));
  q = filler[nfill - 1]; /* a filler block: growing it in place is impossible */
  give = 1;
  calls = 0;
  p = realloc(q, 160);
  CHECK(p && calls >= 1 && holds(p, 30, 0x40 + nfill - 1));
  filler[nfill - 1] = p; /* now bigger; teardown frees it as a filler */
  teardown();
  CHECK(__heap_bytes_free() == total0);

  /* 4. a request bigger than the whole heap does not call reclaim */
  CHECK(!build(3));
  give = 1;
  calls = 0;
  CHECK(malloc(__heap_limit() + 100) == NULL);
  CHECK(calls == 0 && nres == 3);
  teardown();

  /* 5. with several blocks given up over time, the heap stays consistent */
  for (i = 0; i < 3; i++) {
    CHECK(!build(6));
    give = 1;
    p = malloc(250);
    CHECK(p && !hook_bad);
    free(p);
    teardown();
    CHECK(__heap_bytes_free() == total0);
  }
  p = malloc(total0);
  CHECK(p);
  free(p);
  return EXIT_SUCCESS;
}
