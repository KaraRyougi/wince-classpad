/* hhk.ld - HollyHock program (.hhk / .hh3): an ELF whose ALLOC sections the
 * launcher copies to their addresses and whose entry it calls as a subroutine.
 * HHK_BASE/HHK_LIMIT (0x8CFF0000-0x8D000000, the window HollyHock gives to
 * programs), PAYLOAD_BASE (Makefile HHK_PAYLOAD) and IMAGE_BASE (the
 * translator's link address) come from the Makefile (--defsym). The
 * translator image is a second loadable region at PAYLOAD_BASE, which the
 * launcher's own copy fills while Casio's OS still runs; hhk/takeover.S moves
 * it to IMAGE_BASE once the OS is shut out (a forward copy, so the payload
 * must not sit below IMAGE_BASE; until 2026-11-06 the two were equal). */
OUTPUT_FORMAT("elf32-sh", "elf32-sh", "elf32-shl")
OUTPUT_ARCH(sh)
ENTRY(_start)
SECTIONS
{
    . = HHK_BASE;
    .text : { KEEP(*(.text.start)) *(.text .text.*) }
    . = ALIGN(4);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4);
    .note.hollyhock : { KEEP(*(.note.hollyhock)) }
    . = ALIGN(4);
    .data : { *(.data .data.*) }
    . = ALIGN(4);
    _bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(4);
    _bss_end = .;
    _hhk_end = .;

    .wince.payload PAYLOAD_BASE : { KEEP(*(.wince.payload)) }

    /* HollyHock 2 launcher metadata: read from the file, never loaded */
    .hollyhock_name        0 (INFO) : { KEEP(*(.hollyhock_name)) }
    .hollyhock_description 0 (INFO) : { KEEP(*(.hollyhock_description)) }
    .hollyhock_author      0 (INFO) : { KEEP(*(.hollyhock_author)) }
    .hollyhock_version     0 (INFO) : { KEEP(*(.hollyhock_version)) }

    /DISCARD/ : { *(.comment) *(.note.GNU-stack) *(.eh_frame*) }
}
ASSERT(PAYLOAD_BASE >= IMAGE_BASE, "the payload must not sit below IMAGE_BASE (takeover copies forward)")
ASSERT(_hhk_end <= HHK_LIMIT, "hhk stub does not fit the HollyHock program window (HHK_BASE..HHK_LIMIT)")
