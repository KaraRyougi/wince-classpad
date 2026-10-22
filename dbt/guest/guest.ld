OUTPUT_FORMAT("elf32-sh-linux", "elf32-shbig-linux", "elf32-sh-linux")
OUTPUT_ARCH(sh)
ENTRY(_start)
SECTIONS
{
    . = 0x8C100000;
    .text : { KEEP(*(.text._start)) *(.text .text.*) }
    . = ALIGN(16);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(16);
    .data : { *(.data .data.*) }
    . = ALIGN(16);
    .bss : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
