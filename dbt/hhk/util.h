/* util.h - the little C runtime the HollyHock programs need (no libc). */
#ifndef HHK_UTIL_H
#define HHK_UTIL_H
#include <stdarg.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

void *memcpy(void *d, const void *s, unsigned n);
void *memset(void *d, int c, unsigned n);
int   memcmp(const void *a, const void *b, unsigned n);
unsigned strlen(const char *s);
int   strncmp(const char *a, const char *b, unsigned n);
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
