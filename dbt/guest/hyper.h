/* Hypercalls to the translator (TRA 0xF0..0xFF). */
#ifndef HYPER_H
#define HYPER_H
static inline void hc_exit(int code) { register int r4 __asm__("r4") = code; __asm__ volatile ("trapa #0xF0" :: "r"(r4) : "memory"); for (;;) ; }
static inline void hc_puts(const char *s) { register const char *r4 __asm__("r4") = s; __asm__ volatile ("trapa #0xF1" :: "r"(r4) : "memory"); }
static inline void hc_putc(int c) { register int r4 __asm__("r4") = c; __asm__ volatile ("trapa #0xF3" :: "r"(r4) : "memory"); }
static inline void hc_puthex(unsigned v) { register unsigned r4 __asm__("r4") = v; __asm__ volatile ("trapa #0xF4" :: "r"(r4) : "memory"); }
static inline void hc_stats(void) { __asm__ volatile ("trapa #0xF5" ::: "memory"); }
#endif
