; M0.2.2 test modules (hand-authored, position-independent: only relative
; branches, gate calls and fixed zero-page addresses - no absolute references
; to their own labels - so no relocation is needed yet; that is M0.2.3).
; Canonical images live in the program image (.rodata, bank 0); the loader
; copies them into a pool.
;   module 0 A (1 unit):  fa_add1(A) = A+1
;   module 1 B (3 units): fb_double, fb_cb (calls A, +100), fb_chain (calls C),
;                         fb_sum(A,X)=A+X, fb_sec, fb_cin
;   module 2 C (2 units): fc_inc(A)=A+1, fc_calld(A) (calls D)
;   module 3 D (1 unit):  fd_x2(A)=2A
;   module 4 E:           declared far too large to load (failure path)

.include "c128.inc"
.include "imag.inc"

.macro CALL id, off
	jsr call_gate
	.byte \id
	.word \off
.endm

; Each image is followed by at least its declared size in filler bytes, so
; copying the declared size (mt_size, a whole number of 32-byte allocation
; units) always stays inside the image section. (Label differences are not
; absolute expressions at assembly time, so sizes are declared by hand.)
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
fb_double:
	asl
	rts
fb_cb:
	CALL 0, fa_add1 - modA_start
	clc
	adc #100
	rts
fb_chain:
	CALL 2, fc_inc - modC_start
	rts
fb_sum:                          ; A + X (args passed through the gate)
	sta __rc2
	txa
	clc
	adc __rc2
	rts
fb_sec:                          ; return with carry set
	sec
	lda #0
	rts
fb_cin:                          ; return the carry-in as 0/1
	lda #0
	rol
	rts

	PAD 3

	.section .rodata.modC,"a",@progbits
modC_start:
fc_inc:
	clc
	adc #1
	rts
fc_calld:
	CALL 3, fd_x2 - modD_start
	rts

	PAD 2

	.section .rodata.modD,"a",@progbits
modD_start:
fd_x2:
	asl
	rts

	PAD 1

; Image address / size tables (bank-0 ROM-like data). Module 4's size is
; deliberately larger than both pools; its image is never copied.
	.section .rodata.mt,"a",@progbits
.globl mt_img, mt_size
mt_img:  .word modA_start, modB_start, modC_start, modD_start, modA_start
mt_size: .word 32, 96, 64, 32, 4000

; C-callable stubs for the bank-0 test driver (m_* take A / A,X, return A).
	.section .text.stubs,"ax",@progbits
.globl m_double, m_cb, m_chain, m_sum, m_calld, t_carry, t_cin, t_iflag, t_inest, t_fail
m_double:
	CALL 1, fb_double - modB_start
	rts
m_cb:
	CALL 1, fb_cb - modB_start
	rts
m_chain:
	CALL 1, fb_chain - modB_start
	rts
m_sum:
	CALL 1, fb_sum - modB_start
	rts
m_calld:
	CALL 2, fc_calld - modC_start
	rts
t_carry:                         ; 1 if the callee's carry-out reaches us
	CALL 1, fb_sec - modB_start
	bcs 1f
	lda #0
	rts
1:	lda #1
	rts
t_cin:                           ; carry seen by the callee (expect 0)
	sec
	CALL 1, fb_cin - modB_start
	rts
t_iflag:                         ; caller has I set: must still be set (4)
	sei
	CALL 1, fb_double - modB_start
	php
	pla
	and #$04
	cli
	rts
t_inest:                         ; same, through a nested bank crossing
	sei
	lda #1
	CALL 1, fb_cb - modB_start
	php
	pla
	and #$04
	cli
	rts
t_fail:                          ; call module 4 (cannot load): expect carry
	ldx #7                       ; set, A=1 (out of memory), X/Y untouched
	ldy #9
	lda #$55
	CALL 4, 0
	bcc 1f
	cpx #7
	bne 1f
	cpy #9
	bne 1f
	rts                          ; A = error code (1)
1:	lda #$EE
	rts

; Reference c128_bank1_call so bank1.o (Common-RAM setup, bank-1 load-time
; copy) is linked; bit just reads the address and has no effect.
	.section .init.202,"ax",@progbits
	bit c128_bank1_call
