/* gfx.c - see gfx.h */
#include "gfx.h"

u16 *gfx_fb;
int gfx_level = 256;

static u16 level(u16 c)
{
    if (gfx_level >= 256) return c;
    return (u16)((((c >> 11) * gfx_level) >> 8) << 11 | ((((c >> 5) & 63) * gfx_level) >> 8) << 5 | (((c & 31) * gfx_level) >> 8));
}

/* rounded corners without antialiasing: how far row i (0 = the outermost) of
 * a corner of radius r is pulled in, r - (int)(sqrt(r^2 - (r - i - .5)^2) + .3) */
static const u8 corner[9][8] = {
    { 0 }, { 0 }, { 1, 0 }, { 2, 1, 0 }, { 2, 1, 0, 0 }, { 3, 2, 1, 0, 0 },
    { 4, 2, 1, 1, 0, 0 }, { 5, 3, 2, 1, 1, 0, 0 }, { 5, 4, 2, 2, 1, 1, 0, 0 },
};

static int inset(int r, int h, int y)
{
    if (r > 8) r = 8;
    if (r > h / 2) r = h / 2;
    if (y < 0 || y >= h) return 0x4000;
    if (y < r) return corner[r][y];
    if (y >= h - r) return corner[r][h - 1 - y];
    return 0;
}

static void span(int x, int y, int w, u16 c)
{
    u16 *p;
    if (y < 0 || y >= GFX_H) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    p = gfx_fb + y * GFX_W + x;
    c = level(c);
    while (w-- > 0) *p++ = c;
}

void gfx_fill(int x, int y, int w, int h, u16 c)
{
    for (; h > 0; h--, y++) span(x, y, w, c);
}

static u16 mix(u16 a, u16 b, int t, int n)   /* a..b at t/n per channel */
{
    int r = ((a >> 11) * (n - t) + (b >> 11) * t) / n;
    int g = (((a >> 5) & 63) * (n - t) + ((b >> 5) & 63) * t) / n;
    int bl = ((a & 31) * (n - t) + (b & 31) * t) / n;
    return (u16)(r << 11 | g << 5 | bl);
}

static u16 grad(const gfx_stop *s, int n, int y, int h)
{
    int t = h > 1 ? y * 255 / (h - 1) : 0, i = 0;
    while (i < n - 2 && t > s[i + 1].t) i++;
    if (t <= s[i].t) return RGB(s[i].rgb);
    if (t >= s[i + 1].t) return RGB(s[i + 1].rgb);
    return mix(RGB(s[i].rgb), RGB(s[i + 1].rgb), t - s[i].t, s[i + 1].t - s[i].t);
}

void gfx_rows(int x, int y, int w, int h, const gfx_stop *s, int n)
{
    int r;
    for (r = 0; r < h; r++) span(x, y + r, w, grad(s, n, r, h));
}

void gfx_rrect(int x, int y, int w, int h, int r, u16 c)
{
    int i, k;
    for (i = 0; i < h; i++) { k = inset(r, h, i); span(x + k, y + i, w - 2 * k, c); }
}

void gfx_rrect_rows(int x, int y, int w, int h, int r, const gfx_stop *s, int n)
{
    int i, k;
    for (i = 0; i < h; i++) { k = inset(r, h, i); span(x + k, y + i, w - 2 * k, grad(s, n, i, h)); }
}

void gfx_rrect_line(int x, int y, int w, int h, int r, u16 c)
{
    int i, k, k2, run;
    for (i = 0; i < h; i++) {
        k = inset(r, h, i);
        if (i == 0 || i == h - 1) { span(x + k, y + i, w - 2 * k, c); continue; }
        k2 = inset(r, h, i - 1) < inset(r, h, i + 1) ? inset(r, h, i + 1) : inset(r, h, i - 1);
        run = k2 - k > 1 ? k2 - k : 1;      /* the staircase back to the neighbour rows */
        span(x + k, y + i, run, c);
        span(x + w - k - run, y + i, run, c);
    }
}

void gfx_line(int x0, int y0, int x1, int y1, u16 c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int e = dx + dy, e2;
    for (;;) {
        span(x0, y0, 1, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

static u16 dimmed(u16 c)                    /* greyed out: luminance, lifted toward the dialog face */
{
    int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
    int l = (r * 77 + g * 150 + b * 29) >> 8;
    l = 120 + l * 120 / 255;
    return (u16)((l >> 3) << 11 | (l >> 2) << 5 | (l >> 3));
}

static void blend(int x, int y, u16 c, int a)
{
    u16 *p;
    if (!a || x < 0 || y < 0 || x >= GFX_W || y >= GFX_H) return;
    p = gfx_fb + y * GFX_W + x;
    c = level(c);
    *p = a >= 15 ? c : mix(*p, c, a, 15);
}

static int alpha(const gfx_image *im, int i)
{
    if (!im->a) return 15;
    return (i & 1) ? im->a[i >> 1] & 15 : im->a[i >> 1] >> 4;
}

void gfx_image_draw(int x, int y, const gfx_image *im, int dim)
{
    int i, j, k = 0;
    for (j = 0; j < im->h; j++)
        for (i = 0; i < im->w; i++, k++) {
            u16 c = im->px[k];
            blend(x + i, y + j, dim ? dimmed(c) : c, alpha(im, k));
        }
}

void gfx_mask_draw(int x, int y, const gfx_image *im, u16 c)
{
    int i, j, k = 0;
    for (j = 0; j < im->h; j++)
        for (i = 0; i < im->w; i++, k++) blend(x + i, y + j, c, alpha(im, k));
}

static const gfx_glyph *glyph(const gfx_font *f, u8 ch)
{
    if (ch < f->first || ch >= f->first + f->count) ch = '?';
    return &f->g[ch - f->first];
}

int gfx_text(const gfx_font *f, int x, int y, const char *s, u16 c)
{
    int x0 = x;
    for (; *s; s++) {
        const gfx_glyph *g = glyph(f, (u8)*s);
        u32 bit = g->off;
        int r, k;
        for (r = 0; r < g->h; r++)
            for (k = 0; k < g->w; k++, bit++)
                if (f->bits[bit >> 3] & (0x80 >> (bit & 7))) span(x + g->left + k, y - g->top + r, 1, c);
        x += g->adv;
    }
    return x - x0;
}

int gfx_text_width(const gfx_font *f, const char *s)
{
    int w = 0;
    for (; *s; s++) w += glyph(f, (u8)*s)->adv;
    return w;
}

int gfx_text_align(const gfx_font *f, int x, int y, const char *s, u16 c, int align)
{
    int w = gfx_text_width(f, s);
    if (align == GFX_CENTER) x -= w / 2;
    else if (align == GFX_RIGHT) x -= w;
    return gfx_text(f, x, y, s, c);
}

int gfx_wrap(const gfx_font *f, int x, int y, int maxw, int lh, const char *s, u16 c)
{
    char line[96];
    int n = 0, last_space = -1;
    for (;;) {
        char ch = *s;
        if (ch && n < (int)sizeof line - 1) {
            line[n] = ch; line[n + 1] = 0;
            if (ch == ' ') last_space = n;
            if (gfx_text_width(f, line) > maxw && last_space > 0) {
                line[last_space] = 0;
                gfx_text(f, x, y, line, c);
                y += lh;
                /* carry what followed the space to the next line */
                n -= last_space + 1;
                memcpy(line, line + last_space + 1, (unsigned)n + 1);
                last_space = -1;
                { int i; for (i = 0; i < n; i++) if (line[i] == ' ') last_space = i; }
            }
            n++; s++;
            continue;
        }
        line[n] = 0;
        if (n) gfx_text(f, x, y, line, c);
        return y;
    }
}
