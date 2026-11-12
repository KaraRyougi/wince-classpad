/* nor.c - programming the calculator's NOR for the guest (docs/08 "storage").
 *
 * The CE storage driver writes Casio's storage memory (Fugue, chip
 * 0x01A20000-0x0385FFFF) but cannot drive the chip itself: while the chip
 * takes a command it returns status words instead of data, and the guest
 * executes from it (the XIP image), as does the translator when it reads
 * guest code to translate. So the guest asks, and the operation runs here,
 * in RAM, with interrupts masked, start to finish:
 *
 *   trapa #0xFF (HC_STANDBY) with r4 = HC_NOR_TAG | op
 *     op 0  probe            -> r0 = HC_NOR_MAGIC
 *     op 1  program          r5 = chip offset (even), r6 = source (P1/P2
 *                            address in RAM), r7 = bytes (even, at most 4096)
 *     op 2  erase a sector   r5 = chip offset of the sector
 *   -> r0 = 0 done and verified, 1 bad arguments, 2 the chip did not finish,
 *      3 verify mismatch
 *
 * Only the storage memory can be written: a range that leaves it is refused,
 * whatever asks. The chip commands are those of hhk/norflash.c (AMD command
 * set, 64-byte write buffer, uniform 128 KiB sectors; casio-emu decodes them
 * with HLE_FLASH_CMD=1). Afterwards the operand cache lines of the P1 alias
 * are invalidated, because the guest reads the flash through P1.
 *
 * A sector erase takes the chip about half a second, during which nothing
 * else runs: the guest's clock loses the timer interrupts of that time. */
#include "rt.h"

#define NOR_P1          0x80000000u
#define NOR_P2          0xA0000000u
#define NOR_SECTOR      0x00020000u
#define NOR_WR_LO       0x01A20000u     /* Casio's storage memory: 242 sectors */
#define NOR_WR_HI       0x03860000u
#define RAM_LO          0x0C000000u
#define RAM_HI          0x0D000000u
#define UNLOCK1         0xAAAu
#define UNLOCK2         0x554u
#define ERASE_LIMIT     200000000u
#define PROGRAM_LIMIT   2000000u

static inline void w16(sf_u32 off, sf_u16 v) { *(volatile sf_u16 *)(NOR_P2 + off) = v; }
static inline sf_u16 r16(sf_u32 off)         { return *(volatile sf_u16 *)(NOR_P2 + off); }

static sf_u32 irq_off(void)
{
    sf_u32 sr, masked;
    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    masked = sr | 0xF0u;
    __asm__ volatile ("ldc %0, sr" :: "r"(masked) : "memory");
    return sr;
}
static void irq_restore(sf_u32 sr) { __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory"); }

static void unlock(sf_u32 sector)
{
    w16(sector + UNLOCK1, 0xAA);
    w16(sector + UNLOCK2, 0x55);
}

static int poll(sf_u32 off, sf_u16 expect, sf_u32 limit)
{
    while (r16(off) != expect) if (!limit--) return -1;
    return 0;
}

/* the guest reads the flash through the cached P1 alias */
static void drop_cache(sf_u32 off, sf_u32 len)
{
    sf_u32 a = (NOR_P1 + off) & ~31u, end = NOR_P1 + off + len;
    for (; a < end; a += 32) __asm__ volatile ("ocbi @%0" :: "r"(a) : "memory");
}

static sf_u32 program_line(sf_u32 off, const sf_u8 *src, sf_u32 n)
{
    sf_u32 sector = off & ~(NOR_SECTOR - 1), i, sr, words = n / 2;
    sf_u16 last = 0;
    int r;
    sr = irq_off();
    unlock(sector);
    w16(sector, 0x25);
    w16(sector, (sf_u16)(words - 1));
    for (i = 0; i < words; i++) {
        last = (sf_u16)(((sf_u16)src[2 * i] << 8) | src[2 * i + 1]);
        w16(off + 2 * i, last);
    }
    w16(sector, 0x29);
    r = poll(off + 2 * (words - 1), last, PROGRAM_LIMIT);
    if (r) w16(sector, 0xF0);
    irq_restore(sr);
    if (r) return 2;
    for (i = 0; i < n; i++)
        if (*(const volatile sf_u8 *)(NOR_P2 + off + i) != src[i]) return 3;
    return 0;
}

static sf_u32 nor_program(sf_u32 off, sf_u32 src, sf_u32 len)
{
    sf_u32 pa = src & 0x1FFFFFFFu, o = off, left = len, n, r = 0;
    const sf_u8 *s = (const sf_u8 *)src;
    if ((off | len) & 1u || !len || len > 4096u) return 1;
    if (off < NOR_WR_LO || off >= NOR_WR_HI || len > NOR_WR_HI - off) return 1;
    if (src < NOR_P1 || src >= 0xC0000000u || pa < RAM_LO || pa >= RAM_HI || len > RAM_HI - pa) return 1;
    while (left && !r) {
        n = 64 - (o & 63);
        if (n > left) n = left;
        r = program_line(o, s, n);
        o += n; s += n; left -= n;
    }
    drop_cache(off, len);
    return r;
}

static sf_u32 erase_sector(sf_u32 sector);

static sf_u32 nor_erase(sf_u32 off)
{
    sf_u32 sector = off & ~(NOR_SECTOR - 1);
    if (off & (NOR_SECTOR - 1) || sector < NOR_WR_LO || sector >= NOR_WR_HI) return 1;
    return erase_sector(sector);
}

static sf_u32 erase_sector(sf_u32 sector)
{
    sf_u32 sr, i;
    const volatile sf_u32 *p = (const volatile sf_u32 *)(NOR_P2 + sector);
    int r;
    sr = irq_off();
    unlock(sector);
    w16(sector + UNLOCK1, 0x80);
    unlock(sector);
    w16(sector, 0x30);
    r = poll(sector, 0xFFFF, ERASE_LIMIT);
    if (r == 0) r = poll(sector + NOR_SECTOR - 2, 0xFFFF, ERASE_LIMIT);
    w16(sector, 0xF0);
    irq_restore(sr);
    drop_cache(sector, NOR_SECTOR);
    if (r) return 2;
    for (i = 0; i < NOR_SECTOR / 4; i++) if (p[i] != 0xFFFFFFFFu) return 3;
    return 0;
}

sf_u32 dbt_nor(sf_u32 op, sf_u32 a, sf_u32 b, sf_u32 c)
{
    switch (op) {
    case 0: return HC_NOR_MAGIC;
    case 1: return nor_program(a, b, c);
    case 2: return nor_erase(a);
    }
    return 1;
}

/* ------------------------------------------------------------ boot log
 * make FLASHLOG=1 (a DEVICE=1 build has no debug port): what the guest and
 * the translator print goes, line by line, into the last sector of the NK
 * window, which the image leaves free (NK_FLASH + NK_FLASH_MAX - 128 KiB;
 * no log when the image reaches into it). The sector is erased when the
 * translator starts and takes the first LOG_MAX bytes of the session, which
 * is the boot; after a boot that hangs, RESET and wince.hh3 again, and the
 * loader copies the text to \fls0\WINCE.LOG (hhk/loader.c). Flash keeps
 * it whatever Casio's OS does with the RAM on its way up.
 * Bits only go from 1 to 0, so a line is programmed once, padded to an even
 * length; the text ends at the first 0xFF. Diagnostic: every start costs an
 * erase of that sector (half a second, one cycle of its life). */
#ifdef DBT_FLASH_LOG
#define LOG_OFF     (NK_FLASH - NOR_P1 + NK_FLASH_MAX - NOR_SECTOR)
#define LOG_MAX     0x1F000u
static sf_u8 g_log_line[64];
static sf_u32 g_log_fill, g_log_at, g_log_busy;
static int g_log_on;

static void log_stamp(void);

void dbt_flashlog_init(sf_u32 nk_end)
{
    const volatile sf_u32 *p = (const volatile sf_u32 *)(NOR_P2 + LOG_OFF);
    sf_u32 i;
    if (nk_end > NOR_P1 + LOG_OFF) return;
    for (i = 0; i < NOR_SECTOR / 4; i++) if (p[i] != 0xFFFFFFFFu) break;
    if (i < NOR_SECTOR / 4 && erase_sector(LOG_OFF)) {
        /* the old log stays; whatever room is left after it says so */
        static const sf_u8 note[] = "\n[log] the erase before the next boot failed\n ";
        for (i = 0; i < 0x1F000u && *(const volatile sf_u8 *)(NOR_P2 + LOG_OFF + i) != 0xFF; i++) ;
        i = (i + 63u) & ~63u;
        if (i < 0x1F000u) program_line(LOG_OFF + i, note, (sizeof note - 1) & ~1u);
        return;
    }
    g_log_fill = g_log_at = 0;
    g_log_on = 1;
    log_stamp();
}

static void log_heard(int c);

void dbt_flashlog_putc(int c)
{
    sf_u32 o, n, left;
    const sf_u8 *s;
    if (!g_log_on) return;
    log_heard(c);
    g_log_line[g_log_fill++] = (sf_u8)c;
    if (c != '\n' && g_log_fill < sizeof g_log_line) return;
    if (g_log_fill & 1) g_log_line[g_log_fill++] = ' ';    /* only after a newline: a full line is even */
    if (g_log_at + g_log_fill > LOG_MAX) { g_log_on = 0; return; }
    g_log_busy++;                       /* the sampler (an interrupt) stays out until the line is in */
    for (o = LOG_OFF + g_log_at, s = g_log_line, left = g_log_fill; left; o += n, s += n, left -= n) {
        n = 64 - (o & 63);
        if (n > left) n = left;
        if (program_line(o, s, n)) { g_log_on = 0; break; }
    }
    g_log_at += g_log_fill;
    g_log_fill = 0;
    g_log_busy--;
}

/* Until the display driver signs on, the log also gets, every two seconds
 * by the RTC's 128 Hz counter (the system tick is not a clock here: on the
 * calculator a boot that hung logged one sample paced by ticks and then no
 * more), where the guest is: the guest pc and SR at whatever exception or
 * interrupt was delivered to it next, what that was, and the address space
 * (PTEH); and after every REPORT samples how often each kind came since the
 * last report (interrupts by INTEVT, exceptions by EXPEVT + 0x10000). A
 * loop shows as the same few pcs, a storm as one source with a huge count,
 * a wait as the idle loop; a guest that takes nothing at all any more
 * leaves no sample, which says that much. MAX_SAMPLES in all. The first
 * line of the log is the RTC's time (BCD hour, minute, second), so that two
 * boots' logs differ. Once the display driver has signed on the samples go
 * on at one every five seconds for as long as the log has room: for a
 * system that turns slow later (after the USB drive was used, say). */
#define SAMPLE_64THS    256u            /* 2 s of the 128 Hz counter */
#define REPORT          8u
#define MAX_SAMPLES     64u             /* until the display driver signs on */
#define RUN_64THS       640u            /* afterwards: one every 5 s, as long as the log has room (about two hours) */
#define IRQ_KINDS       14u
#define R64CNT          (*(volatile sf_u8 *)0xA413FEC0u)
static sf_u32 g_sampled, g_ddi, g_last64, g_acc64;
static sf_u32 g_irq_code[IRQ_KINDS], g_irq_count[IRQ_KINDS];
static const char g_ddi_text[] = "CLASSPAD DDI: ";

static void log_raw(const char *s) { while (*s) dbt_flashlog_putc(*s++); }
static void log_hex(sf_u32 v)
{
    char t[9];
    int i;
    for (i = 0; i < 8; i++) t[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    t[8] = 0;
    log_raw(t);
}

static void log_heard(int c)
{
    static sf_u32 match;
    match = (c == g_ddi_text[match]) ? match + 1 : (c == g_ddi_text[0]);
    if (!g_ddi_text[match]) { g_ddi = 1; match = 0; }
}

static void log_stamp(void)
{
    /* RHRCNT, RMINCNT, RSECCNT of the SH7305 RTC (BCD) */
    sf_u32 t = (sf_u32)*(volatile sf_u8 *)0xA413FEC6u << 16 | (sf_u32)*(volatile sf_u8 *)0xA413FEC4u << 8 | *(volatile sf_u8 *)0xA413FEC2u;
    log_raw("[log] boot at "); log_hex(t); log_raw("\n");
}

void dbt_flashlog_irq(sf_u32 guest_pc, sf_u32 ssr, sf_u32 code)
{
    sf_u32 i, now;
    if (!g_log_on || (!g_ddi && g_sampled >= MAX_SAMPLES)) return;
    for (i = 0; i < IRQ_KINDS - 1 && g_irq_code[i] && g_irq_code[i] != code; i++) ;
    if (i == IRQ_KINDS - 1 && g_irq_code[i] != code) g_irq_code[i] = 0xFFFFFFFFu;      /* the rest */
    else g_irq_code[i] = code;
    g_irq_count[i]++;
    now = R64CNT & 0x7Fu;
    g_acc64 += (now - g_last64) & 0x7Fu;
    g_last64 = now;
    if (g_acc64 < (g_ddi ? RUN_64THS : SAMPLE_64THS) || g_log_busy || g_log_fill) return;    /* a line of the guest's is on its way: next time */
    g_acc64 = 0;
    log_raw("[log] pc "); log_hex(guest_pc); log_raw(" sr "); log_hex(ssr);
    log_raw(" at "); log_hex(code); log_raw(" pteh "); log_hex(*(volatile sf_u32 *)0xFF000000u); log_raw("\n");
    if (++g_sampled % REPORT) return;
    log_raw("[log] counts:");
    for (i = 0; i < IRQ_KINDS && g_irq_code[i]; i++) {
        log_raw(" "); log_hex(g_irq_code[i]); log_raw("x"); log_hex(g_irq_count[i]);
        g_irq_code[i] = g_irq_count[i] = 0;
    }
    log_raw("\n");
    /* the clock tree (FRQCRA, PLLCR, FLLFRQ) and the RTC's time: an
     * overclock that went away, and how long the counts above took */
    log_raw("[log] clock "); log_hex(*(volatile sf_u32 *)0xA4150000u);
    log_raw(" "); log_hex(*(volatile sf_u32 *)0xA4150024u);
    log_raw(" "); log_hex(*(volatile sf_u32 *)0xA4150050u);
    log_raw(" at "); log_hex((sf_u32)*(volatile sf_u8 *)0xA413FEC6u << 16 | (sf_u32)*(volatile sf_u8 *)0xA413FEC4u << 8 | *(volatile sf_u8 *)0xA413FEC2u);
    log_raw("\n");
}
#endif
