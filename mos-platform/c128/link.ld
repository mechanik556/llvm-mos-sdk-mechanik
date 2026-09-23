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
     * Common-RAM-Lo window bank1.s configures ($0000-$0FFF). Found via
     * VICE testing (work/M0_C128_BANKING_PLAN.md section 4.1 task 4/6 in
     * llvm-mos-mechanik), not assumed - the first implementation forgot
     * this and hung mid-switch, fetching garbage from unpopulated bank-1
     * memory. $0800-$0FFF avoids $0000-$03FF (BASIC/KERNAL's own zero
     * page/workspace, confirmably not free even in bank 1 - see
     * research/c128_mmu_reference.md's FRE(1)=$0400-$FEFF finding) and
     * the conventional $0400-$07E7 screen memory range, at the cost of
     * competing with the low end of bank1's own budget for those 2K -
     * acceptable given how little of it this needs (under 100 bytes
     * today). */
    common_code (rw) : ORIGIN = 0x0800, LENGTH = 0x0800
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
