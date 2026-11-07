/* payload.S - the translator image (dbt-<guest>.bin, linked at IMAGE_BASE) as
 * a loadable section. In the .hhk the linker puts it at PAYLOAD_BASE (Makefile
 * HHK_PAYLOAD; takeover moves it to IMAGE_BASE); in
 * the emulator variant it follows the stub in the add-in window. IMAGE is the
 * path of the image, from the Makefile. */
        .section .wince.payload,"a",@progbits
        .balign 32
        .global _payload_start, _payload_end
_payload_start:
        .incbin IMAGE
_payload_end:
