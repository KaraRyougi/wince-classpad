/* util.c - see util.h */
#include "util.h"

#if !defined(HHK_HOST) && !defined(FXCP)
void *memcpy(void *d, const void *s, unsigned n)
{
    u8 *dd = d; const u8 *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}

void *memset(void *d, int c, unsigned n)
{
    u8 *dd = d;
    while (n--) *dd++ = (u8)c;
    return d;
}

int memcmp(const void *a, const void *b, unsigned n)
{
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

unsigned strlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

int strncmp(const char *a, const char *b, unsigned n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (u8)*a - (u8)*b;
        if (!*a) return 0;
    }
    return 0;
}

#endif

u32 bytesum(const void *p, u32 n)
{
    const u8 *s = p; u32 sum = 0;
    while (n--) sum += *s++;
    return sum;
}

u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int put(char *out, unsigned cap, int pos, char c)
{
    if ((unsigned)pos + 1 < cap) out[pos] = c;
    return pos + 1;
}

int hhk_vformat(char *out, unsigned cap, const char *fmt, va_list ap)
{
    int pos = 0;
    char num[12];
    while (*fmt) {
        char c = *fmt++;
        if (c != '%') { pos = put(out, cap, pos, c); continue; }
        int zero = 0, width = 0;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        c = *fmt++;
        if (c == '%') { pos = put(out, cap, pos, '%'); continue; }
        if (c == 'c') { pos = put(out, cap, pos, (char)va_arg(ap, int)); continue; }
        if (c == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) pos = put(out, cap, pos, *s++);
            continue;
        }
        if (c == 'd' || c == 'u' || c == 'x' || c == 'X') {
            u32 v = va_arg(ap, u32);
            int neg = 0, n = 0, base = (c == 'd' || c == 'u') ? 10 : 16;
            const char *digits = (c == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            if (c == 'd' && (int)v < 0) { neg = 1; v = (u32)(-(int)v); }
            do { num[n++] = digits[v % base]; v /= base; } while (v);
            if (neg) num[n++] = '-';
            while (n < width && n < (int)sizeof num) num[n++] = zero ? '0' : ' ';
            while (n) pos = put(out, cap, pos, num[--n]);
            continue;
        }
        pos = put(out, cap, pos, '?');
    }
    if (cap) out[(unsigned)pos < cap ? (unsigned)pos : cap - 1] = 0;
    return pos;
}

int hhk_format(char *out, unsigned cap, const char *fmt, ...)
{
    va_list ap; int n;
    va_start(ap, fmt);
    n = hhk_vformat(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

#ifdef HHK_EMU
#define DEBUG_PORT (*(volatile u8 *)0xFFFFF000u)
void emu_puts(const char *s) { while (*s) DEBUG_PORT = (u8)*s++; }
void emu_printf(const char *fmt, ...)
{
    char buf[160]; va_list ap;
    va_start(ap, fmt);
    hhk_vformat(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emu_puts(buf);
}
#endif
