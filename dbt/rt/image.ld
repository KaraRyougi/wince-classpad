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
    _bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    _bss_end = .;
    _image_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
/* leave at least 128 KiB between the end of bss and the C stack top */
ASSERT(_image_end + 0x20000 <= _image_stack_top, "translator image too large for its RAM region (see dbt/Makefile layout)")
