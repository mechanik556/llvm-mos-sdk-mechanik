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
;
; method's argument register (__rc2/__rc3 for a single pointer argument,
; confirmed against actual compiler output, not assumed) is forwarded to
; __call_indir's fixed slot (__rc18/__rc19) before switching banks, since
; __rc0-31 are zero page and therefore Common RAM - reachable regardless
; of which bank is currently mapped.
.section .text.c128_bank1_call,"ax",@progbits
.globl c128_bank1_call
c128_bank1_call:
	ldx __rc2
	stx __rc18
	ldx __rc3
	stx __rc19
	lda MMU_CR
	pha
	sei
	lda #MMU_CFG_RAM1_KERNAL
	sta MMU_CR
	jsr __call_indir
	pla
	sta MMU_CR
	cli
	rts

.bss
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
; Shared-Hi's enable bit is preserved as-is (read-modify-write, not a
; blind overwrite) rather than assumed disabled, in case something else
; already configured it before this runs.
.section .init.011,"ax",@progbits
	lda RCR
	sta __rcrsave
	and #%00001000
	ora #RCR_SHARED_LO_4K
	sta RCR

.section .fini.989,"ax",@progbits
	lda __rcrsave
	sta RCR
