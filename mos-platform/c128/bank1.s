.include "c128.inc"
.include "imag.inc"

; c128_bank1_call: call a function that lives in C128 RAM bank 1 (with
; KERNAL ROM/I-O still mapped in, per MMU_CFG_RAM1_KERNAL) from code
; currently executing in bank 0, then return to bank 0.
;
; Modeled directly on mos-platform/nes-mmc3/mapper.s's banked_call_8000,
; the existing precedent in this codebase for "call a function in a
; currently-unmapped bank" - see work/M0_C128_BANKING_PLAN.md section 4.1
; task 2 in llvm-mos-mechanik for the full design this implements.
;
; Interrupts are disabled for the duration of the switched-away call
; (design requirement, not just this implementation's choice - a bank
; switch changes what an interrupt handler would see at $4000-$BFFF too).
; The caller's I flag is saved and restored (PHP/PLP, not SEI/CLI) so the
; routine is also safe to call with interrupts already disabled, e.g. from
; inside call_gate.
;
; method's argument register (__rc2/__rc3 for a single pointer argument,
; confirmed against actual compiler output, not assumed) is forwarded to
; __call_indir's fixed slot (__rc18/__rc19) before switching banks, since
; __rc0-31 are zero page and therefore Common RAM - reachable regardless
; of which bank is currently mapped.
;
; Placed in .c128commoncode.*, not ordinary .text: the moment MMU_CR
; selects bank 1, the CPU's next instruction fetch has to land on the
; same physical bytes it would on bank 0, which only holds within the
; Common-RAM-Lo window (link.ld's `common_code` region) - found the hard
; way, via VICE testing, not designed in from the start: an earlier
; version of this file placed this routine in ordinary .text and hung
; mid-switch, fetching whatever garbage happened to be in bank 1's
; not-yet-populated memory at that address. See
; work/M0_C128_BANKING_PLAN.md section 4.1 task 4/6 in llvm-mos-mechanik.
; Does NOT call the shared __call_indir (mos-platform/common/crt/
; call-indir.S) despite doing exactly what it does (jmp (__rc18)) -
; __call_indir lives in ordinary .text, shared by every target, and
; calling it while bank 1 is mapped in would hit the exact same
; reachability problem described above, one level deeper (also found via
; VICE testing, not anticipated). A local copy of its one instruction,
; kept inside this same Common-RAM-resident section, avoids depending on
; where the shared symbol happens to be placed.
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

; Bump Common RAM (RCR's Shared-Lo window) from the stock 1K to 4K, per
; work/M0_C128_BANKING_PLAN.md section 4.1 task 4's budget analysis -
; c128_bank1_call's own zero-page/stack usage (like any bank-switched
; code's) depends on more than 1K being safely reachable regardless of
; which bank is mapped. Only linked in (and only runs) if something in
; the program actually references c128_bank1_call, since this object
; file is an ordinary library member, not force-included like
; init-mmu.o - programs that never use bank 1 pay nothing for this.
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
; from common_ram_src to bank1_dest, switching to RAM bank 1 for the
; duration. common_ram_src must be Common RAM (reachable regardless of
; which bank is mapped, e.g. __c128bank1_scratch in bank1-load.c) - it is
; NOT itself switched to, only bank1_dest's bank is. This is the inner
; primitive of the Common-RAM-staged chunked copy that populates static
; bank-1 content at program load time (see bank1-load.c and
; work/M0_C128_BANKING_PLAN.md section 4.1 task 4 in llvm-mos-mechanik) -
; ordinary PRG loading only populates whichever bank is mapped at load
; time, so this content's initial bytes have to be staged through Common
; RAM and copied in after the fact, one chunk at a time.
;
; Deliberately hand-written, not built from smaller calls: the entire
; copy loop runs inside the bank-1-mapped window, so it cannot call any
; ordinary (non-Common-RAM-resident) subroutine - including memcpy -
; without hitting the exact reachability problem call_gate's own
; Common-RAM entry stubs exist to avoid (see the parent design's section
; 11.6 point 7). Inlining the loop here, rather than trying to reuse an
; existing copy routine, sidesteps that problem entirely.
;
; Calling convention confirmed empirically (compiled a matching C
; function with this project's own mos-clang and inspected the output),
; not assumed: dest in __rc2/__rc3, src in __rc4/__rc5, count in A.
;
; Also placed in .c128commoncode.* - same reachability requirement as
; c128_bank1_call above, and for the same VICE-discovered reason.
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
