#include <string.h>

// Populates statically-placed bank-1 content (MOS_C128_BANK1_CODE/_DATA,
// see bank1.h) at program startup. Ordinary PRG loading only populates
// whichever bank is mapped at load time, so this content's bytes travel
// in the loaded (bank-0) image and have to be copied into bank-1
// physical RAM here - see work/M0_C128_BANKING_PLAN.md section 4.1 task
// 4 in llvm-mos-mechanik for the full investigation behind this file.
//
// __c128bank1_copy_chunk (bank1.s) does the actual bank-1-side write,
// CHUNK bytes at a time, via a Common-RAM scratch buffer: this file's
// job is just staging each chunk into that buffer (an ordinary bank-0
// copy, since both the buffer and the LMA source are bank-0-reachable)
// and driving the loop.

extern char __c128bank1_text_vma_start[];
extern char __c128bank1_text_lma_start[];
extern void __c128bank1_text_size;

extern char __c128bank1_data_vma_start[];
extern char __c128bank1_data_lma_start[];
extern void __c128bank1_data_size;

extern char __c128bank1_bss_vma_start[];
extern void __c128bank1_bss_size;

extern char __c128commoncode_vma_start[];
extern char __c128commoncode_lma_start[];
extern void __c128commoncode_size;

void __c128bank1_copy_chunk(char *dest, const char *src, unsigned char count);

// Populates .c128commoncode (c128_bank1_call/__c128bank1_copy_chunk's own
// code, bank1.s) at startup - it has the exact same "ordinary PRG loading
// doesn't populate it" problem bank-1 content does, since its VMA
// ($0C00-$0DFF) isn't contiguous with the rest of the loaded image
// either. Unlike bank-1 content, this copy never crosses a bank
// boundary (both ends are ordinary bank-0 memory), so a plain memcpy is
// correct and sufficient - no chunking or __c128bank1_copy_chunk needed.
// Triggered from bank1.s's own .init.012, before anything (including
// __c128bank1_load below) could call the functions this places - see
// that file for why bank1.s does the triggering rather than this file
// registering itself.
void __c128bank1_load_common_code(void) {
  memcpy(__c128commoncode_vma_start, __c128commoncode_lma_start,
         (unsigned short)&__c128commoncode_size);
}

#define C128BANK1_CHUNK 16

static char __attribute__((section(".zp.bss"))) __c128bank1_scratch[C128BANK1_CHUNK];

static void c128bank1_copy_region(char *vma, const char *lma, unsigned short size) {
  while (size) {
    unsigned char chunk = size > C128BANK1_CHUNK ? C128BANK1_CHUNK : (unsigned char)size;
    memcpy(__c128bank1_scratch, lma, chunk);
    __c128bank1_copy_chunk(vma, __c128bank1_scratch, chunk);
    vma += chunk;
    lma += chunk;
    size -= chunk;
  }
}

static void c128bank1_zero_region(char *vma, unsigned short size) {
  memset(__c128bank1_scratch, 0, C128BANK1_CHUNK);
  while (size) {
    unsigned char chunk = size > C128BANK1_CHUNK ? C128BANK1_CHUNK : (unsigned char)size;
    __c128bank1_copy_chunk(vma, __c128bank1_scratch, chunk);
    vma += chunk;
    size -= chunk;
  }
}

// Deliberately NOT self-registering via its own .init.NNN section here
// (unlike common/crt0/copy-data.c's __do_copy_data pattern) - this file
// is an ordinary lazily-linked library member (like bank1.s), with
// nothing in it that user code ever references directly, so nothing
// would pull it out of the archive and its hook would silently never
// run. bank1.s's own .init.201 (right after its .init.011 Common-RAM
// bump) calls this function directly instead, which both supplies the
// reference that pulls this object in - since bank1.s itself is always
// linked whenever c128_bank1_call is used, i.e. whenever bank-1
// placement is used at all - and gets the ordering right (after
// .init.200's ordinary .data/.bss init, after .init.011's Common-RAM
// bump).
void __c128bank1_load(void) {
  c128bank1_copy_region(__c128bank1_text_vma_start, __c128bank1_text_lma_start,
                         (unsigned short)&__c128bank1_text_size);
  c128bank1_copy_region(__c128bank1_data_vma_start, __c128bank1_data_lma_start,
                         (unsigned short)&__c128bank1_data_size);
  c128bank1_zero_region(__c128bank1_bss_vma_start,
                         (unsigned short)&__c128bank1_bss_size);
}
