/* The little-endian guest test image, embedded as bytes. GUEST is the path. */
        .section .rodata
        .balign 4
        .global _guest_blob_start, _guest_blob_end
_guest_blob_start:
        .incbin GUEST
_guest_blob_end:
