/* hhk-emu.ld - the same stub as a raw casio-emu payload (.g3a) at 0x00300000:
 * exercises hhk_takeover (quiesce, copy, cache sync, jump) in the emulator.
 * The add-in window is read-only data to us, so .data must be empty; .bss goes
 * to the launcher workspace above the emulator's initial stack (0x8C080000). */
OUTPUT_FORMAT("elf32-sh", "elf32-sh", "elf32-shl")
OUTPUT_ARCH(sh)
ENTRY(_start)
SECTIONS
{
    . = 0x00300000;
    .text : { KEEP(*(.text.start)) *(.text .text.*) }
    . = ALIGN(4);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(32);
    .wince.payload : { KEEP(*(.wince.payload)) }
    . = ALIGN(4);
    .data : { *(.data .data.*) }
    _bss_start = 0x8C080000;
    .bss 0x8C080000 (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
    _bss_end = ALIGN(_bss_start + SIZEOF(.bss), 4);
    /DISCARD/ : { *(.comment) *(.note*) *(.hollyhock_*) *(.eh_frame*) }
}
ASSERT(SIZEOF(.data) == 0, "the emulator variant is a raw image: keep writable data in .bss")
