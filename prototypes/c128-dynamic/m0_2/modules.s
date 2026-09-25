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
fb_rec:                          ; recurse A levels through the gate; returns A
	cmp #0                       ; (carry clear), or the gate's failure (carry
	beq 2f                       ; set, A = code) propagated unchanged
	sec
	sbc #1
	CALL 1, fb_rec - modB_start
	bcs 1f
	clc
	adc #1
1:	rts
2:	lda #0
	clc
	rts
fb_calle:                        ; call module 4 (cannot load) from inside B
	CALL 4, 0
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
fc_calla:                        ; bank-1 (or wherever C is) -> module A
	CALL 0, fa_add1 - modA_start
	rts

	PAD 2

	.section .rodata.modD,"a",@progbits
modD_start:
fd_x2:
	asl
	rts

	PAD 1

; Module R (id 5): exercises relocation. Every absolute reference to its own
; labels is listed in modR_reloc. Fixup labels f1..f10 sit on the instruction
; whose operand (at label+1) is relocated.
;   r_entry(A)    = r_tbl[A] + 10        (abs,X data ref + abs JSR)
;   r_viaptr(A)   = A + 10               (jmp through an in-module data word)
;   r_lohi(A)     = A + 10               (pointer built from paired lo/hi imms)
;   r_call_sm()   = 1, or 2 after r_sm_to_b() patches its jmp operand at
;                   runtime with the CURRENT address of r_target_b
;   r_try_evict() = host_try_evict(5): must be refused (module is active)
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
r_sm_to_b:
f7:	lda #mos16lo(r_target_b)
f8:	sta r_call_sm+1
f9:	lda #mos16hi(r_target_b)
f10:	sta r_call_sm+2
	rts
r_try_evict:
	lda #5
	CALL 6, 0
	rts
r_try_defrag:                    ; ask the host to defragment while R is active (pinned)
	CALL 6, 9
	rts
r_try_big:                       ; call H (30 units) while R is pinned
	lda #21
	CALL 8, h_double - modH_start
	rts

	PAD 3

; Module F (id 7, 2 units): f_add7(A) = A+7.
	.section .rodata.modF,"a",@progbits
modF_start:
f_add7:
	clc
	adc #7
	rts

	PAD 2

; Module H (id 8, 30 units): fills nearly all of bank 1's pool, so loading it
; forces automatic eviction of the modules resident there.
	.section .rodata.modH,"a",@progbits
modH_start:
h_double:
	asl
	rts

	PAD 30

; Module G (id 9, 4 units): runs in whatever bank it lands in and uses the host
; module (6) to lock/unlock cacheable heap objects (handle in A, X=0).
; Host jump table offsets: 0 = evict, 3 = lock (-> ptr in A/X, 0 if refused),
; 6 = unlock. Both functions return with carry set on failure, clear on
; success, and use only zero-page scratch __rc2-__rc4 between gate calls.
;   g_incr(h): ++first byte of the object; A=0
;   g_sum(h):  A = low byte of the sum of the object's first 70 bytes
	.section .rodata.modG,"a",@progbits
modG_start:
g_incr:
	pha
	ldx #0
	CALL 6, 3
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
	CALL 6, 6
	lda #0
	clc
	rts
g_sum:
	pha
	ldx #0
	CALL 6, 3
	sta __rc2
	stx __rc3
	ora __rc3
	beq g_fail
	ldy #0
	lda #0
	sta __rc4
g_sum_loop:
	lda __rc4
	clc
	adc (__rc2),y
	sta __rc4
	iny
	cpy #70
	bne g_sum_loop
	lda __rc4
	pha
	tsx
	lda $0102,x
	ldx #0
	CALL 6, 6
	pla
	tax
	pla
	txa
	clc
	rts
g_fail:
	pla
	sec
	rts

	PAD 4

; Relocation tables: {1, off16}=FULL16, {2, lo_off16, hi_off16}=paired LOW8/
; HIGH8, {0}=end. Offsets are relative to the module's image start.
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
; Offsets the test driver checks: [0] = jmp operand of r_call_sm, [1] =
; r_target_a, [2] = r_target_b.
.globl r_info
r_info:
	.word f6+1 - modR_start, r_target_a - modR_start, r_target_b - modR_start

; Image address / size / relocation-table tables (bank-0 read-only data).
; Module 4's size is deliberately larger than both pools; its image is never
; copied. Module 6 is the static host (no image).
	.section .rodata.mt,"a",@progbits
.globl mt_img, mt_size, mt_reloc
mt_img:   .word modA_start, modB_start, modC_start, modD_start, modA_start
          .word modR_start, 0, modF_start, modH_start, modG_start
mt_size:  .word 32, 96, 64, 32, 4000, 96, 0, 64, 960, 128
mt_reloc: .word noreloc, noreloc, noreloc, noreloc, noreloc
          .word modR_reloc, noreloc, noreloc, noreloc, noreloc

; C-callable stubs for the bank-0 test driver (m_* take A / A,X, return A).
	.section .text.stubs,"ax",@progbits
.globl m_double, m_cb, m_chain, m_sum, m_calld, t_carry, t_cin, t_iflag, t_inest, t_fail
.globl m_r_entry, m_r_viaptr, m_r_lohi, m_r_call_sm, m_r_sm_to_b, m_r_try_evict
.globl m_f_add7, m_h_double, m_g_incr, m_g_sum, host_tab
.globl m_add1, m_rec, m_calle, m_r_try_big, m_r_try_defrag, m_c_inc, m_c_calla, m_base
m_base:                          ; no-op stub for measuring loop/call overhead
	rts
m_c_inc:                         ; gate call into module C (bank 1 when bank 0 is full)
	CALL 2, fc_inc - modC_start
	rts
m_c_calla:
	CALL 2, fc_calla - modC_start
	rts
m_r_try_defrag:
	CALL 5, r_try_defrag - modR_start
	rts
m_add1:
	CALL 0, fa_add1 - modA_start
	rts
m_rec:                           ; C: (depth) -> uint16: lo = A, hi = 1 if refused
	CALL 1, fb_rec - modB_start
	bcs 1f
	ldx #0
	rts
1:	ldx #1
	rts
m_calle:
	CALL 1, fb_calle - modB_start
	bcs 1f
	ldx #0
	rts
1:	ldx #1
	rts
m_r_try_big:
	CALL 5, r_try_big - modR_start
	bcs 1f
	ldx #0
	rts
1:	ldx #1
	rts
; Static host module (id 6) entry table: three 3-byte jumps into C (modtab.c).
host_tab:
	jmp host_try_evict
	jmp host_lock
	jmp host_unlock
	jmp host_defrag
m_g_incr:                        ; C: (handle) -> 0 ok, 1 = lock refused
	CALL 9, g_incr - modG_start
	bcs 1f
	lda #0
	rts
1:	lda #1
	rts
m_g_sum:                         ; C: (handle) -> uint16: lo = sum, hi = 1 if refused
	CALL 9, g_sum - modG_start
	bcs 1f
	ldx #0
	rts
1:	ldx #1
	lda #0
	rts
m_r_entry:
	CALL 5, r_entry - modR_start
	rts
m_r_viaptr:
	CALL 5, r_viaptr - modR_start
	rts
m_r_lohi:
	CALL 5, r_lohi - modR_start
	rts
m_r_call_sm:
	CALL 5, r_call_sm - modR_start
	rts
m_r_sm_to_b:
	CALL 5, r_sm_to_b - modR_start
	rts
m_r_try_evict:
	CALL 5, r_try_evict - modR_start
	rts
m_f_add7:
	CALL 7, f_add7 - modF_start
	rts
m_h_double:
	CALL 8, h_double - modH_start
	rts
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
