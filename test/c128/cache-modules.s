; Hand-written relocatable modules for cache-modules.c: the module table in
; the format cache.h documents (ABI version 1), and four modules:
;   0 A (1 unit)  fa_add1(A) = A+1
;   1 B (3 units) fb_cb(A) = A+1+100 (calls A through the gate)
;   2 R (3 units) exercises relocation: r_entry(A) = r_tbl[A]+10, r_viaptr,
;                 r_lohi, and r_call_sm/r_sm_to_b (self-modified jmp operand)
;   3 F (2 units) filler, f_add7(A) = A+7
;   4 G (2 units) uses the host services (cache.h): g_incr(h) locks the
;                 object h, increments its first byte, unlocks it
;   5 host        the runtime's own jump table (mos_cache_set_host)
; Images are position-dependent (assembled at their own address); every
; absolute reference to their own labels is listed in the relocation table.

.include "c128.inc"
.include "imag.inc"

.macro CALL id, off
	jsr __mos_call_gate
	.byte \id
	.word \off
.endm

.macro PAD units
	.fill \units * 32, 1, 0xEA
.endm

	.section .rodata.modA,"a",@progbits
modA_start:
fa_add1:
	clc
	adc #1
	rts
	PAD 1

	.section .rodata.modB,"a",@progbits
modB_start:
fb_cb:
	CALL 0, fa_add1 - modA_start
	clc
	adc #100
	rts
	PAD 3

	.section .rodata.modR,"a",@progbits
modR_start:
r_entry:
	tax
f1:	lda r_tbl,x
f2:	jsr r_helper
	rts
r_helper:
	clc
	adc #10
	rts
r_tbl:	.byte 1, 2, 3, 4
r_ptr:	.word r_helper
r_viaptr:
f3:	jmp (r_ptr)
r_lohi:
	sta __rc4
f4:	lda #mos16lo(r_helper)
	sta __rc2
f5:	lda #mos16hi(r_helper)
	sta __rc3
	lda __rc4
	jmp (__rc2)
r_call_sm:
f6:	jmp r_target_a
r_target_a:
	lda #1
	rts
r_target_b:
	lda #2
	rts
r_try_evict:                     ; ask the host to evict R itself: refused, R is active
	lda #2
	CALL 5, 0
	rts
r_sm_to_b:
f7:	lda #mos16lo(r_target_b)
f8:	sta r_call_sm+1
f9:	lda #mos16hi(r_target_b)
f10:	sta r_call_sm+2
	rts
	PAD 3

	.section .rodata.modG,"a",@progbits
modG_start:
g_incr:                          ; A = handle (low byte), X = 0
	pha
	ldx #0
	CALL 5, 3                ; lock -> pointer in A/X, 0 if refused
	sta __rc2
	stx __rc3
	ora __rc3
	beq g_fail
	ldy #0
	lda (__rc2),y
	clc
	adc #1
	sta (__rc2),y
	pla
	ldx #0
	CALL 5, 6                ; unlock
	lda #0
	clc
	rts
g_fail:
	pla
	sec
	rts
	PAD 2

	.section .rodata.modF,"a",@progbits
modF_start:
f_add7:
	clc
	adc #7
	rts
	PAD 2

; Relocation tables: {1, off16} = a 16-bit value, {2, lo_off16, hi_off16} = a
; value split over a LOW8 and a HIGH8 operand, {0} = end.
	.section .rodata.reloc,"a",@progbits
noreloc:
	.byte 0
modR_reloc:
	.byte 1
	.word f1+1 - modR_start
	.byte 1
	.word f2+1 - modR_start
	.byte 1
	.word r_ptr - modR_start
	.byte 1
	.word f3+1 - modR_start
	.byte 2
	.word f4+1 - modR_start, f5+1 - modR_start
	.byte 1
	.word f6+1 - modR_start
	.byte 2
	.word f7+1 - modR_start, f9+1 - modR_start
	.byte 1
	.word f8+1 - modR_start
	.byte 1
	.word f10+1 - modR_start
	.byte 0

; The module table.
	.section .rodata.mt,"a",@progbits
.globl __mos_mt_count, __mos_mt_img, __mos_mt_size, __mos_mt_reloc
__mos_mt_count: .byte 6
__mos_mt_img:   .word modA_start, modB_start, modR_start, modF_start, modG_start, 0
__mos_mt_size:  .word 32, 96, 96, 64, 64, 0
__mos_mt_reloc: .word noreloc, noreloc, modR_reloc, noreloc, noreloc, noreloc

	.bss
.globl __mos_mt_addr, __mos_mt_cr, __mos_mt_active, __mos_mt_ref, __mos_mt_stamp
__mos_mt_addr:   .fill 12
__mos_mt_cr:     .fill 6
__mos_mt_active: .fill 6
__mos_mt_ref:    .fill 6
__mos_mt_stamp:  .fill 12

; C-callable stubs (arguments and results in A).
	.section .text.stubs,"ax",@progbits
.globl m_add1, m_cb, m_add7, m_r_entry, m_r_viaptr, m_r_lohi, m_r_call_sm, m_r_sm_to_b
.globl m_r_try_evict, m_g_incr, m_no_such_module
m_add1:
	CALL 0, fa_add1 - modA_start
	rts
m_cb:
	CALL 1, fb_cb - modB_start
	rts
m_add7:
	CALL 3, f_add7 - modF_start
	rts
m_r_entry:
	CALL 2, r_entry - modR_start
	rts
m_r_viaptr:
	CALL 2, r_viaptr - modR_start
	rts
m_r_lohi:
	CALL 2, r_lohi - modR_start
	rts
m_r_call_sm:
	CALL 2, r_call_sm - modR_start
	rts
m_r_sm_to_b:
	CALL 2, r_sm_to_b - modR_start
	rts
m_r_try_evict:
	CALL 2, r_try_evict - modR_start
	rts
m_no_such_module:                ; the gate reports failure: carry set, A = 3
	CALL 99, 0
	bcs 1f
	lda #0
	rts
1:	rts
m_g_incr:                        ; A = handle; returns 0, or 1 if the lock was refused
	CALL 4, g_incr - modG_start
	bcs 1f
	lda #0
	rts
1:	lda #1
	rts

; The gate is only linked if referenced.
	.section .init.202,"ax",@progbits
	bit c128_bank1_call
