/* Translator image: flat binary at IMAGE_BASE (P1 RAM). Loaded by the launcher. */
OUTPUT_FORMAT("elf32-sh", "elf32-sh", "elf32-shl")
OUTPUT_ARCH(sh)
ENTRY(_start)
IMAGE_BASE = 0x8CB00000;
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
_image_stack_top = 0x8CDF0000;
