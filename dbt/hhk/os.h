/* os.h - the few Casio OS routines the HollyHock programs call, and how they
 * are found. HollyHock 3 programs normally get OS addresses from a symbol table
 * the launcher hands them (HHK_SYMBOL_TABLE=<hex> in the environment, a table
 * that starts with the OS build date found at 0x814FFFE0, then name\0 + 32-bit
 * big-endian address entries: hollyhock-3 sdk/os/functions/nolto/impl.cpp).
 * We take that table when it is there and otherwise fall back to the known
 * addresses of OS 02.01.7002.0001 (hollyhock-3 versions/.../addresses.json),
 * used only when the OS identifies itself as that version. Without either the
 * programs run headless (the launcher still boots, the flasher gives up). */
#ifndef HHK_OS_H
#define HHK_OS_H
#include "util.h"

extern int os_ok;                 /* 1: the routines below may be called */
extern const char *os_source;     /* where the addresses came from */

int  os_init(int argc, char **argv, char **envp);

/* text UI: 40 columns x 37 rows of the debug font, shown by ui_refresh() */
void ui_clear(void);
void ui_printf(int row, const char *fmt, ...);
void ui_refresh(void);
/* KEYSC read, no OS involved: returns 1 for EXE, 0 for Clear/ON; ui_wait_key(1)
 * also returns 2 for the Keyboard key */
int  ui_wait_exe_or_clear(void);
int  ui_wait_key(int keyboard_too);

void *os_malloc(unsigned n);
void  os_free(void *p);
int   os_open(const char *path);                 /* fd >= 0, or a negative File_Error */
int   os_read(int fd, void *buf, int n);         /* bytes read, or negative */
int   os_lseek(int fd, int off);                 /* absolute seek */
int   os_fsize(int fd, unsigned *size);
int   os_close(int fd);
#endif
