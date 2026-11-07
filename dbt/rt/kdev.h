/* kdev.h - the kernel.dll functions that need device-aware translation
 * (rt/kdev.c, generated table rt/kernel_dev.h, tools/kernel_devfuncs.py). */
#ifndef KDEV_H
#define KDEV_H
#include "rt.h"

#define KDEV_MAX_RANGES 64

/* Find the listed functions in kernel.dll's text [text, text + len) of the
 * image being booted. Fills ranges with sorted, merged [lo, hi) pairs
 * (addresses & 0x1FFFFFFF) and returns their number; 0 when any function is
 * missing or ambiguous (keep the whole module device-aware). ranges holds
 * 2 * max words. */
unsigned kdev_locate(sf_u32 text, sf_u32 len, sf_u32 *ranges, unsigned max);
#endif
