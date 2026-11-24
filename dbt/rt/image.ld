/* Translator image: flat binary at IMAGE_BASE (P1 RAM), loaded by the
 * launcher. IMAGE_BASE and _image_stack_top come from the Makefile
 * (--defsym), which owns the RAM layout. */
OUTPUT_FORMAT("elf32-sh", "elf32-sh", "elf32-shl")
OUTPUT_ARCH(sh)
ENTRY(_start)
SECTIONS
{
    . = IMAGE_BASE;
    .text : { *(.text .text.*) }
    . = ALIGN(16);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(16);
    .data : { *(.data .data.*) }
    . = ALIGN(16);
    /* On-chip memories, as gint's fxcp_hh3.ld and gpsp-sh4-jit's
     * fxcg100_gpsp.ld use them: the 4 KiB IL memory (zero-wait instruction
     * fetch) runs the stubs and the exception vectors (VBR = 0xE5200000),
     * the 16 KiB XRAM+YRAM pair (0xE500E000..0xE5011FFF) holds small hot
     * data. The stubs are stored in the image (LMA) and copied by crt0;
     * both memories lose their contents in R-standby (rt.c dbt_standby). */
    _lilram = .;
    . = . + SIZEOF(.ilram);         /* the stubs' load image; .ilram itself is placed last */
    . = ALIGN(16);
    _bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    _bss_end = .;
    _image_end = .;
    /* No file bytes: initialized only after the launcher has handed over
     * and crt0 has abandoned its stack. Empty since 2026-11-06, when the
     * first MiB became CE RAM (the ASSERT below keeps it that way). */
    .lowbss 0x8C000000 (NOLOAD) : {
        _low_bss_start = .;
        *(.lowbss)
        . = ALIGN(16);
        _low_bss_end = .;
    }
    .xyram 0xE500E000 (NOLOAD) : {
        _rxyram = .;
        *(.xyram .xyram.*)
        . = ALIGN(4);
        _exyram = .;
    }
    .ilram 0xE5200000 : AT(_lilram) {
        _rilram = .;
        *(.ilram .ilram.*)
        . = ALIGN(4);
        _eilram = .;
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(SIZEOF(.ilram) <= 4096, "stubs exceed the 4 KiB IL memory")
ASSERT(SIZEOF(.xyram) <= 16384, "hot data exceeds the 16 KiB XRAM+YRAM pair")
/* Runtime C stack reuses the top of the image region (boot stack is dead after dbt_enter). */
PROVIDE(_dbt_stack_rt_top = _image_stack_top);
/* leave at least 24 KiB between the end of bss and the C stack top */
ASSERT(_image_end + 0x6000 <= _image_stack_top, "translator image too large for its RAM region (see dbt/Makefile layout)")
ASSERT(_low_bss_end <= GUEST_RAM_LO, "boot workspace tables overlap CE RAM (the first MiB is CE's since 2026-11-06)")
