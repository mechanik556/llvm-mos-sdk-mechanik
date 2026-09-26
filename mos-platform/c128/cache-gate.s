; Copyright 2026 LLVM-MOS Project
; Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
; See https://github.com/llvm-mos/llvm-mos-sdk/blob/main/LICENSE for license
; information.

; Bank-aware call gate for relocatable code modules (cache.h), with a
; load-on-miss path. Linked only by programs that reference __mos_call_gate.
;
; A cross-module call site is:   jsr __mos_call_gate
;                                .byte module_id
;                                .word offset_in_module
; The gate dispatches to module_id's resident address + offset, loading the
; module first if it is not resident (mos_cache_module_load, cache.c),
; switching RAM bank if needed, and plants a frame so the callee's RTS lands in
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
; Failure (module cannot be loaded, active-module stack full, no such module):
; nothing is called; returns to the call site with carry SET, A = error code
; (1 = out of memory, 2 = nesting too deep, 3 = no such module), X/Y as
; passed, I restored.

.include "c128.inc"
.include "imag.inc"

AMS_MAX = 16

; Zero-page pool is small (~100 bytes shared with the compiler), so gate
; temporaries that are never live at the same time share bytes.
.zeropage gt_a, gt_x, gt_y, gt_i, __mos_gate_cr, gt_mod, gt_off, gt_ptr
.zeropage gt_tgt, __mos_gate_ams_top
.globl __mos_gate_ams_top, __mos_gate_cr
.section .zp.bss,"aw",@nobits
gt_a:   .fill 1
gt_x:   .fill 1
gt_y:   .fill 1
gt_i:   .fill 1
__mos_gate_cr:  .fill 1
gt_mod: .fill 1
gt_off: .fill 2
gt_ptr: .fill 2
gt_tgt: .fill 2
__mos_gate_ams_top: .fill 1
gt_tcr = gt_ptr        ; free once the resume frame is pushed
ex_a = gt_a            ; exit_gate runs with no dispatch in progress
ex_x = gt_x
ex_y = gt_y
ex_p = gt_mod

; Active-module-ID stack and the C-ABI argument registers saved around the
; C loader (ordinary bank-0 RAM; touched only when normalized).
.bss
ams:     .fill AMS_MAX
rc_save: .fill 18

.section .c128commoncode.gate,"ax",@progbits
.globl __mos_call_gate
__mos_call_gate:
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
	sta __mos_gate_cr
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
	pha                  ; resume-1 is now on the stack
	ldx gt_mod
	cpx __mos_mt_count   ; a module the table does not have?
	bcc .Lknown
	lda #3
	bne .Lfail
.Lknown:
	txa
	asl
	tay                  ; Y = 2*module_id (__mos_mt_addr entries are 16-bit)
	lda __mos_mt_addr+1,y
	bne .Lres
	jsr load_module      ; A = 0 ok, else error code
	bne .Lfail
	ldx gt_mod
	txa
	asl
	tay
.Lres:
	lda #1
	sta __mos_mt_ref,x         ; CLOCK reference bit: used since the eviction sweep
	lda __mos_gate_ams_top
	cmp #AMS_MAX
	bcc .Lroom
	lda #2
	bne .Lfail
.Lroom:
	inc __mos_mt_active,x
	clc
	lda __mos_mt_addr,y
	adc gt_off
	sta gt_tgt
	lda __mos_mt_addr+1,y
	adc gt_off+1
	sta gt_tgt+1
	lda __mos_mt_cr,x
	sta gt_tcr
	ldy __mos_gate_ams_top
	txa
	sta ams,y
	inc __mos_gate_ams_top
	lda gt_i
	pha
	lda __mos_gate_cr
	pha
	lda #mos16hi(exit_gate-1)
	pha
	lda #mos16lo(exit_gate-1)
	pha
	ldx gt_x
	ldy gt_y
	lda gt_a
	pha
	lda gt_tcr
	sta MMU_CR
	pla
	clc
	jmp (gt_tgt)
.Lfail:                  ; A = error code; only resume-1 is on the stack
	sta gt_a
	lda __mos_gate_cr
	sta MMU_CR
	lda gt_i
	ora #$01             ; carry set, I as the caller had it
	pha
	ldx gt_x
	ldy gt_y
	lda gt_a
	plp
	rts

exit_gate:
	sta ex_a
	stx ex_x
	sty ex_y
	php
	pla
	sta ex_p
	lda #MMU_CFG_RAM0_KERNAL
	sta MMU_CR
	dec __mos_gate_ams_top
	ldy __mos_gate_ams_top
	ldx ams,y
	dec __mos_mt_active,x
	pla
	sta __mos_gate_cr
	pla
	sta gt_i
	lda ex_p
	and #$FB
	ora gt_i
	sta ex_p
	lda __mos_gate_cr
	sta MMU_CR
	lda ex_p
	pha
	ldx ex_x
	ldy ex_y
	lda ex_a
	plp
	rts

; Call the C loader with the C-ABI argument registers (__rc2-__rc19) saved
; and restored: they may hold the callee's arguments. Runs normalized to
; bank 0, so ordinary .text is reachable.
.text
load_module:
	ldx #0
1:	lda __rc2,x
	sta rc_save,x
	inx
	cpx #18
	bne 1b
	lda __mos_gate_cr            ; caller's bank: which one to prefer
	and #$40
	beq 3f
	ldx #1
	bne 4f
3:	ldx #0
4:	lda gt_mod
	jsr mos_cache_module_load
	pha
	ldx #0
2:	lda rc_save,x
	sta __rc2,x
	inx
	cpx #18
	bne 2b
	pla
	rts
