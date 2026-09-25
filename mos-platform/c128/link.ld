/* C128 PRG file linker script.
 *
 * BASIC memory regions are freely overwritten, but any KERNAL regions are left alone.
 *
 * Produces a PRG file with a SYS command to start the program.
 */

__basic_zp_start = 0x000A;
__basic_zp_end = 0x0090;

PROVIDE(__c128_common_code_origin = 0x0800);
PROVIDE(__c128_common_code_length = 0x0200);

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
     * bank1-load.c's own copy mechanism at program startup instead.
     *
     * The origin carries a bank tag (+0x10000), the same convention the NES
     * (chr_rom) and GEOS VLIR overlay scripts use: the CPU only sees the low
     * 16 bits, but lld's overlap check works on full VMAs, and without the
     * tag any bank-1 content beyond $1C00 looks like it collides with the
     * bank-0 program that starts at $1C01.
     *
     * This memory is not part of the malloc heap and cannot be: see bank1.h
     * for why bank-1 memory is only reachable through explicit accessors. */
    bank1 (rw) : ORIGIN = 0x10000 + 0x1000, LENGTH = 0xB000
    /* Common-RAM-resident code: anything that executes the actual MMU_CR
     * bank switch (c128_bank1_call, __c128bank1_copy_chunk, bank1.s) must
     * live here, not in ordinary `ram` - once CR selects bank 1, the next
     * instruction fetch has to land on the same physical bytes regardless
     * of which bank is mapped, which is only guaranteed within the 4K
     * Common-RAM-Lo window bank1.s configures ($0000-$0FFF).
     *
     * Default: the low end of BASIC's runtime stack, $0800-$09FF (512
     * bytes). BASIC fills that stack from $09FF downward and only the
     * BASIC interpreter uses it (per Compute's Mapping the Commodore 128),
     * and the interpreter is not running while a bare-metal C program is,
     * so the low end is effectively free. bank1-load.c saves the original
     * bytes at startup and restores them at exit, so even a program started
     * from inside a BASIC FOR/GOSUB comes back intact. Consequence: a
     * program that calls into the BASIC interpreter must pick another area.
     *
     * Override with --defsym (both symbols), e.g. in the driver command:
     *   -Wl,--defsym=__c128_common_code_origin=0x0C00
     *   -Wl,--defsym=__c128_common_code_length=0x0200
     * Other candidates inside the Common-RAM-Lo window:
     *   $0C00-$0DFF  RS-232 buffers - free unless RS-232 is used (or move
     *                RIBUF/ROBUF, $C8-$CB, elsewhere)
     *   $0B00-$0BBF  cassette/disk-boot buffer - free unless tape is used
     *   $0E00-$0FFF  default sprite definitions - free unless sprites used
     *   NOT usable:  $0000-$03FF KERNAL workspace, $0400-$07E7 text screen,
     *                $0A00-$0AFF system vectors/editor variables.
     * The chosen area must lie within $0000-$0FFF (4K Common-RAM-Lo). */
    common_code (rw) : ORIGIN = __c128_common_code_origin, LENGTH = __c128_common_code_length
}

ASSERT(__c128_common_code_origin + __c128_common_code_length <= 0x1000,
       "c128: the Common-RAM code area must lie within $0000-$0FFF (4K Common RAM)")

INCLUDE commodore.ld

INPUT(init-mmu.o)

/* MOS_C128_BANK1_CODE/_DATA content (bank1.h): placed in bank1 (VMA) but
 * loaded into ram alongside everything else (LMA), since ordinary PRG
 * loading only ever populates one bank - bank1-load.c copies it into
 * bank1 at startup. */
SECTIONS {
    .c128bank1.text : { *(.c128bank1.text) } >bank1 AT>ram
    .c128bank1.data : { *(.c128bank1.data) } >bank1 AT>ram
    .c128bank1.bss (NOLOAD) : { *(.c128bank1.bss) } >bank1
    .c128commoncode : { *(.c128commoncode.*) } >common_code AT>ram
    /* Holds the original contents of the area .c128commoncode occupies at
     * runtime, saved at startup and restored at exit (bank1-load.c). */
    .c128commoncode_save (NOLOAD) : { . += SIZEOF(.c128commoncode); } >ram
}

__c128commoncode_vma_start = ADDR(.c128commoncode);
__c128commoncode_lma_start = LOADADDR(.c128commoncode);
__c128commoncode_size = SIZEOF(.c128commoncode);
__c128commoncode_save_start = ADDR(.c128commoncode_save);

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
