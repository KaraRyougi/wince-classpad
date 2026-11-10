/* loader.c - the Windows CE loader (wince.hh3) on the fx-CP400: a gint
 * program that HollyHock 3 starts from Casio's OS. It shows the Luna-style
 * screens of hhk/ui.c (docs/design/loader-luna.html) with four actions:
 *   - Flash CE Core OS: NK.bin into the NK window, the hole inside Casio's
 *     OS image, which Casio's boots leave alone (persistent);
 *   - Flash Additional Applications: EXT.bin into the EXT window, the
 *     eActivity area, which Casio's OS reformats on its next start;
 *   - both, NK first;
 *   - boot what is in the flash.
 * Files come from the storage memory through gint's file system (POSIX
 * open/read on /NK.bin, /EXT.bin: hhk/nkflash.c); a file identical to its
 * window is not written again (compared while the menu is up). Keys, the
 * display (gint's VRAM, drawn by hhk/gfx.c) and the clock are gint's; there
 * are no OS addresses in here. Then the fx-CP400 goes to the translator
 * image the ELF carries at PAYLOAD_BASE (payload.S): hhk/takeover.S masks
 * interrupts, quiets the interrupt sources, moves the image to IMAGE_BASE
 * and jumps; the translator finds the CE image in the NOR at NK_FLASH.
 * The payload is checked against the sum the build recorded (PAYLOAD_SUM/
 * PAYLOAD_SIZE from <build>/...-payload.h) so a truncated copy never runs.
 * The emulator's variant without OS or flashing is hhk/launcher.c. */
#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/rtc.h>
#include "util.h"
#include "nkflash.h"
#include "ui.h"

HHK_NAME("Windows CE");
HHK_DESCRIPTION("flashes fls0/NK.bin and EXT.bin if wanted, then boots CE through the translator");
HHK_AUTHOR("wince-classpad");
HHK_VERSION(HHK_APP_VERSION);

extern const u8 payload_start[], payload_end[];
extern void hhk_takeover(const void *src, void *dst, unsigned len, unsigned entry) __attribute__((noreturn));

#ifndef IMAGE_BASE
#error IMAGE_BASE (the translator link address) must come from the Makefile
#endif
#ifndef NK_FLASH
#define NK_FLASH 0x80EE0000u
#endif
#ifndef NK_FLASH_MAX
#define NK_FLASH_MAX 0x00500000u
#endif
#ifndef GUEST_RAM_HI
#error GUEST_RAM_HI must come from the Makefile
#endif

/* ROMHDR of an XIP image: pointer at image+0x44 (after "ECEC"), little-endian
 * fields dllfirst, dlllast, physfirst, physlast, nummods, ulRAMStart,
 * ulRAMFree, ulRAMEnd ... (tools/nkbin.py). 0 when there is no XIP image. */
static u32 ce_ram_end(void)
{
    const u8 *p = (const u8 *)NK_FLASH;
    u32 hdr;
    if (memcmp(p + 0x40, "ECEC", 4)) return 0;
    hdr = le32(p + 0x44);
    if (hdr < NK_FLASH || hdr > NK_FLASH + NK_FLASH_MAX - 0x60) return 0;
    return le32((const u8 *)hdr + 7 * 4);
}

/* --- the graphical loader --- */

static ui_scan scan;
static int win_of[2] = { -1, -1 };              /* nk_windows index of NK, EXT */

static void show(void) { dupdate(); }

/* nk_last without the flasher's "FAILED: " */
static const char *reason(void)
{
    return strncmp(nk_last, "FAILED: ", 8) ? nk_last : nk_last + 8;
}

/* the next key among EXE, Clear, Up, Down (arrows repeat when held) */
enum { K_EXE = 1, K_CLEAR, K_UP, K_DOWN };
static int next_key(int arrows)
{
    for (;;) {
        key_event_t e = getkey_opt(GETKEY_REP_ARROWS, NULL);
        if (e.key == KEY_EXE) return K_EXE;
        if (e.key == KEY_CLEAR) return K_CLEAR;
        if (arrows && e.key == KEY_UP) return K_UP;
        if (arrows && e.key == KEY_DOWN) return K_DOWN;
    }
}

static int wait_back(void)
{
    next_key(0);
    return 1;
}

static int error_screen(const char *title, const char *headline, const char *detail, const char *const *todo, int n)
{
    ui_error(title, headline, detail, todo, n);
    show();
    return wait_back();
}

/* first pass: which files are there and sane; second: do they differ from
 * their window (the menu says "checking..." meanwhile) */
static void scan_files(void)
{
    ui_file *f[2] = { &scan.nk, &scan.ext };
    int i, w, r;
    nkinfo info;
    for (i = 0; i < nk_nwindows && i < 2; i++)
        win_of[strncmp(nk_windows[i].name, "EXT", 4) ? 0 : 1] = i;
    scan.nk_base = NK_FLASH;
    scan.nk_window = win_of[0] >= 0 ? nkflash_window_state(&nk_windows[win_of[0]]) : "?";
    for (i = 0; i < 2; i++) {
        w = win_of[i];
        f[i]->state = UF_NONE; f[i]->size = 0; f[i]->why[0] = 0;
        if (w < 0) { f[i]->state = UF_NOREGION; continue; }
        r = nkflash_open(&nk_windows[w]);
        if (r == NKF_BAD) { f[i]->state = UF_BAD; memcpy(f[i]->why, reason(), sizeof f[i]->why); continue; }
        if (r == NKF_NOFILE) continue;
        nkflash_info(&info);
        f[i]->size = info.size;
        f[i]->state = UF_DIFF;
        nkflash_close();
    }
}

static void compare_files(void)
{
    ui_file *f[2] = { &scan.nk, &scan.ext };
    int i, r;
    for (i = 0; i < 2; i++) {
        if (f[i]->state != UF_DIFF) continue;
        if (nkflash_open(&nk_windows[win_of[i]]) != NKF_READY) { f[i]->state = UF_BAD; memcpy(f[i]->why, reason(), sizeof f[i]->why); continue; }
        r = nkflash_compare();
        nkflash_close();
        if (r < 0) { f[i]->state = UF_BAD; hhk_format(f[i]->why, sizeof f[i]->why, "cannot be read"); }
        else if (r == 1) f[i]->state = UF_SAME;
    }
}

static int default_action(void)
{
    int a = scan.nk.state == UF_DIFF ? (scan.ext.state == UF_DIFF ? UA_BOTH : UA_CORE)
          : scan.ext.state == UF_DIFF ? UA_APPS : UA_BOOT;
    return ui_action_enabled(&scan, a) ? a : UA_BOOT;
}

/* --- the flashing job --- */

enum { PH_ERASE, PH_WRITE, PH_VERIFY };
static struct {
    ui_job job;
    int step[2][3];                 /* job step of erase/write/verify per file, -1 */
    int file;                       /* 0 NK, 1 EXT: the file being written */
    int phase, record, nrecords;
    u32 units_done, units_total;    /* the whole job, for the second bar */
    u32 phase_units, phase_total;   /* units of the phase running */
    u32 t0;
    int full;                       /* redraw everything next time */
} J;

#define ERASE_UNITS 0x10000u        /* a sector's erase weighs about 64 KiB of programming */

static void mb(char *out, unsigned cap, u32 bytes)
{
    u32 tenth = (bytes * 10u + 524288u) / 1048576u;
    hhk_format(out, cap, "%u.%u MB", tenth / 10, tenth % 10);
}

#define DAY_TICKS (86400u * 128u)            /* rtc_ticks() counts 1/128 s from midnight */
static int seconds_left(u32 permille)
{
    u32 now = rtc_ticks(), el = (now + DAY_TICKS - J.t0) % DAY_TICKS;
    if (permille < 30 || el < 2 * 128) return -1;
    return (int)((el * (1000 - permille) / permille + 127) / 128);
}

static void redraw(const char *detail, u32 phase_done, u32 phase_total)
{
    u32 step = phase_total ? phase_done * 1000u / phase_total : 0;
    u32 all = J.units_total ? (J.units_done + J.phase_units) * 1000u / J.units_total : 1000;
    if (step > 1000) step = 1000;
    if (all > 1000) all = 1000;
    ui_flash(&J.job, J.full, detail, (int)step, (int)all, seconds_left(all));
    J.full = 0;
    show();
}

static void set_state(int step, int st)
{
    if (step >= 0) J.job.state[step] = st;
}

static void on_progress(int what, u32 done, u32 total, u32 addr)
{
    char d[64], a[16], b[16];
    if (what == NKP_RECORD) { J.record = (int)done; J.nrecords = (int)total; return; }
    if (what == NKP_ERASE) {
        J.phase_units = done * ERASE_UNITS;
        hhk_format(d, sizeof d, "Sector %u of %u at %08X", done < total ? done + 1 : total, total, addr);
        redraw(d, done, total);
        return;
    }
    if (what == NKP_WRITE && J.phase == PH_ERASE) {     /* the erase is over: writing starts */
        set_state(J.step[J.file][0], UI_DONE);
        set_state(J.step[J.file][1], UI_NOW);
        J.units_done += J.phase_total;
        J.phase = PH_WRITE; J.phase_total = total; J.full = 1;
    }
    J.phase_units = what == NKP_COMPARE ? done / 4 : done;
    mb(a, sizeof a, done); mb(b, sizeof b, total);
    if (what == NKP_COMPARE) hhk_format(d, sizeof d, "Comparing with flash " GFX_MIDDOT " %s of %s", a, b);
    else if (J.nrecords) hhk_format(d, sizeof d, "Record %u of %u " GFX_MIDDOT " %s of %s", (u32)J.record, (u32)J.nrecords, a, b);
    else hhk_format(d, sizeof d, "%s of %s", a, b);
    redraw(d, done, total);
}

static const char *const action_title[UA_COUNT] = {
    "Flashing CE Core OS", "Flashing Additional Applications", "Flashing Core OS + Applications", "Starting Windows CE",
};
static const char *const action_name[UA_COUNT] = {
    "Flash CE Core OS", "Flash Additional Applications", "Flash Core OS + Applications", "Boot Windows CE Directly",
};

static int add_step(const char *label, int st)
{
    int i = J.job.n++;
    hhk_format(J.job.label[i], sizeof J.job.label[i], "%s", label);
    J.job.state[i] = st;
    return i;
}

static const char *const file_name[2] = { "NK.bin", "EXT.bin" };

/* the steps for each file the action writes; -1 when a file went away */
static int plan(int action)
{
    ui_file *f[2] = { &scan.nk, &scan.ext };
    char l[40];
    int i;
    nkinfo info;
    memset(&J, 0, sizeof J);
    J.job.title = action_title[action];
    for (i = 0; i < 2; i++) {
        J.step[i][0] = J.step[i][1] = J.step[i][2] = -1;
        if ((i == 0 && action == UA_APPS) || (i == 1 && action == UA_CORE)) continue;
        if (f[i]->state == UF_SAME) {
            hhk_format(l, sizeof l, "%s already in flash", file_name[i]);
            add_step(l, UI_DONE);
            continue;
        }
        if (nkflash_open(&nk_windows[win_of[i]]) != NKF_READY) return -1;
        nkflash_info(&info);
        nkflash_close();
        hhk_format(l, sizeof l, "Check %s", file_name[i]);                 add_step(l, UI_DONE);
        hhk_format(l, sizeof l, "Erase %u flash sectors", info.sectors);   J.step[i][0] = add_step(l, UI_TODO);
        if (info.records) hhk_format(l, sizeof l, "Write %u records", info.records);
        else hhk_format(l, sizeof l, "Write %s", file_name[i]);
        J.step[i][1] = add_step(l, UI_TODO);
        hhk_format(l, sizeof l, "Verify %s", file_name[i]);                J.step[i][2] = add_step(l, UI_TODO);
        J.units_total += info.sectors * ERASE_UNITS + info.bytes + info.bytes / 4;
    }
    add_step("Start Windows CE", UI_TODO);
    return 0;
}

/* write file i (erase, program, compare again); 0, or which phase failed + 1 */
static int write_file(int i)
{
    nkinfo info;
    int r;
    if (J.step[i][0] < 0) return 0;               /* already in flash */
    J.file = i; J.phase = PH_ERASE; J.record = J.nrecords = 0;
    if (nkflash_open(&nk_windows[win_of[i]]) != NKF_READY) { set_state(J.step[i][0], UI_FAILED); return PH_ERASE + 1; }
    nkflash_info(&info);
    J.phase_total = info.sectors * ERASE_UNITS; J.phase_units = 0;
    set_state(J.step[i][0], UI_NOW); J.full = 1;
    redraw("Preparing" GFX_ELLIP, 0, 1);
    r = nkflash_write();
    nkflash_close();
    if (r) { set_state(J.step[i][J.phase == PH_ERASE ? 0 : 1], UI_FAILED); J.full = 1; redraw(reason(), 0, 1); return J.phase + 1; }
    set_state(J.step[i][1], UI_DONE);
    set_state(J.step[i][2], UI_NOW);
    J.units_done += info.bytes; J.phase = PH_VERIFY; J.phase_units = 0; J.phase_total = info.bytes / 4; J.full = 1;
    J.nrecords = 0;
    if (nkflash_open(&nk_windows[win_of[i]]) != NKF_READY) r = -1;
    else { r = nkflash_compare(); nkflash_close(); }
    if (r != 1) { set_state(J.step[i][2], UI_FAILED); J.full = 1; redraw("What was written does not match the file", 0, 1); return PH_VERIFY + 1; }
    set_state(J.step[i][2], UI_DONE);
    J.units_done += info.bytes / 4; J.phase_units = 0;
    return 0;
}

static int flash_failed(int action, int file, int phase)
{
    static char headline[64], detail[160], again[120], copy[80];
    const char *todo[3];
    const char *n = file_name[file];
    hhk_format(headline, sizeof headline, "%s", reason());
    if (headline[0] >= 'a' && headline[0] <= 'z') headline[0] -= 32;
    if (phase == PH_ERASE + 1)
        hhk_format(detail, sizeof detail, "Erasing the flash for %s stopped. Part of the image there may be gone, so it may not start.", n);
    else if (phase == PH_WRITE + 1)
        hhk_format(detail, sizeof detail, "Writing %s stopped part way, so the image in flash is incomplete.", n);
    else
        hhk_format(detail, sizeof detail, "%s was written, but the flash does not read back the same.", n);
    hhk_format(again, sizeof again, "Start wince.hh3 again and choose %s. %s is still in \\fls0.", action_name[action], n);
    hhk_format(copy, sizeof copy, "If it fails again, copy %s over USB once more.", n);
    todo[0] = "Press EXE to return to HollyHock.";
    todo[1] = again;
    todo[2] = copy;
    return error_screen("Flashing stopped", headline, detail, todo, 3);
}

static int ram_mismatch(void)
{
    static char detail[160];
    static const char *const todo[] = {
        "Press EXE to return to HollyHock.",
        "Rebuild the image with the current config.bib (RAM line) and copy NK.bin again.",
    };
    hhk_format(detail, sizeof detail, "Its RAM ends at %08X, but the translator starts at %08X.", ce_ram_end(), (u32)GUEST_RAM_HI);
    return error_screen("Image does not fit", "The CE image in flash is too large.", detail, todo, 2);
}

static int loader(void)
{
    int sel, k, i, r;
    gfx_fb = gint_vram;
    scan_files();
    sel = UA_BOOT;
    ui_menu(&scan, sel, 1); show();
    compare_files();
    sel = default_action();
    for (;;) {
        ui_menu(&scan, sel, 0); show();
        k = next_key(1);
        if (k == K_CLEAR) return 0;
        if (k == K_UP || k == K_DOWN) {
            for (i = 0; i < UA_COUNT; i++) {
                sel = (sel + (k == K_UP ? UA_COUNT - 1 : 1)) % UA_COUNT;
                if (ui_action_enabled(&scan, sel)) break;
            }
            continue;
        }
        if (ui_action_enabled(&scan, sel)) break;
    }
    if (sel == UA_BOOT || sel == UA_APPS) {
        if (ce_ram_end() > GUEST_RAM_HI) return ram_mismatch();
    }
    if (sel != UA_BOOT) {
        if (plan(sel)) {
            static const char *const todo[] = { "Press EXE to return to HollyHock.", "Check the files in \\fls0 and start wince.hh3 again." };
            return error_screen("Flashing stopped", "A file changed after the check.", reason(), todo, 2);
        }
        nk_progress = on_progress;
        J.t0 = rtc_ticks();
        J.full = 1;
        for (i = 0; i < 2; i++) {
            r = write_file(i);
            if (r) { nk_progress = 0; return flash_failed(sel, i, r); }
        }
        nk_progress = 0;
        if (ce_ram_end() > GUEST_RAM_HI) return ram_mismatch();
        set_state(J.job.n - 1, UI_NOW); J.full = 1; J.phase_units = 0;
        redraw("Handing over to the translator", 1, 1);
    }
    ui_boot(); show();
    return -1;                                    /* boot */
}

int main(void)
{
    u32 len = (u32)(payload_end - payload_start);
    int r;
    gfx_fb = gint_vram;
    if (len != PAYLOAD_SIZE || bytesum(payload_start, len) != PAYLOAD_SUM) {
        static const char *const todo[] = { "Press EXE to return to HollyHock.", "Copy wince.hh3 to the calculator again." };
        return error_screen("Loader damaged", "wince.hh3 is incomplete.",
                            "The translator inside it does not match the sum the build recorded, so it is not started.", todo, 2);
    }
    r = loader();
    if (r >= 0) return r;
    hhk_takeover(payload_start, (void *)IMAGE_BASE, len, IMAGE_BASE);
}
