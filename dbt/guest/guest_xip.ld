/* guest_xip.ld - a test image that executes in place from the emulated NOR:
 * code and rodata at the flash address the translator scans (dbt/rt/main.c
 * NK_FLASH), data and bss in the guest RAM window. objcopy -O binary emits
 * the flash part (text, rodata, the data initialisers at their load
 * address) as one flat image from 0x81000000; the start code copies .data
 * to RAM, as the CE kernel's copy entries do for a ROMIMAGE. */
OUTPUT_FORMAT("elf32-shl", "elf32-sh", "elf32-shl")
OUTPUT_ARCH(sh)
ENTRY(_start)
MEMORY
{
    rom : ORIGIN = 0x81000000, LENGTH = 1M
    ram : ORIGIN = 0x8C100000, LENGTH = 1M
}
SECTIONS
{
    .text : { KEEP(*(.text._start)) *(.text .text.*) } > rom
    .rodata : ALIGN(16) { *(.rodata .rodata.*) } > rom
    .data : ALIGN(16) { __data_start = .; *(.data .data.*) . = ALIGN(4); __data_end = .; } > ram AT> rom
    __data_lma = LOADADDR(.data);
    __rom_end = LOADADDR(.data) + SIZEOF(.data);
    .bss : ALIGN(16) { __bss_start = .; *(.bss .bss.*) *(COMMON) . = ALIGN(4); __bss_end = .; } > ram
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
