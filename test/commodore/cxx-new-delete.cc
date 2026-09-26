#include <new>
#include <stdint.h>
#include <stdlib.h>

extern "C" {
size_t __set_heap_limit(size_t limit);
size_t __heap_bytes_free(void);
}

/* operator new/delete are thin wrappers over malloc/free: the heap is the same
 * bank-0 heap, exhaustion is reported the same way (nothrow new returns
 * nullptr), and the low-memory hook is consulted. */

namespace {
struct Obj {
  unsigned char b[50];
  static unsigned live;
  Obj() {
    for (unsigned i = 0; i < sizeof b; i++)
      b[i] = 7;
    live++;
  }
  ~Obj() { live--; }
};
unsigned Obj::live;

void *reserve;
unsigned hook_calls;
} // namespace

extern "C" int __malloc_low_memory(size_t) {
  hook_calls++;
  if (!reserve)
    return 0;
  free(reserve);
  reserve = nullptr;
  return 1;
}

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c))                                                                  \
      return EXIT_FAILURE;                                                     \
  } while (0)

int main() {
  __set_heap_limit(1000);
  size_t total0;
  {
    Obj *p = new Obj;
    CHECK(p && p->b[49] == 7 && Obj::live == 1);
    delete p;
    CHECK(Obj::live == 0);
  }
  total0 = __heap_bytes_free();

  /* arrays run every constructor and destructor */
  Obj *arr = new Obj[5];
  CHECK(arr && Obj::live == 5 && arr[4].b[0] == 7);
  delete[] arr;
  CHECK(Obj::live == 0);
  CHECK(__heap_bytes_free() == total0);

  delete static_cast<Obj *>(nullptr); /* harmless */

  /* exhaustion: the hook is consulted, then nothrow new reports failure */
  Obj *held[30];
  unsigned n = 0;
  while (n < 30) {
    held[n] = new (std::nothrow) Obj;
    if (!held[n])
      break;
    n++;
  }
  CHECK(n > 5 && n < 30);
  CHECK(hook_calls >= 1);

  /* with something for the hook to give up, the request succeeds after it */
  for (unsigned i = 0; i < n; i++)
    delete held[i];
  CHECK(__heap_bytes_free() == total0);

  reserve = malloc(400);
  CHECK(reserve);
  Obj *fill[30];
  unsigned m = 0;
  while (m < 30) {
    fill[m] = new (std::nothrow) Obj;
    if (!fill[m])
      break;
    m++;
  }
  CHECK(reserve == nullptr); /* the hook freed it during the fill */
  CHECK(m > 11);             /* and those bytes became objects (600/52) */
  for (unsigned i = 0; i < m; i++)
    delete fill[i];
  CHECK(__heap_bytes_free() == total0);
  return EXIT_SUCCESS;
}
