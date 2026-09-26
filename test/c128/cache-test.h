#ifndef CACHE_TEST_H
#define CACHE_TEST_H

#include <cache.h>

// Fill an object's first n bytes with a pattern derived from seed, and check
// them later. Both go through lock/unlock, so they also move the object into
// bank 0. Return 1 on success (0 if the lock was refused, or on a mismatch).
static inline int fill(mos_cache_handle_t h, unsigned n, unsigned char seed) {
  unsigned char *p = mos_cache_lock(h);
  unsigned i;
  if (!p)
    return 0;
  for (i = 0; i < n; i++)
    p[i] = (unsigned char)(seed + i * 7);
  mos_cache_unlock(h);
  return 1;
}

static inline int check(mos_cache_handle_t h, unsigned n, unsigned char seed) {
  unsigned char *p = mos_cache_lock(h);
  unsigned i;
  int ok = 1;
  if (!p)
    return 0;
  for (i = 0; i < n; i++)
    if (p[i] != (unsigned char)(seed + i * 7))
      ok = 0;
  mos_cache_unlock(h);
  return ok;
}

#endif // CACHE_TEST_H
