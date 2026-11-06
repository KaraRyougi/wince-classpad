/* nkflash.h - the CE image flasher as a library for wince.hhk (nkflash.c) */
#ifndef NKFLASH_H
#define NKFLASH_H

enum { NKF_NOFILE = 1, NKF_READY = 0, NKF_BAD = -1 };

/* a NOR window and the files that feed it: NK always, EXT with NK_WINDOW=eact */
typedef struct {
    const char *name;                            /* "NK" / "EXT" */
    u32 base, max;                               /* P1 start, bytes */
    const char *const *paths;                    /* candidate files, 0-terminated */
    int eact;                                    /* the eActivity area: Fugue's, erasable */
} nkwindow;

extern const nkwindow nk_windows[];
extern const int nk_nwindows;

extern int nk_row;                               /* next UI row the flasher prints on */
void nk_say(const char *fmt, ...);               /* one UI line, refreshed */
const char *nkflash_window_state(const nkwindow *w); /* "is blank" / "holds a CE image" / "is the eActivity volume" */
int  nkflash_open(const nkwindow *w);            /* the window's file found and sane: NKF_READY */
int  nkflash_compare(void);                      /* 1 already in flash, 0 differs, -1 read error */
int  nkflash_write(void);                        /* erase the span and program; 0 ok */
void nkflash_close(void);
#endif
