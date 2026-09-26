// Copyright 2026 LLVM-MOS Project
// Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
// See https://github.com/llvm-mos/llvm-mos-sdk/blob/main/LICENSE for license
// information.

#include <string.h>

#include "cache-internal.h"

// Populates statically-placed bank-1 content (MOS_C128_BANK1_CODE/_DATA,
// see bank1.h) at program startup. Ordinary PRG loading only populates
// whichever bank is mapped at load time, so this content's bytes travel
// in the loaded (bank-0) image and have to be copied into bank-1
// physical RAM here.
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
extern char __c128commoncode_save_start[];

void __c128bank1_copy_chunk(char *dest, const char *src, unsigned char count);

// Populates .c128commoncode (c128_bank1_call/__c128bank1_copy_chunk's own
// code, bank1.s) at startup - it has the same "ordinary PRG loading
// doesn't populate it" problem bank-1 content does, since its VMA (by
// default $0800, see link.ld) isn't contiguous with the rest of the loaded
// image either. Unlike bank-1 content, this copy never crosses a bank
// boundary (both ends are ordinary bank-0 memory), so a plain memcpy is
// correct and sufficient - no chunking or __c128bank1_copy_chunk needed.
// The area's original contents (by default the idle low end of BASIC's
// runtime stack) are saved first and put back by
// __c128bank1_restore_common_code at exit, so the program leaves that
// memory as it found it. Both are triggered from bank1.s (.init.012 /
// .fini.988), before anything (including __c128bank1_load below) could
// call the functions this places.
void __c128bank1_load_common_code(void) {
  unsigned short size = (unsigned short)&__c128commoncode_size;
  memcpy(__c128commoncode_save_start, __c128commoncode_vma_start, size);
  memcpy(__c128commoncode_vma_start, __c128commoncode_lma_start, size);
}

void __c128bank1_restore_common_code(void) {
  memcpy(__c128commoncode_vma_start, __c128commoncode_save_start,
         (unsigned short)&__c128commoncode_size);
}

#define C128BANK1_CHUNK 16

// Staging buffer for the chunked copy. It must be Common RAM, and zero page is
// the only ordinary variable storage that is: this costs C128BANK1_CHUNK bytes
// of the (scarce) zero-page pool in every program that uses bank 1.
static char
    __attribute__((section(".zp.bss"))) __c128bank1_scratch[C128BANK1_CHUNK];

// Transfers between banks, a chunk at a time through the Common-RAM scratch
// buffer, in one routine to keep the code small: a write copies bank-0-visible
// memory into bank 1, a read the other way, and a move copies within bank 1.
// The copy loop is symmetric: with bank 1 mapped, the scratch buffer is Common
// RAM, and addresses on the bank-1 side are read or written in bank 1. A move
// copies forward, so it is only safe downward (destination below source).
enum { XFER_WRITE, XFER_READ, XFER_MOVE };

static __attribute__((noinline)) void
xfer(unsigned char mode, char *dst, const char *src, unsigned short size) {
  while (size) {
    unsigned char chunk =
        size > C128BANK1_CHUNK ? C128BANK1_CHUNK : (unsigned char)size;
    if (mode == XFER_WRITE) {
      memcpy(__c128bank1_scratch, src, chunk);
    } else {
      __c128bank1_copy_chunk(__c128bank1_scratch, src, chunk);
      if (mode == XFER_READ)
        memcpy(dst, __c128bank1_scratch, chunk);
    }
    if (mode != XFER_READ)
      __c128bank1_copy_chunk(dst, __c128bank1_scratch, chunk);
    dst += chunk;
    src += chunk;
    size -= chunk;
  }
}

static void c128bank1_zero_region(char *vma, unsigned short size) {
  memset(__c128bank1_scratch, 0, C128BANK1_CHUNK);
  while (size) {
    unsigned char chunk =
        size > C128BANK1_CHUNK ? C128BANK1_CHUNK : (unsigned char)size;
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
// run. bank1.s's own .init.201 calls this function directly instead,
// which both supplies the reference that pulls this object in - since
// bank1.s itself is always linked whenever c128_bank1_call is used, i.e.
// whenever bank-1 placement is used at all - and gets the ordering right
// (after .init.200's ordinary .data/.bss init, after .init.011's
// Common-RAM bump).
void __c128bank1_load(void) {
  xfer(XFER_WRITE, __c128bank1_text_vma_start, __c128bank1_text_lma_start,
       (unsigned short)&__c128bank1_text_size);
  xfer(XFER_WRITE, __c128bank1_data_vma_start, __c128bank1_data_lma_start,
       (unsigned short)&__c128bank1_data_size);
  c128bank1_zero_region(__c128bank1_bss_vma_start,
                        (unsigned short)&__c128bank1_bss_size);
}

// Run-time copies between bank 0 and bank 1 (declared in bank1.h and
// cache-internal.h), staged through the same Common-RAM scratch buffer (so
// using them costs no zero page beyond the 16 bytes bank-1 placement already
// costs). The bank-1 side is an address, not a pointer: see bank1.h.
void c128_bank1_write(unsigned short bank1_dest, const void *src,
                      unsigned short size) {
  xfer(XFER_WRITE, (char *)bank1_dest, (const char *)src, size);
}

void c128_bank1_read(void *dest, unsigned short bank1_src,
                     unsigned short size) {
  xfer(XFER_READ, (char *)dest, (const char *)bank1_src, size);
}

// Move size bytes within bank 1, downward (bank1_dst <= bank1_src). Private to
// the cache runtime (cache-internal.h).
void __c128bank1_move(unsigned short bank1_dst, unsigned short bank1_src,
                      unsigned short size) {
  xfer(XFER_MOVE, (char *)bank1_dst, (const char *)bank1_src, size);
}
