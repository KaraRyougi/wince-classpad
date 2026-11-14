/* nkflash.h - the CE image flasher used by wince.hh3 (nkflash.c) */
#ifndef NKFLASH_H
#define NKFLASH_H

enum { NKF_NOFILE = 1, NKF_READY = 0, NKF_BAD = -1 };

/* a NOR window and the files that feed it (NK, EXT) */
typedef struct {
    const char *name;                            /* "NK" / "EXT" */
    u32 base, max;                               /* P1 start, bytes */
    const char *const *paths;                    /* candidate files, 0-terminated */
    int eact;                                    /* the eActivity area: Fugue's, erasable */
} nkwindow;

extern const nkwindow nk_windows[];
extern const int nk_nwindows;

extern char nk_last[48];                         /* the last line, e.g. why a step failed */
/* A window that neither is blank nor shows our image is not erased: the
 * write stops before the first erase with nk_refused set. That is also what
 * a flash of ours looks like when it was cut off (a reset or the batteries
 * during the erase: the marker sector may be gone while later sectors still
 * hold the old image), so the caller may ask the user and try again with
 * nk_force set: every sector of the window's span is then erased. */
extern int nk_refused, nk_force;
extern volatile int nk_abort;                    /* set (from nk_progress): nkflash_compare stops, returns -2 */
/* progress of the step running: compare and write in bytes, erase in
 * sectors, record = the record being written (1-based) */
enum { NKP_COMPARE, NKP_ERASE, NKP_WRITE, NKP_RECORD };
extern void (*nk_progress)(int what, u32 done, u32 total, u32 addr);
typedef struct { u32 size, bytes, records, sectors; int xip; } nkinfo;
void nkflash_info(nkinfo *i);                    /* after nkflash_open: what a write will do */
void nk_say(const char *fmt, ...);               /* a step's line, kept in nk_last */
const char *nkflash_window_state(const nkwindow *w); /* "is blank" / "holds a CE image" / "is the eActivity volume" */
int  window_has_our_image(const nkwindow *w);    /* "ECEC" at +0x40 or a B000FF header */
int  nkflash_open(const nkwindow *w);            /* the window's file found and sane: NKF_READY */
int  nkflash_compare(void);                      /* 1 already in flash, 0 differs, -1 read error, -2 nk_abort */
int  nkflash_write(void);                        /* erase the span and program; 0 ok */
void nkflash_close(void);
#endif
