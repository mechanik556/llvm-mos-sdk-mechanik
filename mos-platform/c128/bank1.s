; Copyright 2026 LLVM-MOS Project
; Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
; See https://github.com/llvm-mos/llvm-mos-sdk/blob/main/LICENSE for license
; information.

.include "c128.inc"
.include "imag.inc"

; c128_bank1_call(method): call a function that lives in C128 RAM bank 1
; (with KERNAL ROM/I-O still mapped in, per MMU_CFG_RAM1_KERNAL) from code
; running in bank 0, then return to bank 0. Modeled on
; mos-platform/nes-mmc3/mapper.s's banked_call_8000.
;
; Interrupts are disabled for the duration of the call: a bank switch changes
; what an interrupt handler would see at $4000-$BFFF too. The caller's I flag
; is saved and restored (PHP/PLP, not SEI/CLI), so it is also safe to call with
; interrupts already disabled. NMIs are not masked; the KERNAL ROM stays
; mapped, so its NMI handler remains reachable.
;
; method arrives in __rc2/__rc3 and is forwarded to the fixed slot the shared
; __call_indir uses (__rc18/__rc19) before switching banks: __rc0-31 are zero
; page, hence Common RAM, reachable whichever bank is mapped.
;
; Placed in .c128commoncode.*, not ordinary .text: the instant MMU_CR selects
; bank 1 the next instruction fetch must land on the same physical bytes as in
; bank 0, which only holds inside the Common-RAM-Lo window (link.ld's
; `common_code` region). For the same reason it does not call the shared
; __call_indir (common/crt/call-indir.S), which is ordinary .text and would not
; be reachable with bank 1 mapped; a local copy of its one instruction, in this
; same section, is used instead.
.section .c128commoncode.c128_bank1_call,"ax",@progbits
.globl c128_bank1_call
c128_bank1_call:
	ldx __rc2
	stx __rc18
	ldx __rc3
	stx __rc19
	php
	lda MMU_CR
	pha
	sei
	lda #MMU_CFG_RAM1_KERNAL
	sta MMU_CR
	jsr .Lc128_bank1_call_indir
	pla
	sta MMU_CR
	plp
	rts
.Lc128_bank1_call_indir:
	jmp (__rc18)

; Saved at .init.011, before the .bss is zeroed (.init.200), so it must not live
; in .bss (it would be restored as $00, disabling Common RAM at exit).
.section .noinit,"aw",@nobits
__rcrsave:
	.fill 1

; Enlarge Common RAM (RCR's Shared-Lo window) from the stock 1K to 4K:
; c128_bank1_call's zero-page and stack use (like any bank-switched code's)
; needs more than 1K to be reachable whichever bank is mapped. This object is
; only linked when the program references c128_bank1_call, so programs that
; never use bank 1 pay nothing for it.
;
; Only the Shared-Lo enable and size bits (2-0) are changed (read-modify-write,
; not a blind overwrite): the VIC bank select (bits 7-6) and the Shared-Hi
; enable (bit 3) are kept as they were. Note the size field is shared, so if
; Shared-Hi is enabled its size becomes 4K too.
.section .init.011,"ax",@progbits
	lda RCR
	sta __rcrsave
	and #%11111000
	ora #RCR_SHARED_LO_4K
	sta RCR

.section .fini.989,"ax",@progbits
	lda __rcrsave
	sta RCR

; Populate .c128commoncode (c128_bank1_call/__c128bank1_copy_chunk's own
; VMA, below) right after the Common-RAM bump above and before anything
; else - in particular before .init.201's __c128bank1_load, which is the
; first thing that actually calls __c128bank1_copy_chunk. This copy
; never crosses a bank boundary itself (both the common-code area and this
; content's LMA in `ram` are ordinary bank-0 memory, simultaneously
; reachable without any switch), so a plain call to the C library's own
; memcpy is fine here, unlike the bank-1 case.
.section .init.012,"ax",@progbits
	jsr __c128bank1_load_common_code

; Put the original contents of that area back at exit, before the RCR
; restore (.fini.989). (BASIC's memory configuration is restored later still,
; when control actually returns to BASIC: init-mmu.S.) Nothing after this
; point may call into bank 1.
.section .fini.988,"ax",@progbits
	jsr __c128bank1_restore_common_code

; Populate MOS_C128_BANK1_CODE/_DATA content (bank1-load.c) after
; ordinary .data/.bss init (.init.200) and after the Common-RAM bump
; above (.init.011). This jsr - not bank1-load.c's own registration - is
; what actually pulls bank1-load.o out of the library archive: nothing
; else ever references __c128bank1_load by name, and bank1-load.c is an
; ordinary lazily-linked library member like this file, so without this
; reference its .init hook would never link in at all, even for programs
; that do use bank-1 placement. This file (bank1.s) is always linked
; whenever c128_bank1_call is used, which is always true of any program
; using bank-1 placement (it's the only way to reach bank-1 content), so
; this reference reliably travels with it.
.section .init.201,"ax",@progbits
	jsr __c128bank1_load

; __c128bank1_copy_chunk(char *bank1_dest, const char *common_ram_src,
;                         unsigned char count)
;
; Copies count bytes (1-255; count=0 is not supported and would copy 256)
; from common_ram_src to bank1_dest, with RAM bank 1 mapped for the duration.
; common_ram_src must be Common RAM (reachable whichever bank is mapped, e.g.
; __c128bank1_scratch in bank1-load.c). This is the inner primitive of the
; Common-RAM-staged chunked copy that populates static bank-1 content at
; program startup (bank1-load.c): ordinary PRG loading only populates
; whichever bank is mapped at load time.
;
; The whole copy loop runs with bank 1 mapped, so it cannot call any ordinary
; (non-Common-RAM) subroutine such as memcpy; hence it is hand-written here.
;
; Calling convention: dest in __rc2/__rc3, src in __rc4/__rc5, count in A (as
; the compiler passes them for a matching C prototype).
;
; Placed in .c128commoncode.* for the same reachability reason as
; c128_bank1_call.
.section .c128commoncode.__c128bank1_copy_chunk,"ax",@progbits
.globl __c128bank1_copy_chunk
__c128bank1_copy_chunk:
	tax
	php
	lda MMU_CR
	pha
	sei
	lda #MMU_CFG_RAM1_KERNAL
	sta MMU_CR
	ldy #0
.Lc128bank1_copy_loop:
	lda (__rc4),y
	sta (__rc2),y
	iny
	dex
	bne .Lc128bank1_copy_loop
	pla
	sta MMU_CR
	plp
	rts
