/* mmio.h - MMIO access from a little-endian guest under the translator.
 *
 * The translator byte-swaps every 16/32-bit load and store so that the
 * guest sees a little-endian memory image. Device registers are not memory:
 * the bus delivers the bytes in big-endian order, so the guest must
 * pre-swap the value it writes and post-swap the value it reads to cancel
 * the translator's swap. Byte accesses need nothing. See docs/04. */
#ifndef GUEST_MMIO_H
#define GUEST_MMIO_H

static inline unsigned mm_bswap32(unsigned v)
{ return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
static inline unsigned short mm_bswap16(unsigned short v)
{ return (unsigned short)((v >> 8) | (v << 8)); }

static inline void mmio_w32(unsigned a, unsigned v) { *(volatile unsigned *)a = mm_bswap32(v); }
static inline unsigned mmio_r32(unsigned a)         { return mm_bswap32(*(volatile unsigned *)a); }
static inline void mmio_w16(unsigned a, unsigned short v) { *(volatile unsigned short *)a = mm_bswap16(v); }
static inline unsigned short mmio_r16(unsigned a)   { return mm_bswap16(*(volatile unsigned short *)a); }
static inline void mmio_w8(unsigned a, unsigned char v) { *(volatile unsigned char *)a = v; }
static inline unsigned char mmio_r8(unsigned a)     { return *(volatile unsigned char *)a; }

#endif
