/* Hypercalls to the translator (TRA 0xF0..0xFF). */
#ifndef HYPER_H
#define HYPER_H
static inline void hc_exit(int code) { register int r4 __asm__("r4") = code; __asm__ volatile ("trapa #0xF0" :: "r"(r4) : "memory"); for (;;) ; }
static inline void hc_puts(const char *s) { register const char *r4 __asm__("r4") = s; __asm__ volatile ("trapa #0xF1" :: "r"(r4) : "memory"); }
static inline void hc_putc(int c) { register int r4 __asm__("r4") = c; __asm__ volatile ("trapa #0xF3" :: "r"(r4) : "memory"); }
static inline void hc_puthex(unsigned v) { register unsigned r4 __asm__("r4") = v; __asm__ volatile ("trapa #0xF4" :: "r"(r4) : "memory"); }
static inline void hc_stats(void) { __asm__ volatile ("trapa #0xF5" ::: "memory"); }
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
