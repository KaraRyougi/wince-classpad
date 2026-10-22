OUTPUT_FORMAT("elf32-sh", "elf32-sh", "elf32-shl")
OUTPUT_ARCH(sh)
ENTRY(_start)
SECTIONS
{
    . = 0x00300000;
    .text : { *(.text .text.*) }
    . = ALIGN(32);
    .rodata : { *(.rodata .rodata.*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.data*) *(.bss*) }
}
