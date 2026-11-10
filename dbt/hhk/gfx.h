/* gfx.h - the loader's drawing: RGB565 straight into a 320x528 frame (gint's
 * VRAM on the calculator, a plain buffer in the host preview), shown by the
 * caller (gint's dupdate). Everything the Luna design needs and no more:
 * fills, per-row gradients, rounded rectangles without antialiasing (Luna's
 * corners), 1-bit bitmap text and RGB565 pictures with 4-bit alpha
 * (font_data.c, art_data.c: tools/mkbitmapfont.py, tools/mkloaderart.py). */
#ifndef HHK_GFX_H
#define HHK_GFX_H
#include "util.h"

#define GFX_W 320
#define GFX_H 528
#define RGB(h) ((u16)((((h) >> 19) & 0x1F) << 11 | (((h) >> 10) & 0x3F) << 5 | (((h) >> 3) & 0x1F)))

typedef struct { u8 adv; signed char left, top; u8 w, h; u16 off; } gfx_glyph;
typedef struct { u8 first, count, ascent; const gfx_glyph *g; const u8 *bits; } gfx_font;
/* px: w*h RGB565 or 0 (a mask drawn in one colour); a: 4-bit alpha, two
 * pixels per byte, high nibble first, or 0 (opaque) */
typedef struct { u16 w, h; const u16 *px; const u8 *a; } gfx_image;
typedef struct { u8 t; u32 rgb; } gfx_stop;     /* t: 0..255 down the area */

/* text codes beyond ASCII (font_data.c EXTRA) */
#define GFX_MIDDOT "\x7f"
#define GFX_UP     "\x80"
#define GFX_DOWN   "\x81"
#define GFX_ELLIP  "\x82"

extern u16 *gfx_fb;
extern const gfx_font font_t9b, font_t10, font_t11, font_t11b, font_t12b, font_tr13b;

void gfx_fill(int x, int y, int w, int h, u16 c);
void gfx_rows(int x, int y, int w, int h, const gfx_stop *s, int n);
void gfx_rrect(int x, int y, int w, int h, int r, u16 c);
void gfx_rrect_rows(int x, int y, int w, int h, int r, const gfx_stop *s, int n);
void gfx_rrect_line(int x, int y, int w, int h, int r, u16 c);
/* a 1 px line between whole-pixel points (Bresenham) */
void gfx_line(int x0, int y0, int x1, int y1, u16 c);
/* dim: drawn greyed out (disabled actions) */
void gfx_image_draw(int x, int y, const gfx_image *im, int dim);
void gfx_mask_draw(int x, int y, const gfx_image *im, u16 c);
/* text on its baseline y; returns the advance */
int  gfx_text(const gfx_font *f, int x, int y, const char *s, u16 c);
int  gfx_text_width(const gfx_font *f, const char *s);
enum { GFX_LEFT, GFX_CENTER, GFX_RIGHT };
int  gfx_text_align(const gfx_font *f, int x, int y, const char *s, u16 c, int align);
/* word-wrapped to maxw, lines lh apart from baseline y; returns the last baseline */
int  gfx_wrap(const gfx_font *f, int x, int y, int maxw, int lh, const char *s, u16 c);
#endif
