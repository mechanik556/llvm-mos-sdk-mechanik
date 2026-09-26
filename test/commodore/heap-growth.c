#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Heap limit and exhaustion. Grows the arena in steps to the maximum safe size,
 * allocating until malloc returns NULL each time (no low-memory hook is defined
 * here, so exhaustion must simply be NULL). Checks that every block lies inside
 * [__heap_start, __heap_start + limit), that the arena stays below the soft
 * stack, that patterns survive, and that freeing everything gives all the
 * memory back. */

extern char __heap_start;
size_t __heap_limit(void);
size_t __set_heap_limit(size_t limit);
size_t __get_heap_max_safe_size(void);
size_t __heap_bytes_free(void);

#define BLOCK 62
#define MAXBLOCKS 800
static unsigned char *blk[MAXBLOCKS];

static unsigned int fill_until_null(unsigned int n, unsigned char tag0) {
  unsigned int i = n;
  while (i < MAXBLOCKS) {
    unsigned char *p = malloc(BLOCK);
    uintptr_t a = (uintptr_t)p;
    if (!p)
      break;
    if (a < (uintptr_t)&__heap_start ||
        a + BLOCK > (uintptr_t)&__heap_start + __heap_limit())
      return 0xFFFF;
    blk[i] = p;
    memset(p, (unsigned char)(tag0 + i), BLOCK);
    i++;
  }
  return i;
}

int main(void) {
  size_t max_safe = __get_heap_max_safe_size(), lim;
  unsigned int n0, n1, n2, i, j;
  size_t free_before, free_after;

  if (__set_heap_limit(1000) < 990 || __heap_limit() > 1000)
    return EXIT_FAILURE;
  free_before = __heap_bytes_free();
  n0 = fill_until_null(0, 1);
  if (n0 == 0xFFFF || n0 < 8 || n0 > 16 || malloc(BLOCK))
    return EXIT_FAILURE;
  lim = __set_heap_limit(3000); /* grows */
  if (lim < 2990 || lim > 3000)
    return EXIT_FAILURE;
  n1 = fill_until_null(n0, 1);
  if (n1 == 0xFFFF || n1 <= n0 || malloc(BLOCK))
    return EXIT_FAILURE;
  lim = __set_heap_limit(0xFFFF); /* capped to what is safe */
  if (lim > max_safe || (uintptr_t)&__heap_start + lim > 0xFFFF)
    return EXIT_FAILURE;
  n2 = fill_until_null(n1, 1);
  if (n2 == 0xFFFF || n2 <= n1 || malloc(BLOCK))
    return EXIT_FAILURE;
  for (i = 0; i < n2; i++)
    for (j = 0; j < BLOCK; j++)
      if (blk[i][j] != (unsigned char)(1 + i))
        return EXIT_FAILURE;
  for (i = 0; i < n2; i++)
    free(blk[i]);
  free_after = __heap_bytes_free();
  /* everything coalesced back: the growth steps only added free space */
  return free_after >= free_before + (lim - 1000) - 8 ? EXIT_SUCCESS
                                                      : EXIT_FAILURE;
}
