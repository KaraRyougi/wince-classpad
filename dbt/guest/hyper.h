/* Hypercalls to the translator (TRA 0xF0..0xFF). */
#ifndef HYPER_H
#define HYPER_H
static inline void hc_exit(int code) { register int r4 __asm__("r4") = code; __asm__ volatile ("trapa #0xF0" :: "r"(r4) : "memory"); for (;;) ; }
static inline void hc_puts(const char *s) { register const char *r4 __asm__("r4") = s; __asm__ volatile ("trapa #0xF1" :: "r"(r4) : "memory"); }
static inline void hc_putc(int c) { register int r4 __asm__("r4") = c; __asm__ volatile ("trapa #0xF3" :: "r"(r4) : "memory"); }
static inline void hc_puthex(unsigned v) { register unsigned r4 __asm__("r4") = v; __asm__ volatile ("trapa #0xF4" :: "r"(r4) : "memory"); }
static inline void hc_stats(void) { __asm__ volatile ("trapa #0xF5" ::: "memory"); }
/* paravirtual notifications a kernel/OAL would issue */
static inline void hc_tlb_inval(unsigned addr, unsigned len)   /* user mappings changed; len 0 = all */
{
    register unsigned r4 __asm__("r4") = addr; register unsigned r5 __asm__("r5") = len;
    __asm__ volatile ("trapa #0xF9" :: "r"(r4), "r"(r5) : "memory");
}
static inline void hc_asid(unsigned asid)
{
    register unsigned r4 __asm__("r4") = asid;
    __asm__ volatile ("trapa #0xFA" :: "r"(r4) : "memory");
}
static inline unsigned hc_getstat(unsigned idx)
{
    register unsigned r4 __asm__("r4") = idx; register unsigned r0 __asm__("r0");
    __asm__ volatile ("trapa #0xFB" : "=r"(r0) : "r"(r4) : "memory");
    return r0;
}
/* tests: translate guest code in [lo, hi) device-aware from the next lookup on (docs/05) */
static inline void hc_devrange(unsigned lo, unsigned hi)
{
    register unsigned r4 __asm__("r4") = lo; register unsigned r5 __asm__("r5") = hi;
    __asm__ volatile ("trapa #0xFC" :: "r"(r4), "r"(r5) : "memory");
}
/* guest code in [addr, addr+len) changed: drop its translations */
static inline void hc_flush(unsigned addr, unsigned len)
{
    register unsigned r4 __asm__("r4") = addr; register unsigned r5 __asm__("r5") = len;
    __asm__ volatile ("trapa #0xF2" :: "r"(r4), "r"(r5) : "memory");
}
/* print `words` 16-bit host words starting at host address `addr` (debugging) */
static inline void hc_dump(unsigned addr, unsigned words)
{
    register unsigned r4 __asm__("r4") = addr; register unsigned r5 __asm__("r5") = words;
    __asm__ volatile ("trapa #0xF7" :: "r"(r4), "r"(r5) : "memory");
}
/* run benchmark kernel `which` natively for n iterations (0xFFFFFFFF: start TMU1); returns TMU1 ticks */
static inline unsigned hc_bench(unsigned which, unsigned n)
{
    register unsigned r4 __asm__("r4") = which; register unsigned r5 __asm__("r5") = n; register unsigned r0 __asm__("r0");
    __asm__ volatile ("trapa #0xF6" : "=r"(r0) : "r"(r4), "r"(r5) : "memory");
    return r0;
}
#endif
/* FD: the display push done natively by the translator: pixels as stored by
 * the guest (little-endian 16-bit) from a P1/P2 address to the LCD data port */
static inline void hc_lcd_blit(const void *src, unsigned pixels, unsigned port)
{
    register const void *r4 __asm__("r4") = src; register unsigned r5 __asm__("r5") = pixels; register unsigned r6 __asm__("r6") = port;
    __asm__ volatile ("trapa #0xFD" :: "r"(r4), "r"(r5), "r"(r6) : "memory");
}
/* FE: DCS panel push with the window set by the translator (atomic with the pixels) */
static inline void hc_lcd_push(const void *src, unsigned top, unsigned rows, unsigned width)
{
    register const void *r4 __asm__("r4") = src; register unsigned r5 __asm__("r5") = top;
    register unsigned r6 __asm__("r6") = rows; register unsigned r7 __asm__("r7") = width;
    __asm__ volatile ("trapa #0xFE" :: "r"(r4), "r"(r5), "r"(r6), "r"(r7) : "memory");
}
