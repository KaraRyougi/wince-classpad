//------------------------------------------------------------------------------
//  hpcore.h - the HTML part of peghelp.exe: tokenizer and layout, with no
//  Windows dependency so the host tests (bsp/tests/test_peghelp.c) run it too.
//
//  The subset is what Pocket PC help files use: b/strong, i/em, u, font size,
//  sub/sup, br, p, hr, div, center (as a block), h1-h6, ul/ol/li, dl/dt/dd,
//  table/tr/td/th (auto column widths, no borders), a href / a name, img,
//  title and the Htm-Help meta. No scripts, no style sheets, no forms.
//
//  Written as C89 for the CE 6.0 compiler (declarations first, no stdint.h).
//------------------------------------------------------------------------------
#ifndef HPCORE_H
#define HPCORE_H

typedef unsigned short hp_char;         // UTF-16, WCHAR on CE

// font index: size class (0 small, 1 normal, 2 big, 3 bigger) and style bits
#define HP_FONT_ITALIC      1
#define HP_FONT_BOLD        2
#define HP_FONT_UNDERLINE   4
#define HP_FONT_SIZE(f)     ((f) >> 3)
#define HP_FONT(size, bits) (((size) << 3) | (bits))
#define HP_FONT_COUNT       32

enum { HP_TEXT = 1, HP_RULE, HP_IMAGE, HP_BULLET };

typedef struct {
    int kind;
    int font;
    int link;           // token index of the <a href>, -1 = not a link
    int x, y, w, h;     // document coordinates
    int s, n;           // text (HP_TEXT, HP_BULLET) or image source (HP_IMAGE)
} hp_item;

typedef struct {
    void *ctx;
    // height and ascent of a font, in pixels
    void (*font)(void *ctx, int font, int *height, int *ascent);
    // advance width of s[0..n)
    int (*width)(void *ctx, int font, const hp_char *s, int n);
    // size of an image named src[0..n) relative to the document; 0 = none
    int (*image)(void *ctx, const hp_char *src, int n, int *w, int *h);
} hp_metrics;

typedef struct hp_doc hp_doc;

// bytes of an HTML file (Windows-1252, or UTF-8 with a BOM)
hp_doc *hp_parse(const unsigned char *bytes, int n);
void hp_free(hp_doc *d);

// lay the document out for a view width; returns the document height
int hp_layout(hp_doc *d, const hp_metrics *m, int width);

const hp_item *hp_items(const hp_doc *d, int *count);
const hp_char *hp_item_text(const hp_doc *d, const hp_item *it, int *n);

// <title> text, and the Htm-Help meta's target ("file.htm#anchor"); NULL = none
const hp_char *hp_title(const hp_doc *d, int *n);
const hp_char *hp_contents(const hp_doc *d, int *n);

// y of <a name=...> (case-insensitive) after hp_layout, -1 = no such anchor
int hp_anchor(const hp_doc *d, const hp_char *name, int n);

// link under a document point (-1 = none) and its href
int hp_hit(const hp_doc *d, int x, int y);
const hp_char *hp_href(const hp_doc *d, int link, int *n);

#endif
