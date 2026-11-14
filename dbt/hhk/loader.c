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
 * open/read on /NK.bin, /EXT.bin: hhk/nkflash.c), as Platform Builder wrote
 * them or packed by tools/cepack.py (hhk/cez.c); a file identical to its
 * window is not written again (compared while the menu is up). Keys, the
 * display (gint's VRAM, drawn by hhk/gfx.c) and the clock are gint's; there
 * are no OS addresses in here. Then the fx-CP400 goes to the translator
 * image the ELF carries at PAYLOAD_BASE (payload.S): hhk/takeover.S masks
 * interrupts, quiets the interrupt sources, moves the image to IMAGE_BASE
 * and jumps; the translator finds the CE image in the NOR at NK_FLASH.
 * The payload is checked against the sum the build recorded (PAYLOAD_SUM/
 * PAYLOAD_SIZE from <build>/...-payload.h) so a truncated copy never runs.
 * The emulator's variant without OS or flashing is hhk/launcher.c. */
#include <gint/gint.h>
#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/rtc.h>
#include <gint/clock.h>
#include <fcntl.h>
#include <unistd.h>
#include "bootmarquee.h"
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

/* input: keys and the touch screen, both from gint's event queue. A tap
 * counts when the stylus lifts on the control it went down on, as Windows
 * buttons do; meanwhile the button under it is drawn pressed (ui_pressed)
 * and comes back up if the stylus slides off. EXE and Clear press the
 * screen's button for a moment too, so every action shows where it came
 * from. */
enum { K_NONE, K_EXE, K_CLEAR, K_UP, K_DOWN, K_PRESS, K_DRAG, K_TAP };
static int tap_x, tap_y, down_x = -1, down_y = -1, cur_x, cur_y;

static int read_input(int block)
{
    for (;;) {
        key_event_t e = block ? waitevent(NULL) : pollevent();
        if (e.type == KEYEV_NONE) return K_NONE;
        if (e.type == KEYEV_DOWN) {
            if (e.key == KEY_EXE) return K_EXE;
            if (e.key == KEY_CLEAR) return K_CLEAR;
            if (e.key == KEY_UP) return K_UP;
            if (e.key == KEY_DOWN) return K_DOWN;
        } else if (e.type == KEYEV_TOUCH_DOWN) {
            down_x = cur_x = e.x; down_y = cur_y = e.y;
            return K_PRESS;
        } else if (e.type == KEYEV_TOUCH_DRAG) {
            cur_x = e.x; cur_y = e.y;
            return K_DRAG;
        } else if (e.type == KEYEV_TOUCH_UP) {
            tap_x = e.x; tap_y = e.y;
            if (down_x < 0) { down_x = tap_x; down_y = tap_y; }
            return K_TAP;
        }
    }
}

/* which button the stylus holds down: the control under it, if that is a
 * button and the one it went down on (hit = the screen's hit test) */
static int held_button(int (*hit)(int, int))
{
    int h = hit(cur_x, cur_y);
    if (h < UI_HIT_START || h != hit(down_x, down_y)) return UI_HIT_NONE;
    return h;
}

/* the error screen being shown, for redrawing it with a button pressed */
static struct { const char *title, *headline, *detail; const char *const *todo; int n; } E;
static void draw_error(void) { ui_error(E.title, E.headline, E.detail, E.todo, E.n); show(); }

static void flash_button(int which, void (*draw)(void))
{
    ui_pressed = which; draw();
    sleep_us(120000);
    ui_pressed = UI_HIT_NONE; draw();
}

/* a tap on the error screen's button or close box, or EXE / Clear */
static int wait_back(void)
{
    for (;;) {
        int k = read_input(1), was = ui_pressed;
        if (k == K_EXE || k == K_CLEAR) { flash_button(UI_HIT_BACK, draw_error); return 1; }
        if (k == K_PRESS || k == K_DRAG) ui_pressed = held_button(ui_error_hit);
        if (k == K_TAP) {
            int h = ui_error_hit(tap_x, tap_y);
            ui_pressed = UI_HIT_NONE;
            if (h != UI_HIT_NONE && h == ui_error_hit(down_x, down_y)) { draw_error(); return 1; }
        }
        if (ui_pressed != was) draw_error();
    }
}

static int error_screen(const char *title, const char *headline, const char *detail, const char *const *todo, int n)
{
    E.title = title; E.headline = headline; E.detail = detail; E.todo = todo; E.n = n;
    ui_pressed = UI_HIT_NONE;
    draw_error();
    return wait_back();
}

/* first pass: which files are there and sane; second: do they differ from
 * their window (the menu says "checking..." meanwhile) */
/* what writing each file will do, from the scan: the plan is drawn from this
 * without opening the files again (an open walks every record of the file,
 * seconds on Casio's file system, with the menu still on the screen) */
static nkinfo finfo[2];

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
        finfo[i] = info;
        f[i]->size = info.size;
        f[i]->state = UF_DIFF;
        nkflash_close();
    }
}

/* the menu while it waits: the selection, whether the user moved it, and
 * what was asked for while the files were still being compared */
static int sel, moved, asked;              /* asked: 0, UA_* + 1, or -1 cancel */

static void menu_move(int dir)
{
    int i;
    for (i = 0; i < UA_COUNT; i++) {
        sel = (sel + (dir < 0 ? UA_COUNT - 1 : 1)) % UA_COUNT;
        if (ui_action_enabled(&scan, sel)) break;
    }
    moved = 1;
}

static int checking;                       /* the files are still being compared */
static void draw_menu(void) { ui_menu(&scan, sel, checking); show(); }

/* one input on the menu: 0 nothing to do, 2 redraw, 1 start sel, -1 cancel */
static int menu_input(int k)
{
    int hit, was = ui_pressed;
    if (k == K_CLEAR) { flash_button(UI_HIT_CANCEL, draw_menu); return -1; }
    if (k == K_EXE) {
        if (!ui_action_enabled(&scan, sel)) return 0;
        flash_button(UI_HIT_START, draw_menu);
        return 1;
    }
    if (k == K_UP || k == K_DOWN) { menu_move(k == K_UP ? -1 : 1); return 2; }
    if (k == K_PRESS || k == K_DRAG) {
        ui_pressed = held_button(ui_menu_hit);
        return ui_pressed != was ? 2 : 0;
    }
    if (k != K_TAP) return 0;
    ui_pressed = UI_HIT_NONE;
    hit = ui_menu_hit(tap_x, tap_y);
    if (hit != ui_menu_hit(down_x, down_y)) return was != UI_HIT_NONE ? 2 : 0;   /* slid off the control */
    if (hit >= 0 && hit < UA_COUNT) {
        if (ui_action_enabled(&scan, hit) && hit != sel) { sel = hit; moved = 1; return 2; }
        return was != UI_HIT_NONE ? 2 : 0;
    }
    if (hit == UI_HIT_START && ui_action_enabled(&scan, sel)) { draw_menu(); return 1; }
    if (hit == UI_HIT_CANCEL || hit == UI_HIT_CLOSE) { draw_menu(); return -1; }
    return was != UI_HIT_NONE ? 2 : 0;
}

/* called back by nkflash_compare every 32 KiB: the menu stays live. Cancel
 * stops the comparison at once, so does Boot (it needs no comparison);
 * the other actions wait for it to finish, with the Start button held down
 * so that the tap is seen to have been taken */
static void compare_poll(int what, u32 done, u32 total, u32 addr)
{
    int k, r, redraw_menu = 0;
    (void)what; (void)done; (void)total; (void)addr;
    while ((k = read_input(0)) != K_NONE) {
        if (asked > 0) {
            /* Start has been taken and waits for the comparison: its button
             * stays down, the selection is fixed, only Cancel still counts */
            if (k == K_CLEAR) r = -1;
            else if (k == K_TAP) {
                r = ui_menu_hit(tap_x, tap_y);
                r = (r == ui_menu_hit(down_x, down_y) && (r == UI_HIT_CANCEL || r == UI_HIT_CLOSE)) ? -1 : 0;
            } else r = 0;
            if (r < 0) { asked = -1; nk_abort = 1; }
            continue;
        }
        r = menu_input(k);
        if (r == 2) redraw_menu = 1;
        if (r < 0) { asked = -1; nk_abort = 1; }
        else if (r == 1) {
            asked = sel + 1;
            if (sel == UA_BOOT) nk_abort = 1;
            else { ui_pressed = UI_HIT_START; redraw_menu = 1; }   /* seen to be taken until the comparison is over */
        }
    }
    if (redraw_menu && !nk_abort) draw_menu();
}

static void compare_files(void)
{
    ui_file *f[2] = { &scan.nk, &scan.ext };
    int i, r;
    nk_progress = compare_poll;
    for (i = 0; i < 2 && !nk_abort; i++) {
        if (f[i]->state != UF_DIFF) continue;
        if (nkflash_open(&nk_windows[win_of[i]]) != NKF_READY) { f[i]->state = UF_BAD; memcpy(f[i]->why, reason(), sizeof f[i]->why); continue; }
        r = nkflash_compare();
        nkflash_close();
        if (r == -2) break;                                   /* cancelled or Boot: left unknown */
        if (r < 0) { f[i]->state = UF_BAD; hhk_format(f[i]->why, sizeof f[i]->why, "cannot be read"); }
        else if (r == 1) f[i]->state = UF_SAME;
    }
    nk_progress = 0;
    nk_abort = 0;
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
    u32 units_done, units_total;    /* the whole job, for the second bar, in KiB-equivalents */
    u32 phase_units, phase_total;   /* units of the phase running */
    u32 shown_step, shown_all;      /* what the bars show: never back, within a step */
    int full;                       /* redraw everything next time */
} J;

/* The second bar weighs the job's parts by how long they take, in units of
 * 1 KiB programmed: a 128 KiB sector erase weighs ERASE_UNITS, a KiB written
 * 1, a KiB compared VERIFY_DIV-th of that. Everything is counted in KiB so
 * that "x 1000" for the per-mille stays far below 2^32 (counting in bytes it
 * overflowed past 4.29 MB, which is what scrambled the bars). */
#define ERASE_UNITS 64u
#define VERIFY_DIV  4u
#define KIB(b)      (((b) + 1023u) >> 10)

static u32 permille(u32 done, u32 total)
{
    if (!total) return 1000;
    if (done >= total) return 1000;
    while (total > 4000000u) { total >>= 1; done >>= 1; }   /* keeps done * 1000 in range */
    return done * 1000u / total;
}

static void mb(char *out, unsigned cap, u32 bytes)
{
    u32 tenth = (bytes * 10u + 524288u) / 1048576u;
    hhk_format(out, cap, "%u.%u MB", tenth / 10, tenth % 10);
}

#define DAY_TICKS (86400u * 128u)            /* rtc_ticks() counts 1/128 s from midnight */

static void redraw(const char *detail, u32 phase_done, u32 phase_total)
{
    u32 step = permille(phase_done, phase_total);
    u32 all = permille(J.units_done + J.phase_units, J.units_total);
    if (J.full) J.shown_step = 0;                   /* a new step starts its bar again */
    if (step < J.shown_step) step = J.shown_step;
    if (all < J.shown_all) all = J.shown_all;
    J.shown_step = step; J.shown_all = all;
    ui_flash(&J.job, J.full, detail, (int)step, (int)all);
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
        J.phase = PH_WRITE; J.phase_total = KIB(total); J.full = 1;
    }
    J.phase_units = what == NKP_COMPARE ? KIB(done) / VERIFY_DIV : KIB(done);
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

/* the steps for each file the action writes */
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
        info = finfo[i];                    /* a file that changed since is caught when it is opened to be written */
        hhk_format(l, sizeof l, "Check %s", file_name[i]);                 add_step(l, UI_DONE);
        hhk_format(l, sizeof l, "Erase %u flash sectors", info.sectors);   J.step[i][0] = add_step(l, UI_TODO);
        if (info.records) hhk_format(l, sizeof l, "Write %u records", info.records);
        else hhk_format(l, sizeof l, "Write %s", file_name[i]);
        J.step[i][1] = add_step(l, UI_TODO);
        hhk_format(l, sizeof l, "Verify %s", file_name[i]);                J.step[i][2] = add_step(l, UI_TODO);
        J.units_total += info.sectors * ERASE_UNITS + KIB(info.bytes) + KIB(info.bytes) / VERIFY_DIV;
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
    /* the screen first: opening the file walks its records, which takes a while */
    J.phase_total = finfo[i].sectors * ERASE_UNITS; J.phase_units = 0;
    set_state(J.step[i][0], UI_NOW); J.full = 1;
    redraw("Preparing" GFX_ELLIP, 0, 1);
    if (nkflash_open(&nk_windows[win_of[i]]) != NKF_READY) { set_state(J.step[i][0], UI_FAILED); J.full = 1; redraw(reason(), 0, 1); return PH_ERASE + 1; }
    nkflash_info(&info);
    J.phase_total = info.sectors * ERASE_UNITS;
    r = nkflash_write();
    nkflash_close();
    if (r) { set_state(J.step[i][J.phase == PH_ERASE ? 0 : 1], UI_FAILED); J.full = 1; redraw(reason(), 0, 1); return J.phase + 1; }
    set_state(J.step[i][1], UI_DONE);
    set_state(J.step[i][2], UI_NOW);
    J.units_done += KIB(info.bytes); J.phase = PH_VERIFY; J.phase_units = 0; J.phase_total = KIB(info.bytes) / VERIFY_DIV; J.full = 1;
    J.nrecords = 0;
    redraw("Preparing" GFX_ELLIP, 0, 1);
    if (nkflash_open(&nk_windows[win_of[i]]) != NKF_READY) r = -1;
    else { r = nkflash_compare(); nkflash_close(); }
    if (r != 1) { set_state(J.step[i][2], UI_FAILED); J.full = 1; redraw("What was written does not match the file", 0, 1); return PH_VERIFY + 1; }
    set_state(J.step[i][2], UI_DONE);
    J.units_done += KIB(info.bytes) / VERIFY_DIV; J.phase_units = 0;
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

/* The window is neither blank nor recognisably ours (nk_refused): what a
 * flash of ours that was cut off leaves behind. EXE erases it and goes on;
 * anything else leaves. 1 = go on. */
static int confirm_erase(int file)
{
    static char headline[64], detail[200];
    static const char *const todo[] = {
        "Press EXE to erase this area and write the image.",
        "Press Clear, or tap the button, to leave it as it is.",
        "Only go on if Windows CE was flashed on this calculator before.",
    };
    hhk_format(headline, sizeof headline, "%s", reason());
    if (headline[0] >= 'a' && headline[0] <= 'z') headline[0] -= 32;
    hhk_format(detail, sizeof detail, "The flash area for %s is not empty and does not hold a complete Windows CE image. "
               "An earlier flashing that was cut off (a reset, the batteries) leaves it like this.", file_name[file]);
    E.title = "Flash area not empty"; E.headline = headline; E.detail = detail; E.todo = todo; E.n = 3;
    ui_pressed = UI_HIT_NONE;
    draw_error();
    for (;;) {
        int k = read_input(1), was = ui_pressed;
        if (k == K_EXE) return 1;
        if (k == K_CLEAR) { flash_button(UI_HIT_BACK, draw_error); return 0; }
        if (k == K_PRESS || k == K_DRAG) ui_pressed = held_button(ui_error_hit);
        if (k == K_TAP) {
            int h = ui_error_hit(tap_x, tap_y);
            ui_pressed = UI_HIT_NONE;
            if (h != UI_HIT_NONE && h == ui_error_hit(down_x, down_y)) { draw_error(); return 0; }
        }
        if (ui_pressed != was) draw_error();
    }
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

/* the start screen fades in from black (about half a second) with its
 * marquee already moving; the translator keeps it moving (rt/bootmarquee.c) */
static void boot_screen(void)
{
    int lvl, pos = 0;
    u32 t = rtc_ticks();
    for (lvl = 0; lvl <= 256; lvl += 16) {
        u32 now = rtc_ticks();
        if ((now + DAY_TICKS - t) % DAY_TICKS >= BM_FRAME_TICKS) { t = now; pos = (pos + 1) % BM_POSITIONS; }
        gfx_level = lvl;
        ui_boot(pos);
        show();
        sleep_us(20000);
    }
    gfx_level = 256;
}

static int loader(void)
{
    int i, r;
    gfx_fb = gint_vram;
    scan_files();
    sel = default_action();                /* provisional: before the comparison every file "differs" */
    checking = 1;
    draw_menu();
    compare_files();
    checking = 0;
    ui_pressed = UI_HIT_NONE;
    if (asked < 0) return 0;
    if (!moved) sel = default_action();
    if (asked > 0 && ui_action_enabled(&scan, asked - 1)) sel = asked - 1;
    else {
        draw_menu();
        for (;;) {
            r = menu_input(read_input(1));
            if (r < 0) return 0;
            if (r == 1) break;
            if (r == 2) draw_menu();
        }
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
        J.full = 1;
        for (i = 0; i < 2; i++) {
            r = write_file(i);
            if (r && nk_refused && confirm_erase(i)) {
                nk_force = 1;
                r = write_file(i);
                nk_force = 0;
            }
            if (r) { nk_progress = 0; return flash_failed(sel, i, r); }
        }
        nk_progress = 0;
        if (ce_ram_end() > GUEST_RAM_HI) return ram_mismatch();
        set_state(J.job.n - 1, UI_NOW); J.full = 1; J.phase_units = 0;
        redraw("Handing over to the translator", 1, 1);
    }
    boot_screen();
    return -1;                                    /* boot */
}

/* What the last CE session logged, for a boot that never reached the
 * desktop: a translator built with FLASHLOG=1 writes the boot's debug output
 * (the OAL's and the drivers' lines, its own) as text into the last sector
 * of the NK window (rt/nor.c "boot log"). When there is text there, it goes
 * to \fls0\WINCE.LOG, unless the file already has that length (the same
 * log, saved by an earlier start). */
#define LOG_TEXT    ((const u8 *)(NK_FLASH + NK_FLASH_MAX - 0x20000u))
#define LOG_MAX     0x1F000u
static void save_last_log(void)
{
    u32 n, i;
    int fd;
    for (n = 0; n < LOG_MAX && LOG_TEXT[n] != 0xFF; n++) ;
    {   /* a note the translator left behind the text (after up to 63 blank bytes) */
        u32 m = (n + 63u) & ~63u, e;
        if (m > n && m < 0x1F000u && LOG_TEXT[m] != 0xFF) {
            for (e = m; e < 0x1F000u && LOG_TEXT[e] != 0xFF; e++) ;
            n = e;
        }
    }
    if (n < 16) return;
    for (i = 0; i < 16; i++)                    /* text, not the tail of an image */
        if ((LOG_TEXT[i] < 0x20 || LOG_TEXT[i] > 0x7E) && LOG_TEXT[i] != '\r' && LOG_TEXT[i] != '\n') return;
    /* never over a file that is there (truncating one is not something
     * the file system here is known to do): WINCE.LOG, then WINCE2.LOG ..
     * WINCE9.LOG; nothing when the newest of them has this length already */
    {
        char name[16];
        u32 k;
        off_t have = -1;
        for (k = 1; k <= 9; k++) {
            if (k == 1) hhk_format(name, sizeof name, "/WINCE.LOG");
            else hhk_format(name, sizeof name, "/WINCE%u.LOG", k);
            fd = open(name, O_RDONLY);
            if (fd < 0) break;
            have = lseek(fd, 0, SEEK_END);
            close(fd);
        }
        if (k > 9 || have == (off_t)n) return;
        fd = open(name, O_WRONLY | O_CREAT);
        if (fd < 0) return;
        write(fd, LOG_TEXT, n);
        close(fd);
    }
}

static u32 payload_len;
static int takeover_from_os(void)
{
    hhk_takeover(payload_start, (void *)IMAGE_BASE, payload_len, IMAGE_BASE);
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
    save_last_log();
    r = loader();
    if (r >= 0) return r;
    /* Hand over with the hardware as Casio's OS had it, not as gint has it:
     * gint_world_switch puts every driver's state back (timers and the
     * extra timers, RTC, INTC, CPG, PASCR/IRMCR, CPUOPM.INTMU, the DMA and
     * I2C modules it powered, the touch sensor's set-up) before it calls
     * out. The translator, the OAL's standby and the drivers that read a
     * device's set-up back (touch, backlight) were all brought up from the
     * OS's state, the state a HollyHock program is started in. Started
     * straight from gint's world (until 2026-11-10) CE ran, but the first
     * build to do so did not come back from a suspend on the calculator.
     * The call never returns: takeover.S masks interrupts first thing. */
    payload_len = len;
    gint_world_switch(GINT_CALL(takeover_from_os));
    for (;;) ;
}
