/* nkbin.h - Windows CE image loader for the translator runtime. Two forms:
 * a .bin (B000FF) whose records are copied byte-for-byte into guest RAM (our
 * memory model is byte-identical to a little-endian machine), and a flat
 * ROMIMAGE already in the NOR that the kernel executes in place (XIP). In
 * both the ROMHDR is found through the "ECEC" marker at image start + 0x40
 * and gives the RAM range, kernel.dll's code range and nk.exe's entry. */
#ifndef NKBIN_H
#define NKBIN_H
#include "rt.h"

typedef struct {
    sf_u32 start, length, entry;      /* from the header and the terminator record */
    sf_u32 nrecords, bytes;           /* records copied */
    sf_u32 romhdr;                    /* address of the ROMHDR, 0 if no ECEC marker */
    sf_u32 physfirst, physlast, nummods;
    sf_u32 ram_start, ram_free, ram_end;
    sf_u32 kernel_flags, cpu_type;    /* usCPUType: 0x01A6 = SH4 */
    sf_u32 kernel_lo, kernel_hi;      /* kernel.dll from the module table (load .. load + e32 vsize), 0 if absent */
    sf_u32 kernel_text, kernel_text_len; /* its largest code section (.text), 0 if absent */
    sf_u32 nk_entry;                  /* nk.exe's entry point from its e32 header, 0 if absent */
    sf_u32 fail_off;                  /* source offset where parsing stopped (diagnostics) */
    int    xip;                       /* 1: executes in place from flash (nothing was copied) */
} nkbin_info;

enum { NKBIN_OK = 0, NKBIN_ERR_MAGIC, NKBIN_ERR_TRUNCATED, NKBIN_ERR_RANGE, NKBIN_ERR_CHECKSUM, NKBIN_ERR_NOENTRY };

/* True when `p` starts with the B000FF magic. */
int nkbin_is_image(const sf_u8 *p);

/* Load the image at `bin` (at most `maxlen` bytes readable) into guest RAM,
 * accepting only records inside [ram_lo, ram_hi). Fills *out as far as
 * parsing got. */
int nkbin_load(const sf_u8 *bin, sf_u32 maxlen, sf_u32 ram_lo, sf_u32 ram_hi, nkbin_info *out);

/* A flat ROMIMAGE at `start` with its bytes readable in [lo, hi): the ECEC
 * marker at start + 0x40 and a ROMHDR pointer inside the range. */
int nkbin_is_xip(sf_u32 start, sf_u32 lo, sf_u32 hi);

/* Describe the XIP image at `start` (no copy): ROMHDR, kernel.dll range and
 * the entry (nk.exe's, or the image start when the module table lacks it). */
int nkbin_xip(sf_u32 start, sf_u32 lo, sf_u32 hi, nkbin_info *out);

void nkbin_print(const nkbin_info *i);
#endif
