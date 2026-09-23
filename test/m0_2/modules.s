; M0.2.1 test modules (hand-authored) and Module Table.
;   module 0: bank 0    fa_add1(A) = A+1
;   module 1: bank 1    fb_double(A): b1val=A, return 2A
;                       fb_get(): return b1val (initial $55, via load-time copy)
;                       fb_cb(A): calls module 0 fa_add1 (bank1 -> bank0), +100
;                       fb_chain(A): calls module 2 fc_inc (bank1 -> bank1)
;   module 2: bank 1    fc_inc(A) = A+1

.include "c128.inc"

.macro CALL id, off
	jsr call_gate
	.byte \id
	.word \off
.endm

.section .text.modA,"ax",@progbits
modA_start:
fa_add1:
	clc
	adc #1
	rts

.section .c128bank1.text,"ax",@progbits
modB_start:
fb_double:
	sta b1val
	asl
	rts
fb_get:
	lda b1val
	rts
fb_cb:
	CALL 0, fa_add1 - modA_start
	clc
	adc #100
	rts
fb_chain:
	CALL 2, fc_inc - modC_start
	rts
fb_sum:                          ; A + X, args passed through the gate
	sta b1tmp
	txa
	clc
	adc b1tmp
	rts
fb_sec:                          ; return with carry set
	sec
	lda #0
	rts
fb_cin:                          ; return the carry-in as 0/1
	lda #0
	rol
	rts
modC_start:
fc_inc:
	clc
	adc #1
	rts

.section .c128bank1.data,"aw",@progbits
b1val:
	.byte $55
b1tmp:
	.byte 0

; Module Table (struct of arrays, ordinary bank-0 RAM).
.data
.globl mt_addr, mt_cr
mt_addr: .word modA_start, modB_start, modC_start
mt_cr:  .byte MMU_CFG_RAM0_KERNAL, MMU_CFG_RAM1_KERNAL, MMU_CFG_RAM1_KERNAL
.bss
.globl mt_active
mt_active: .fill 3

; C-callable stubs for the bank-0 test driver.
.section .text.stubs,"ax",@progbits
.globl m_double, m_get, m_cb, m_chain, m_sum, t_carry, t_cin, t_iflag, t_inest
m_double:
	CALL 1, fb_double - modB_start
	rts
m_get:
	CALL 1, fb_get - modB_start
	rts
m_cb:
	CALL 1, fb_cb - modB_start
	rts
m_chain:
	CALL 1, fb_chain - modB_start
	rts
m_sum:                           ; C: (A, X) -> A
	CALL 1, fb_sum - modB_start
	rts
t_carry:                         ; returns 1 if callee's carry-out reaches us
	CALL 1, fb_sec - modB_start
	bcs 1f
	lda #0
	rts
1:	lda #1
	rts
t_cin:                           ; returns carry seen by callee (expect 0)
	sec
	CALL 1, fb_cin - modB_start
	rts
t_iflag:                         ; caller has I set: must still be set (expect 4)
	sei
	CALL 1, fb_get - modB_start
	php
	pla
	and #$04
	cli
	rts
t_inest:                         ; same, through a nested bank1->bank0 call
	sei
	lda #1
	CALL 1, fb_cb - modB_start
	php
	pla
	and #$04
	cli
	rts

; Reference c128_bank1_call so bank1.o (Common-RAM setup, bank-1 load-time
; copy) is linked; `bit` just reads the address and has no effect.
.section .init.202,"ax",@progbits
	bit c128_bank1_call
