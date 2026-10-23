/* nkbin.h - Windows CE .bin (B000FF) image loader for the translator runtime.
 * The image bytes are a little-endian memory image; records are copied
 * byte-for-byte into guest RAM (our memory model is byte-identical to a
 * little-endian machine), then the ROMHDR is read back through the "ECEC"
 * marker at image start + 0x40. */
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
    sf_u32 fail_off;                  /* source offset where parsing stopped (diagnostics) */
} nkbin_info;

enum { NKBIN_OK = 0, NKBIN_ERR_MAGIC, NKBIN_ERR_TRUNCATED, NKBIN_ERR_RANGE, NKBIN_ERR_CHECKSUM, NKBIN_ERR_NOENTRY };

/* True when `p` starts with the B000FF magic. */
int nkbin_is_image(const sf_u8 *p);

/* Load the image at `bin` (at most `maxlen` bytes readable) into guest RAM,
 * accepting only records inside [ram_lo, ram_hi). Fills *out as far as
 * parsing got. */
int nkbin_load(const sf_u8 *bin, sf_u32 maxlen, sf_u32 ram_lo, sf_u32 ram_hi, nkbin_info *out);

void nkbin_print(const nkbin_info *i);
#endif
