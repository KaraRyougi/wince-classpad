/* util.h - the little C runtime the HollyHock programs need (no libc). */
#ifndef HHK_UTIL_H
#define HHK_UTIL_H
#include <stdarg.h>

#include <stdint.h>
typedef uint8_t  u8;                                    /* the same types as gint's */
typedef uint16_t u16;
typedef uint32_t u32;

#if defined(HHK_HOST) || defined(FXCP)                  /* host preview, gint: a real libc */
#include <string.h>
#else
void *memcpy(void *d, const void *s, unsigned n);
void *memset(void *d, int c, unsigned n);
int   memcmp(const void *a, const void *b, unsigned n);
unsigned strlen(const char *s);
int   strncmp(const char *a, const char *b, unsigned n);
#endif
/* %s %c %d %u %x %X %% with optional 0-padding and width (e.g. %08x) */
int   hhk_vformat(char *out, unsigned cap, const char *fmt, va_list ap);
int   hhk_format(char *out, unsigned cap, const char *fmt, ...);
u32   bytesum(const void *p, u32 n);                    /* 32-bit sum of bytes */
u32   le32(const u8 *p);                                /* little-endian field */

#ifdef HHK_EMU
void emu_puts(const char *s);                           /* casio-emu debug port */
void emu_printf(const char *fmt, ...);
#endif
#endif
