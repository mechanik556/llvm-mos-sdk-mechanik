; Copyright 2026 LLVM-MOS Project
; Licensed under the Apache License, Version 2.0 with LLVM Exceptions.
; See https://github.com/llvm-mos/llvm-mos-sdk/blob/main/LICENSE for license
; information.

; The host module's jump table (cache.h, "Host services for modules"): four
; 3-byte jumps into C (cache-host.c), entered through the call gate.
	.section .text.__mos_host_tab,"ax",@progbits
.globl __mos_host_tab
__mos_host_tab:
	jmp __mos_host_evict
	jmp __mos_host_lock
	jmp __mos_host_unlock
	jmp __mos_host_defrag
