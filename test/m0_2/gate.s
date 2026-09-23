; M0.2.1 prototype: call_gate / exit_gate with a Module Table, bank-aware.
; See work/M0_C128_BANKING_PLAN.md (llvm-mos-mechanik) sections 4.2 and 5.
;
; A cross-module call site is:   jsr call_gate
;                                .byte module_id
;                                .word offset_in_module
; call_gate dispatches to module_id's resident address + offset, switching
; RAM bank if needed, and plants a frame so the callee's RTS lands in
; exit_gate, which does the bookkeeping and returns to the call site.
;
; Stack frame planted per call, top down:
;   exit_gate-1 (2), caller's $FF00 (1), caller's I flag (1), resume-1 (2)
;
; All gate code lives in Common RAM (.c128commoncode.*): it runs while a
; callee's bank may be mapped. It normalizes to bank 0 before touching the
; Module Table and active-module stack, which are ordinary bank-0 RAM.
; Call-site operands are read BEFORE normalizing, since a call site in
; bank-1 code is only visible under the caller's own mapping.
;
; Registers: A/X/Y carry callee arguments and are preserved through
; dispatch; carry is cleared on dispatch. On return, A/X/Y and all flags
; except I come from the callee; I is restored to the caller's value.

.include "c128.inc"
.include "imag.inc"

AMS_MAX = 16

; Zero-page pool is small (~100 bytes shared with the compiler), so gate
; temporaries that are never live at the same time share bytes.
.zeropage gt_a, gt_x, gt_y, gt_i, gt_cr, gt_mod, gt_off, gt_ptr, gt_tgt, ams_top
.globl ams_top
.section .zp.bss,"aw",@nobits
gt_a:   .fill 1
gt_x:   .fill 1
gt_y:   .fill 1
gt_i:   .fill 1
gt_cr:  .fill 1
gt_mod: .fill 1
gt_off: .fill 2
gt_ptr: .fill 2
gt_tgt: .fill 2
ams_top: .fill 1
gt_tcr = gt_ptr        ; free once the resume frame is pushed
ex_a = gt_a            ; exit_gate runs with no dispatch in progress
ex_x = gt_x
ex_y = gt_y
ex_p = gt_mod

; Active-module-ID stack (ordinary bank-0 RAM; touched only when normalized).
.bss
ams:    .fill AMS_MAX

.section .c128commoncode.gate,"ax",@progbits
.globl call_gate
call_gate:
	sta gt_a
	stx gt_x
	sty gt_y
	php
	pla
	and #$04
	sta gt_i
	sei
	pla
	sta gt_ptr
	pla
	sta gt_ptr+1
	ldy #1
	lda (gt_ptr),y
	sta gt_mod
	iny
	lda (gt_ptr),y
	sta gt_off
	iny
	lda (gt_ptr),y
	sta gt_off+1
	lda MMU_CR
	sta gt_cr
	lda #MMU_CFG_RAM0_KERNAL
	sta MMU_CR
	clc
	lda gt_ptr
	adc #3
	sta gt_ptr
	bcc .Lnc
	inc gt_ptr+1
.Lnc:
	lda gt_ptr+1
	pha
	lda gt_ptr
	pha
	lda gt_i
	pha
	lda gt_cr
	pha
	lda #mos16hi(exit_gate-1)
	pha
	lda #mos16lo(exit_gate-1)
	pha
	ldx gt_mod
	txa
	asl
	tay                  ; Y = 2*module_id (mt_addr entries are 16-bit)
	lda mt_addr+1,y
	beq .Lfatal          ; high byte 0 = not resident
	inc mt_active,x
	clc
	lda mt_addr,y
	adc gt_off
	sta gt_tgt
	lda mt_addr+1,y
	adc gt_off+1
	sta gt_tgt+1
	lda mt_cr,x
	sta gt_tcr
	ldy ams_top
	cpy #AMS_MAX
	bcs .Lfatal
	txa
	sta ams,y
	inc ams_top
	ldx gt_x
	ldy gt_y
	lda gt_a
	pha
	lda gt_tcr
	sta MMU_CR
	pla
	clc
	jmp (gt_tgt)
.Lfatal:
	jmp .Lfatal          ; M0.2.1: not-resident / overflow not yet handled

exit_gate:
	sta ex_a
	stx ex_x
	sty ex_y
	php
	pla
	sta ex_p
	lda #MMU_CFG_RAM0_KERNAL
	sta MMU_CR
	dec ams_top
	ldy ams_top
	ldx ams,y
	dec mt_active,x
	pla
	sta gt_cr
	pla
	sta gt_i
	lda ex_p
	and #$FB
	ora gt_i
	sta ex_p
	lda gt_cr
	sta MMU_CR
	lda ex_p
	pha
	ldx ex_x
	ldy ex_y
	lda ex_a
	plp
	rts
