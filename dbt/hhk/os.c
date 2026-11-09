/* os.c - see os.h */
#include "os.h"

int os_ok = 0;
const char *os_source = "none";

static void (*Debug_Printf)(unsigned x, unsigned y, int invert, int zero, const char *fmt, ...);
static void (*LCD_Refresh)(void);
static void (*LCD_ClearScreen)(void);
static void *(*Mem_Malloc)(unsigned n);
static void (*Mem_Free)(void *p);
static int (*File_Open)(const char *path, int flags);
static int (*File_Read)(int fd, void *buf, int n);
static int (*File_Lseek)(int fd, int off, int whence);
static int (*File_Fstat)(int fd, void *stat);
static int (*File_Close)(int fd);

struct sym { const char *name; void **slot; u32 v7002; };
static struct sym syms[] = {
    { "Debug_Printf",    (void **)&Debug_Printf,    0x8002dbb0u },
    { "LCD_Refresh",     (void **)&LCD_Refresh,     0x800373acu },
    { "LCD_ClearScreen", (void **)&LCD_ClearScreen, 0x80039f0cu },
    { "Mem_Malloc",      (void **)&Mem_Malloc,      0x800d0ad8u },
    { "Mem_Free",        (void **)&Mem_Free,        0x800a86acu },
    { "File_Open",       (void **)&File_Open,       0x80058a1cu },
    { "File_Read",       (void **)&File_Read,       0x80058a46u },
    { "File_Lseek",      (void **)&File_Lseek,      0x80058b6cu },
    { "File_Fstat",      (void **)&File_Fstat,      0x80058aeeu },
    { "File_Close",      (void **)&File_Close,      0x80058aa2u },
};
#define NSYMS (sizeof syms / sizeof syms[0])

#define OS_VERSION   ((const char *)0x80020028u)   /* "02.01.7002.0001" */
#define OS_BUILDDATE ((const char *)0x814FFFE0u)   /* HollyHock's "safe guard" */

static int in_ram(u32 a)   { return a >= 0x8C000000u && a < 0x8D000000u; }
static int in_flash(u32 a) { return a >= 0x80000000u && a < 0x84000000u; }
static int in_os_code(u32 a) { return a >= 0x80000000u && a < 0x81500000u; }

static int parse_hex(const char *s, u32 *out)
{
    u32 v = 0; int n = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++, n++) {
        char c = *s; u32 d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return n > 0 && n <= 8;
}

/* HollyHock 3 symbol table: build date (as at OS_BUILDDATE), then entries */
static int load_table(const u8 *t, u32 len)
{
    u32 pos, found = 0, i;
    unsigned guard = strlen(OS_BUILDDATE);
    if (guard == 0 || guard > 31 || strncmp((const char *)t, OS_BUILDDATE, guard + 1)) return 0;
    pos = guard + 1;
    while (pos + 5 <= len) {
        const char *name = (const char *)t + pos; unsigned nl = 0;
        while (pos + nl < len && name[nl] >= 0x20 && name[nl] < 0x7F) nl++;
        if (nl == 0 || nl > 80 || pos + nl >= len || name[nl] != 0) break;
        pos += nl + 1;
        if (pos + 4 > len) break;
        u32 addr = ((u32)t[pos] << 24) | ((u32)t[pos + 1] << 16) | ((u32)t[pos + 2] << 8) | t[pos + 3];
        pos += 4;
        for (i = 0; i < NSYMS; i++)
            if (!strncmp(name, syms[i].name, nl + 1) && in_os_code(addr)) { *syms[i].slot = (void *)addr; found++; }
    }
    return found == NSYMS;
}

static int from_env(char **envp)
{
    u32 table = 0, len = 0, i;
    if (!envp || !(in_ram((u32)envp) || in_flash((u32)envp))) return 0;
    for (i = 0; i < 64 && envp[i]; i++) {
        const char *e = envp[i];
        if (!(in_ram((u32)e) || in_flash((u32)e))) return 0;
        if (!strncmp(e, "HHK_SYMBOL_TABLE=", 17)) { if (!parse_hex(e + 17, &table)) return 0; }
        else if (!strncmp(e, "HHK_SYMBOL_TABLE_LEN=", 21)) { if (!parse_hex(e + 21, &len)) return 0; }
    }
    if (!table || !(in_ram(table) || in_flash(table))) return 0;
    if (len == 0 || len > 0x10000) len = 0x10000;
    return load_table((const u8 *)table, len);
}

int os_init(int argc, char **argv, char **envp)
{
    u32 i;
    (void)argc; (void)argv;
    if (from_env(envp)) { os_ok = 1; os_source = "HHK_SYMBOL_TABLE"; return 1; }
    if (!strncmp(OS_VERSION, "02.01.7002", 10) && !strncmp(OS_BUILDDATE, "2023.0209.1500", 15)) {
        for (i = 0; i < NSYMS; i++) *syms[i].slot = (void *)syms[i].v7002;
        os_ok = 1; os_source = "built-in table for OS 02.01.7002";
        return 1;
    }
    return 0;
}

void ui_clear(void) { if (os_ok) LCD_ClearScreen(); }

void ui_printf(int row, const char *fmt, ...)
{
    char buf[48]; va_list ap;
    if (!os_ok) return;
    va_start(ap, fmt);
    hhk_vformat(buf, 41, fmt, ap);        /* one 40-column line */
    va_end(ap);
    Debug_Printf(0, (unsigned)row, 0, 0, "%s", buf);
}

void ui_refresh(void) { if (os_ok) LCD_Refresh(); }

/* KEYSC data registers as hollyhock's getKey() reads them (sdk/calc/calc.cpp) */
static void keys(u32 *k1, u32 *k2)
{
    volatile u16 *hw = (volatile u16 *)0xA44B0000u;
    *k1 = ((u32)hw[0] << 16) | hw[1];
    *k2 = ((u32)hw[2] << 16) | hw[3];
    (void)hw[4]; (void)hw[5];
}
/* KIUDATA[w] bit b is bit (16 * (1 - w % 2) + b) of k1 (w = 0, 1) or k2
 * (w = 2, 3); the fx-CP400 key at matrix code c (casio-emu's numbering, the
 * BSP's kbd_layout.cpp) has w = (c % 10) / 2, b = c / 10 - 1 + 8 * (c % 2). */
#define KEY1_EXE      0x04000000u      /* code 31 */
#define KEY1_CLEAR    0x00020000u      /* code 20 */
#define KEY2_KEYBOARD 0x80000000u      /* code 85: the Keyboard key */

int ui_wait_key(int keyboard_too)
{
    u32 k1, k2;
    do keys(&k1, &k2); while ((k1 & (KEY1_EXE | KEY1_CLEAR)) || (k2 & KEY2_KEYBOARD));   /* released first */
    for (;;) {
        keys(&k1, &k2);
        if (k1 & KEY1_EXE) return 1;
        if (k1 & KEY1_CLEAR) return 0;
        if (keyboard_too && (k2 & KEY2_KEYBOARD)) return 2;
    }
}

int ui_wait_exe_or_clear(void) { return ui_wait_key(0); }

void *os_malloc(unsigned n) { return os_ok ? Mem_Malloc(n) : 0; }
void  os_free(void *p)      { if (os_ok && p) Mem_Free(p); }
int   os_open(const char *path) { return File_Open(path, 1 /* FILE_OPEN_READ */); }
int   os_read(int fd, void *buf, int n) { return File_Read(fd, buf, n); }
int   os_lseek(int fd, int off) { return File_Lseek(fd, off, 0 /* SEEK_SET */); }
int   os_close(int fd) { return File_Close(fd); }
int   os_fsize(int fd, unsigned *size)
{
    u32 st[8];                        /* struct File_Stat: fileSize at +4 */
    int r = File_Fstat(fd, st);
    if (r < 0) return r;
    *size = st[1];
    return 0;
}
