/* ui.c - see ui.h. Coordinates and colours are the mockup's
 * (docs/design/loader-luna.html: drawMenu, drawFlash, drawBoot, drawError). */
#include "ui.h"

extern const gfx_image art_chip, art_box, art_both, art_go, art_warn, art_err,
    art_radio_on, art_radio_off, art_radio_dis_on, art_radio_dis_off,
    art_step_done, art_step_now, art_step_todo, art_step_fail, art_close,
    art_flag17, art_flag36, art_lettering;

#define W GFX_W
#define H GFX_H

/* the Luna palette (mockup P) */
#define C_FACE      0xECE9D8
#define C_WHITE     0xFFFFFF
#define C_INK       0x000000
#define C_TEXT2     0x4B4B4B
#define C_GREY      0xA7A6AA
#define C_EDGE      0xACA899
#define C_SELECT    0x316AC5
#define C_SELSOFT   0xDCE8FC
#define C_BTNBORDER 0x003C74
#define C_RED       0xD8331F
#define C_GROUP     0xD0D0BF
#define C_GROUPTXT  0x0046D5

static void hline(int x, int y, int w, u32 c) { gfx_fill(x, y, w, 1, RGB(c)); }

static void title_bar(const char *label)
{
    static const gfx_stop rows[] = { { 0, 0x3D95FF }, { 31, 0x0058EE }, { 191, 0x0058EE }, { 230, 0x0560E8 }, { 255, 0x00309C } };
    gfx_rows(0, 0, W, 28, rows, 5);
    gfx_image_draw(8, 7, &art_flag17, 0);
    gfx_text(&font_tr13b, 32, 20, label, RGB(0x001A63));   /* the mockup's 1 px drop shadow */
    gfx_text(&font_tr13b, 31, 19, label, RGB(C_WHITE));
    gfx_image_draw(W - 27, 4, &art_close, 0);
}

static void header(const char *title, const char *sub, const gfx_image *icon)
{
    gfx_fill(0, 28, W, 58, RGB(C_WHITE));
    hline(0, 86, W, C_EDGE);
    hline(0, 87, W, C_WHITE);
    gfx_text(&font_t12b, 14, 50, title, RGB(C_INK));
    gfx_wrap(&font_t11, 14, 67, W - 74, 13, sub, RGB(C_TEXT2));
    if (icon) gfx_image_draw(W - 46, 38, icon, 0);
}

static void footer_line(void)
{
    hline(0, H - 50, W, C_EDGE);
    hline(0, H - 49, W, C_WHITE);
}

static void button(int x, int y, int w, const char *label, int def, int disabled)
{
    static const gfx_stop face[] = { { 0, 0xFFFFFF }, { 217, 0xECEBE6 }, { 255, 0xD6D0C5 } };
    const int h = 23;
    if (disabled) gfx_rrect(x, y, w, h, 3, RGB(0xF5F4EA));
    else gfx_rrect_rows(x, y, w, h, 3, face, 3);
    gfx_rrect_line(x, y, w, h, 3, RGB(disabled ? 0xC9C7BA : C_BTNBORDER));
    if (def && !disabled) {
        gfx_rrect_line(x + 1, y + 1, w - 2, h - 2, 2, RGB(0x8CB0EF));
        gfx_rrect_line(x + 2, y + 2, w - 4, h - 4, 2, RGB(0xCEE7FF));
    }
    gfx_text_align(&font_t11, x + w / 2, y + 16, label, RGB(disabled ? C_GREY : C_INK), GFX_CENTER);
}

static void group_box(int x, int y, int w, int h, const char *label)
{
    int lw = gfx_text_width(&font_t11, label);
    gfx_rrect_line(x, y, w, h, 3, RGB(C_GROUP));
    gfx_fill(x + 7, y - 6, lw + 6, 12, RGB(C_FACE));
    gfx_text(&font_t11, x + 10, y + 4, label, RGB(C_GROUPTXT));
}

enum { TAG_PER, TAG_NON, TAG_BOTH, TAG_NONE };
static int tag(int x, int y, const char *label, int kind)
{
    static const u32 c[4][3] = {
        { 0xE3F6E0, 0x2E7D2A, 0x9ACD92 }, { 0xFFF1D6, 0x9A5800, 0xF0C27A },
        { 0xE7EEFC, 0x264C9E, 0xA9BEE8 }, { 0xF1F1EC, 0x5A5A5A, 0xCFCDC2 },
    };
    int w = gfx_text_width(&font_t9b, label) + 10;
    gfx_rrect(x, y, w, 13, 6, RGB(c[kind][0]));
    gfx_rrect_line(x, y, w, 13, 6, RGB(c[kind][2]));
    gfx_text(&font_t9b, x + 5, y + 10, label, RGB(c[kind][1]));
    return w;
}

/* --- menu --- */

static const struct {
    const char *title, *tag, *desc;
    int kind, need_nk, need_ext;
    const gfx_image *icon;
} actions[UA_COUNT] = {
    { "Flash CE Core OS", "Persistent", "Writes NK.bin into flash, then starts Windows CE. Kept until you flash again.", TAG_PER, 1, 0, &art_chip },
    { "Flash Additional Applications", "Non-persistent", "Writes EXT.bin to the eActivity area. Casio OS clears it on its next start.", TAG_NON, 0, 1, &art_box },
    { "Flash Core OS + Applications", "Both", "Writes NK.bin, then EXT.bin, then starts Windows CE.", TAG_BOTH, 1, 1, &art_both },
    { "Boot Windows CE Directly", "Writes nothing", "Starts the image already in flash. Nothing is changed.", TAG_NONE, 0, 0, &art_go },
};

static int usable(const ui_file *f) { return f->state == UF_SAME || f->state == UF_DIFF; }

int ui_action_enabled(const ui_scan *s, int a)
{
    if (actions[a].need_nk && !usable(&s->nk)) return 0;
    if (actions[a].need_ext && !usable(&s->ext)) return 0;
    return 1;
}

/* why an action cannot run, from the first file it lacks */
static void why_not(const ui_scan *s, int a, char *out, unsigned cap)
{
    const ui_file *f = (actions[a].need_nk && !usable(&s->nk)) ? &s->nk : &s->ext;
    const char *name = f == &s->nk ? "NK.bin" : "EXT.bin";
    if (f->state == UF_NOREGION) hhk_format(out, cap, "This build has no EXT region");
    else if (f->state == UF_BAD) hhk_format(out, cap, "%s rejected: %s", name, f->why);
    else if (actions[a].need_nk && actions[a].need_ext && !usable(&s->nk) && !usable(&s->ext))
        hhk_format(out, cap, "NK.bin and EXT.bin not found in \\fls0");
    else hhk_format(out, cap, "%s not found in \\fls0", name);
}

static void size_text(char *out, unsigned cap, u32 bytes)
{
    u32 tenth = (bytes * 10u + 524288u) / 1048576u;          /* MB to one decimal */
    hhk_format(out, cap, "%u.%u MB", tenth / 10, tenth % 10);
}

static void file_line(int y, const char *label, const ui_file *f, int checking)
{
    char v[48], sz[16];
    u32 c = C_INK;
    gfx_text(&font_t11, 18, y, label, RGB(C_TEXT2));
    size_text(sz, sizeof sz, f->size);
    switch (f->state) {
    case UF_NONE:     hhk_format(v, sizeof v, "not found"); c = C_GREY; break;
    case UF_NOREGION: hhk_format(v, sizeof v, "no EXT region in this build"); c = C_GREY; break;
    case UF_BAD:      hhk_format(v, sizeof v, "rejected"); c = C_RED; break;
    case UF_SAME:     hhk_format(v, sizeof v, "%s " GFX_MIDDOT " already in flash", sz); c = 0x2E7D2A; break;
    default:
        if (checking) { hhk_format(v, sizeof v, "%s " GFX_MIDDOT " checking" GFX_ELLIP, sz); c = C_TEXT2; }
        else { hhk_format(v, sizeof v, "%s " GFX_MIDDOT " differs from flash", sz); c = 0x9A5800; }
    }
    gfx_text(&font_t11, 108, y, v, RGB(c));
}

void ui_menu(const ui_scan *s, int sel, int checking)
{
    static const gfx_stop selrows[] = { { 0, C_WHITE }, { 255, C_SELSOFT } };
    const int top = 96, ih = 70;
    char buf[64];
    int i, gy, kx, dx;
    gfx_fill(0, 0, W, H, RGB(C_FACE));
    title_bar("Windows CE Loader");
    header("Choose how to start", "Select an action with " GFX_UP " " GFX_DOWN ", then press EXE.", &art_chip);
    for (i = 0; i < UA_COUNT; i++) {
        int y = top + i * (ih + 4), dis = !ui_action_enabled(s, i), on = i == sel;
        if (on) {
            gfx_rrect_rows(6, y, W - 12, ih, 4, selrows, 2);
            gfx_rrect_line(6, y, W - 12, ih, 4, RGB(C_SELECT));
        }
        gfx_image_draw(14, y + 12, dis ? (on ? &art_radio_dis_on : &art_radio_dis_off) : (on ? &art_radio_on : &art_radio_off), 0);
        gfx_image_draw(actions[i].icon == &art_both ? 29 : 32, y + (actions[i].icon == &art_both ? 4 : 7), actions[i].icon, dis);
        gfx_text(&font_t12b, 72, y + 19, actions[i].title, RGB(dis ? C_GREY : C_INK));
        tag(72, y + 25, actions[i].tag, dis ? TAG_NONE : actions[i].kind);
        if (dis) {
            why_not(s, i, buf, sizeof buf);
            gfx_wrap(&font_t11, 72, y + 53, W - 86, 13, buf, RGB(C_RED));
        } else {
            gfx_wrap(&font_t11, 72, y + 52, W - 86, 13, actions[i].desc, RGB(on ? C_INK : C_TEXT2));
        }
    }
    gy = top + UA_COUNT * (ih + 4) + 10;
    group_box(8, gy, W - 16, 68, "Status");
    gfx_text(&font_t11, 18, gy + 21, "Flash window", RGB(C_TEXT2));
    hhk_format(buf, sizeof buf, "%08X " GFX_MIDDOT " %s", s->nk_base, s->nk_window);
    gfx_text(&font_t11, 108, gy + 21, buf, RGB(C_INK));
    file_line(gy + 38, "NK.bin", &s->nk, checking);
    file_line(gy + 55, "EXT.bin", &s->ext, checking);
    footer_line();
    button(W - 170, H - 40, 76, sel == UA_BOOT ? "Boot" : "Start", 1, !ui_action_enabled(s, sel));
    button(W - 86, H - 40, 76, "Cancel", 0, 0);
    kx = 12;
    dx = gfx_text_width(&font_t11b, "EXE");
    if (gfx_text_width(&font_t11b, "Clear") > dx) dx = gfx_text_width(&font_t11b, "Clear");
    dx += kx + 6;
    gfx_text(&font_t11b, kx, H - 31, "EXE", RGB(C_TEXT2));
    gfx_text(&font_t11, dx, H - 31, "start", RGB(C_TEXT2));
    gfx_text(&font_t11b, kx, H - 19, "Clear", RGB(C_TEXT2));
    gfx_text(&font_t11, dx, H - 19, "back to HollyHock", RGB(C_TEXT2));
}

/* --- flashing --- */

#define CHUNK_W 6
#define CHUNK_GAP 2
static const u32 green_rows[8] = { 0xACEDAD, 0x7BE47D, 0x4CDA50, 0x2ED330, 0x42D845, 0x76E275, 0x8FE791, 0xC6F2C6 };
static const u32 blue_rows[8]  = { 0xB9CEFF, 0x86A6F6, 0x5A82F0, 0x3762E2, 0x2F5CE0, 0x4A71E6, 0x7C9BEE, 0xAFC3F6 };
#define BAR_W(n) (6 + (n) * (CHUNK_W + CHUNK_GAP) - CHUNK_GAP)

static void chunk(int x, int y, const u32 *rows)
{
    int r;
    for (r = 0; r < 8; r++) gfx_fill(x, y + r, CHUNK_W, 1, RGB(rows[r]));
}

static void progress(int x, int y, int w, int permille)
{
    int n = (w - 6 + CHUNK_GAP) / (CHUNK_W + CHUNK_GAP), lit, i;
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    lit = (n * permille + 500) / 1000;
    gfx_rrect(x, y, w, 14, 3, RGB(C_WHITE));
    gfx_rrect_line(x, y, w, 14, 3, RGB(0x8E8F8F));
    hline(x + 2, y + 1, w - 4, 0xE7E7E7);
    for (i = 0; i < lit; i++) chunk(x + 3 + i * (CHUNK_W + CHUNK_GAP), y + 3, green_rows);
}

static int steps_bottom(const ui_job *j) { return 104 + j->n * 21; }

void ui_flash(const ui_job *j, int full, const char *detail, int step_permille, int all_permille, int secs_left)
{
    static const gfx_image *const icons[4] = { &art_step_todo, &art_step_now, &art_step_done, &art_step_fail };
    char buf[32];
    int i, y, by, cur = 0;
    for (i = 0; i < j->n; i++) if (j->state[i] == UI_NOW || j->state[i] == UI_FAILED) cur = i;
    by = steps_bottom(j) + 16;
    if (full) {
        gfx_fill(0, 0, W, H, RGB(C_FACE));
        title_bar("Windows CE Loader");
        header(j->title, "Keep the calculator powered. Do not press RESET until Windows CE starts.", &art_warn);
        group_box(8, by, W - 16, 134, "Progress");
        footer_line();
        button(W - 86, H - 40, 76, "Cancel", 0, 1);
    }
    /* the step list and the inside of the progress box change as it runs */
    gfx_fill(0, 92, W, by - 6 - 92, RGB(C_FACE));
    for (i = 0, y = 104; i < j->n; i++, y += 21) {
        int st = j->state[i];
        gfx_image_draw(16, y - 9, icons[st], 0);
        gfx_text(st == UI_NOW ? &font_t11b : &font_t11, 36, y, j->label[i], RGB(st == UI_TODO ? 0x7A7868 : C_INK));
    }
    gfx_fill(10, by + 8, W - 20, 124, RGB(C_FACE));
    gfx_text(&font_t11b, 18, by + 24, j->label[cur], RGB(C_INK));
    gfx_text(&font_t11, 18, by + 40, detail, RGB(C_TEXT2));
    progress(18, by + 48, BAR_W(30), step_permille);
    hhk_format(buf, sizeof buf, "%u%%", (u32)(step_permille < 0 ? 0 : step_permille) / 10);
    gfx_text_align(&font_t11, W - 20, by + 59, buf, RGB(C_INK), GFX_RIGHT);
    gfx_text(&font_t11b, 18, by + 90, "All steps", RGB(C_INK));
    progress(18, by + 98, BAR_W(30), all_permille);
    hhk_format(buf, sizeof buf, "%u%%", (u32)(all_permille < 0 ? 0 : all_permille) / 10);
    gfx_text_align(&font_t11, W - 20, by + 109, buf, RGB(C_INK), GFX_RIGHT);
    if (secs_left >= 0) {
        hhk_format(buf, sizeof buf, "About %u s left", (u32)secs_left);
        gfx_text(&font_t10, 18, by + 128, buf, RGB(C_TEXT2));
    }
}

/* --- start and error --- */

void ui_boot(void)
{
    const int lw = 207, x = (W - lw + 1) / 2, y = 232, mx = W / 2 - 46, my = 430, mw = 92;
    int s;
    gfx_fill(0, 0, W, H, RGB(0x000000));
    gfx_image_draw(x - 3, y + (32 - art_flag36.h) / 2, &art_flag36, 0);
    gfx_mask_draw(x + 36, y, &art_lettering, RGB(C_WHITE));
    gfx_rrect(mx, my, mw, 14, 4, RGB(0x000000));
    gfx_rrect_line(mx, my, mw, 14, 4, RGB(0xB9B9B9));
    gfx_rrect_line(mx + 1, my + 1, mw - 2, 12, 3, RGB(0x4A4A4A));
    for (s = 4; s < 7; s++) chunk(mx + 4 + s * (CHUNK_W + CHUNK_GAP), my + 3, blue_rows);
}

void ui_error(const char *title, const char *headline, const char *detail, const char *const *todo, int ntodo)
{
    char buf[128];
    int y, i;
    gfx_fill(0, 0, W, H, RGB(C_FACE));
    title_bar("Windows CE Loader");
    header(title, "Windows CE was not started.", &art_err);
    gfx_image_draw(14, 104, &art_err, 0);
    gfx_text(&font_t11b, 58, 118, headline, RGB(C_INK));
    y = gfx_wrap(&font_t11, 58, 136, W - 70, 14, detail, RGB(C_INK));
    if (ntodo > 0) {
        int gy = y + 26, top = gy + 22;
        for (y = top, i = 0; i < ntodo; i++) {
            hhk_format(buf, sizeof buf, "%u. %s", (u32)(i + 1), todo[i]);
            y = gfx_wrap(&font_t11, 18, y, W - 40, 14, buf, RGB(C_INK)) + 18;
        }
        group_box(8, gy, W - 16, y - 18 - gy + 20, "What to do");
    }
    footer_line();
    button(W - 150, H - 40, 138, "Back to HollyHock", 1, 0);
}
