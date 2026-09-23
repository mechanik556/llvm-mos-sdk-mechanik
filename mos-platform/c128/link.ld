/* C128 PRG file linker script.
 *
 * BASIC memory regions are freely overwritten, but any KERNAL regions are left alone.
 *
 * Produces a PRG file with a SYS command to start the program.
 */

__basic_zp_start = 0x000A;
__basic_zp_end = 0x0090;

MEMORY {
    ram (rw) : ORIGIN = 0x1C01, LENGTH = 0xA3FF
    /* RAM bank 1, for MOS_C128_BANK1_CODE/_DATA-placed content (bank1.h).
     * Starts right after the 4K Common-RAM-Lo window bank1.s configures
     * (bank1's own $0000-$0FFF is unavailable - it's force-mirrored to
     * bank 0's physical RAM regardless of CR, same as bank 0's own view
     * of that range), and ends where KERNAL ROM/I-O take over at $C000
     * under MMU_CFG_RAM1_KERNAL (c128.inc) - unlike bank 0's `ram`
     * region, no BASIC-compatibility start offset is needed here, since
     * this region is never BASIC-LOADed directly; it's populated by
     * bank1-load.c's own copy mechanism at program startup instead. */
    bank1 (rw) : ORIGIN = 0x1000, LENGTH = 0xB000
    /* Common-RAM-resident code: anything that executes the actual MMU_CR
     * bank switch (c128_bank1_call, __c128bank1_copy_chunk, bank1.s) must
     * live here, not in ordinary `ram` - once CR selects bank 1, the next
     * instruction fetch has to land on the same physical bytes regardless
     * of which bank is mapped, which is only guaranteed within the 4K
     * Common-RAM-Lo window bank1.s configures ($0000-$0FFF). The first
     * implementation forgot this and hung mid-switch (found via VICE).
     *
     * $0C00-$0DFF is the RS-232 input/output buffer pair - per Compute's
     * Mapping the Commodore 128 (ch. on free memory, "probably the best
     * area for short ML routines"), free unless the program uses RS-232,
     * and unlike the cassette buffer ($0B00) it survives a reset. Other
     * candidates in the window were rejected: $0000-$03FF KERNAL/BASIC
     * workspace, $0400-$07E7 text screen, $0800-$09FF BASIC runtime stack,
     * $0A00-$0AFF system vectors/editor variables, $0B00 disk-boot/tape
     * buffer, $0E00-$0FFF default sprite definitions. Programs that open
     * RS-232 must not use bank-1 placement (or relocate this region). */
    common_code (rw) : ORIGIN = 0x0C00, LENGTH = 0x0200
}

INCLUDE commodore.ld

INPUT(init-mmu.o)

/* MOS_C128_BANK1_CODE/_DATA content (bank1.h): placed in bank1 (VMA) but
 * loaded into ram alongside everything else (LMA), since ordinary PRG
 * loading only ever populates one bank - bank1-load.c copies it into
 * bank1 at startup. See work/M0_C128_BANKING_PLAN.md section 4.1 task 4
 * in llvm-mos-mechanik for the full reasoning. */
SECTIONS {
    .c128bank1.text : { *(.c128bank1.text) } >bank1 AT>ram
    .c128bank1.data : { *(.c128bank1.data) } >bank1 AT>ram
    .c128bank1.bss (NOLOAD) : { *(.c128bank1.bss) } >bank1
    .c128commoncode : { *(.c128commoncode.*) } >common_code AT>ram
}

__c128commoncode_vma_start = ADDR(.c128commoncode);
__c128commoncode_lma_start = LOADADDR(.c128commoncode);
__c128commoncode_size = SIZEOF(.c128commoncode);

__c128bank1_text_vma_start = ADDR(.c128bank1.text);
__c128bank1_text_lma_start = LOADADDR(.c128bank1.text);
__c128bank1_text_size = SIZEOF(.c128bank1.text);

__c128bank1_data_vma_start = ADDR(.c128bank1.data);
__c128bank1_data_lma_start = LOADADDR(.c128bank1.data);
__c128bank1_data_size = SIZEOF(.c128bank1.data);

__c128bank1_bss_vma_start = ADDR(.c128bank1.bss);
__c128bank1_bss_size = SIZEOF(.c128bank1.bss);

__stack = 0xC000;

OUTPUT_FORMAT {
    /* Tells the C128 LOAD command where to place the file's contents. */
    SHORT(ORIGIN(ram))
    TRIM(ram)
}
