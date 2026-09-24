#include <string.h>

/* Bank-0 <-> bank-1 transfers for the module/object loader (modtab.c), staged
 * through a Common-RAM (zero-page) scratch buffer with the platform's
 * __c128bank1_copy_chunk, which copies with bank 1 mapped. The platform library
 * only needs such a copy at startup and keeps its own private one; the loader
 * needs both directions at run time, so it has its own. */

void __c128bank1_copy_chunk(char *dest, const char *src, unsigned char count);

#define XFER_CHUNK 16

static char __attribute__((section(".zp.bss"))) xfer_scratch[XFER_CHUNK];

/* Copy size bytes from bank-0-visible src into bank-1 RAM at vma. */
void proto_bank1_write(char *vma, const char *lma, unsigned short size) {
  while (size) {
    unsigned char chunk = size > XFER_CHUNK ? XFER_CHUNK : (unsigned char)size;
    memcpy(xfer_scratch, lma, chunk);
    __c128bank1_copy_chunk(vma, xfer_scratch, chunk);
    vma += chunk;
    lma += chunk;
    size -= chunk;
  }
}

/* Copy size bytes from bank-1 RAM at src into bank-0-visible dest. */
void proto_bank1_read(char *dest, const char *src, unsigned short size) {
  while (size) {
    unsigned char chunk = size > XFER_CHUNK ? XFER_CHUNK : (unsigned char)size;
    __c128bank1_copy_chunk(xfer_scratch, src, chunk);
    memcpy(dest, xfer_scratch, chunk);
    dest += chunk;
    src += chunk;
    size -= chunk;
  }
}
