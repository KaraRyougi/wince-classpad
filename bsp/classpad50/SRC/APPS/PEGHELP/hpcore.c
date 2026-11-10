//------------------------------------------------------------------------------
//  hpcore.c - HTML tokenizer and layout for peghelp.exe (see hpcore.h)
//
//  hp_parse turns the file into tokens once: text runs (entities decoded into
//  the text pool) and tags (an id, open/close, and the attribute text left in
//  the source buffer, read on demand). hp_layout walks the tokens with a style
//  state and fills lines left to right, one item per word; a line's items get
//  their y when the line ends (common baseline). Tables measure each cell
//  twice (one line: natural width; one word per line: minimum width) and
//  share the view width between the columns the way browsers' automatic
//  table layout does.
//------------------------------------------------------------------------------
#include <stdlib.h>
#include <string.h>
#include "hpcore.h"

#define MARGIN          4
#define LIST_INDENT     20
#define MAX_DEPTH       8
#define MAX_SIZES       16
#define HUGE_WIDTH      (1 << 28)

enum {
    TG_NONE, TG_A, TG_B, TG_STRONG, TG_I, TG_EM, TG_CITE, TG_VAR, TG_U, TG_BR,
    TG_P, TG_HR, TG_FONT, TG_OL, TG_UL, TG_LI, TG_DL, TG_DT, TG_DD, TG_TABLE,
    TG_TR, TG_TD, TG_TH, TG_TITLE, TG_HEAD, TG_BODY, TG_SUP, TG_SUB, TG_H1,
    TG_H2, TG_H3, TG_H4, TG_H5, TG_H6, TG_DIV, TG_CENTER, TG_IMG, TG_META,
    TG_SCRIPT, TG_STYLE, TG_BLOCKQUOTE, TG_PRE, TG_CAPTION, TG_HTML
};

static const char *const g_tagNames[] = {
    "", "a", "b", "strong", "i", "em", "cite", "var", "u", "br",
    "p", "hr", "font", "ol", "ul", "li", "dl", "dt", "dd", "table",
    "tr", "td", "th", "title", "head", "body", "sup", "sub", "h1",
    "h2", "h3", "h4", "h5", "h6", "div", "center", "img", "meta",
    "script", "style", "blockquote", "pre", "caption", "html"
};

enum { TK_TEXT = 1, TK_TAG };

typedef struct {
    int type;
    int tag;
    int close;
    int s, n;           // TK_TEXT: pool; TK_TAG: attributes in src
} hp_tok;

typedef struct {
    int s, n;           // name in src
    int y;
} hp_anc;

struct hp_doc {
    hp_char *src;   int nsrc;
    hp_char *pool;  int npool, cpool;
    hp_char *lpool; int nlpool, clpool;     // layout strings (list numbers)
    hp_tok *tok;    int ntok, ctok;
    hp_item *item;  int nitem, citem;
    hp_anc *anc;    int nanc, canc;
    int titleS, titleN;
    int metaTok;
    int height;
};

//------------------------------------------------------------------------------
// helpers

static int Grow(void **p, int *cap, int need, int size)
{
    int nc;
    void *q;
    if (need <= *cap)
        return 1;
    nc = *cap ? *cap * 2 : 64;
    while (nc < need)
        nc *= 2;
    q = realloc(*p, (size_t)nc * (size_t)size);
    if (!q)
        return 0;
    *p = q;
    *cap = nc;
    return 1;
}

static int IsWs(hp_char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static int IsAlpha(hp_char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int IsAlnum(hp_char c)
{
    return IsAlpha(c) || (c >= '0' && c <= '9');
}

static hp_char Lower(hp_char c)
{
    return (hp_char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
}

// case-insensitive compare of src[s..s+n) with an ASCII string
static int EqA(const hp_char *s, int n, const char *a)
{
    int i;
    for (i = 0; i < n; i++)
        if (!a[i] || Lower(s[i]) != (hp_char)Lower((hp_char)(unsigned char)a[i]))
            return 0;
    return a[n] == 0;
}

static int EqW(const hp_char *a, int na, const hp_char *b, int nb)
{
    int i;
    if (na != nb)
        return 0;
    for (i = 0; i < na; i++)
        if (Lower(a[i]) != Lower(b[i]))
            return 0;
    return 1;
}

//------------------------------------------------------------------------------
// decoding

// Windows-1252 0x80-0x9F (browsers also use it for &#128; - &#159;)
static const unsigned short g_cp1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

static hp_char Cp1252(unsigned int c)
{
    if (c >= 0x80 && c < 0xA0)
        return g_cp1252[c - 0x80];
    return (hp_char)c;
}

static int Decode(hp_doc *d, const unsigned char *b, int n)
{
    int i = 0, o = 0;
    int utf8 = n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF;
    d->src = (hp_char *)malloc((size_t)(n + 1) * sizeof(hp_char));
    if (!d->src)
        return 0;
    if (utf8)
        i = 3;
    while (i < n) {
        unsigned int c = b[i++];
        if (utf8 && c >= 0xC0) {
            int more = c >= 0xE0 ? (c >= 0xF0 ? 3 : 2) : 1;
            c &= more == 1 ? 0x1F : (more == 2 ? 0x0F : 0x07);
            while (more-- > 0 && i < n && (b[i] & 0xC0) == 0x80)
                c = (c << 6) | (b[i++] & 0x3F);
            if (c > 0xFFFF)
                c = '?';
            d->src[o++] = (hp_char)c;
        } else {
            d->src[o++] = utf8 ? (hp_char)c : Cp1252(c);
        }
    }
    d->src[o] = 0;
    d->nsrc = o;
    return 1;
}

typedef struct { const char *name; unsigned short ch; } hp_entity;

static const hp_entity g_entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
    { "apos", '\'' }, { "nbsp", 0xA0 }, { "copy", 0xA9 }, { "reg", 0xAE },
    { "trade", 0x2122 }, { "deg", 0xB0 }, { "plusmn", 0xB1 }, { "sup2", 0xB2 },
    { "sup3", 0xB3 }, { "micro", 0xB5 }, { "middot", 0xB7 }, { "frac12", 0xBD },
    { "frac14", 0xBC }, { "frac34", 0xBE }, { "times", 0xD7 }, { "divide", 0xF7 },
    { "laquo", 0xAB }, { "raquo", 0xBB }, { "bull", 0x2022 }, { "hellip", 0x2026 },
    { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "pi", 0x03C0 }, { "larr", 0x2190 },
    { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "le", 0x2264 },
    { "ge", 0x2265 }, { "ne", 0x2260 }, { "infin", 0x221E }, { "radic", 0x221A },
    { "theta", 0x03B8 }, { "alpha", 0x03B1 }, { "beta", 0x03B2 }, { "Delta", 0x0394 },
    { "sect", 0xA7 }, { "para", 0xB6 }, { "iexcl", 0xA1 }, { "iquest", 0xBF },
    { NULL, 0 }
};

static int PoolPut(hp_doc *d, hp_char c)
{
    if (!Grow((void **)&d->pool, &d->cpool, d->npool + 1, sizeof(hp_char)))
        return 0;
    d->pool[d->npool++] = c;
    return 1;
}

// src[i] == '&': decodes one entity into the pool, returns the index after it
static int Entity(hp_doc *d, int i, int end)
{
    const hp_char *s = d->src;
    int j = i + 1;
    unsigned int v = 0;
    if (j < end && s[j] == '#') {
        int hex = 0, digits = 0;
        j++;
        if (j < end && (s[j] == 'x' || s[j] == 'X')) {
            hex = 1;
            j++;
        }
        for (; j < end; j++) {
            hp_char c = s[j];
            if (c >= '0' && c <= '9')
                v = v * (hex ? 16 : 10) + (c - '0');
            else if (hex && Lower(c) >= 'a' && Lower(c) <= 'f')
                v = v * 16 + (Lower(c) - 'a' + 10);
            else
                break;
            digits++;
            if (v > 0xFFFF)
                v = '?';
        }
        if (digits) {
            if (j < end && s[j] == ';')
                j++;
            PoolPut(d, v ? Cp1252(v) : (hp_char)'?');
            return j;
        }
    } else {
        int k;
        while (j < end && IsAlnum(s[j]) && j - i <= 8)
            j++;
        for (k = 0; g_entities[k].name; k++) {
            // entity names are case-sensitive (Delta vs delta); try exact first
            const char *a = g_entities[k].name;
            int n = j - i - 1, m = 0;
            while (m < n && a[m] && s[i + 1 + m] == (hp_char)(unsigned char)a[m])
                m++;
            if (m == n && !a[m]) {
                if (j < end && s[j] == ';')
                    j++;
                PoolPut(d, g_entities[k].ch);
                return j;
            }
        }
    }
    PoolPut(d, '&');
    return i + 1;
}

//------------------------------------------------------------------------------
// tokenizer

static int AddTok(hp_doc *d, int type, int tag, int close, int s, int n)
{
    hp_tok *t;
    if (!Grow((void **)&d->tok, &d->ctok, d->ntok + 1, sizeof(hp_tok)))
        return 0;
    t = &d->tok[d->ntok++];
    t->type = type;
    t->tag = tag;
    t->close = close;
    t->s = s;
    t->n = n;
    return 1;
}

static int TagId(const hp_char *s, int n)
{
    int k;
    for (k = 1; k < (int)(sizeof g_tagNames / sizeof g_tagNames[0]); k++)
        if (EqA(s, n, g_tagNames[k]))
            return k;
    return TG_NONE;
}

static int Find(const hp_doc *d, int i, const char *a)
{
    // first index >= i where the ASCII string a starts (case-insensitive)
    int n = (int)strlen(a);
    for (; i + n <= d->nsrc; i++)
        if (EqA(d->src + i, n, a))
            return i;
    return d->nsrc;
}

// attribute value of a tag token; returns 1 and the value's place in src
static int Attr(const hp_doc *d, const hp_tok *t, const char *name, int *vs, int *vn)
{
    const hp_char *s = d->src;
    int i = t->s, end = t->s + t->n;
    while (i < end) {
        int ns, nn;
        while (i < end && (IsWs(s[i]) || s[i] == '/'))
            i++;
        ns = i;
        while (i < end && !IsWs(s[i]) && s[i] != '=' && s[i] != '/')
            i++;
        nn = i - ns;
        while (i < end && IsWs(s[i]))
            i++;
        if (i < end && s[i] == '=') {
            int a, b;
            i++;
            while (i < end && IsWs(s[i]))
                i++;
            if (i < end && (s[i] == '"' || s[i] == '\'')) {
                hp_char q = s[i++];
                a = i;
                while (i < end && s[i] != q)
                    i++;
                b = i;
                if (i < end)
                    i++;
            } else {
                a = i;
                while (i < end && !IsWs(s[i]))
                    i++;
                b = i;
            }
            if (nn && EqA(s + ns, nn, name)) {
                *vs = a;
                *vn = b - a;
                return 1;
            }
        } else if (nn && EqA(s + ns, nn, name)) {
            *vs = ns;
            *vn = 0;
            return 1;
        } else if (!nn) {
            i++;
        }
    }
    return 0;
}

static int AttrInt(const hp_doc *d, const hp_tok *t, const char *name, int def)
{
    int vs, vn, v = 0, i, any = 0;
    if (!Attr(d, t, name, &vs, &vn))
        return def;
    for (i = 0; i < vn && d->src[vs + i] >= '0' && d->src[vs + i] <= '9'; i++) {
        v = v * 10 + (d->src[vs + i] - '0');
        any = 1;
        if (v > 10000)
            break;
    }
    return any ? v : def;
}

hp_doc *hp_parse(const unsigned char *bytes, int n)
{
    hp_doc *d = (hp_doc *)calloc(1, sizeof(hp_doc));
    int i = 0, inTitle = 0;
    if (!d)
        return NULL;
    d->metaTok = -1;
    d->titleS = -1;
    if (!Decode(d, bytes, n)) {
        hp_free(d);
        return NULL;
    }
    while (i < d->nsrc) {
        const hp_char *s = d->src;
        int start;
        if (s[i] == '<') {
            if (EqA(s + i, 4 < d->nsrc - i ? 4 : d->nsrc - i, "<!--")) {
                int e = Find(d, i + 4, "-->");
                i = e < d->nsrc ? e + 3 : e;
                continue;
            }
            if (i + 1 < d->nsrc && (s[i + 1] == '!' || s[i + 1] == '?')) {
                while (i < d->nsrc && s[i] != '>')
                    i++;
                i++;
                continue;
            }
            {
                int k = i + 1, close = 0, ns, tag, a;
                hp_char q = 0;
                if (k < d->nsrc && s[k] == '/') {
                    close = 1;
                    k++;
                }
                if (k < d->nsrc && IsAlpha(s[k])) {
                    ns = k;
                    while (k < d->nsrc && IsAlnum(s[k]))
                        k++;
                    tag = TagId(s + ns, k - ns);
                    a = k;
                    while (k < d->nsrc && (q || s[k] != '>')) {
                        if (q && s[k] == q)
                            q = 0;
                        else if (!q && (s[k] == '"' || s[k] == '\''))
                            q = s[k];
                        k++;
                    }
                    if (tag != TG_NONE) {
                        if (!AddTok(d, TK_TAG, tag, close, a, k - a))
                            break;
                        if (tag == TG_TITLE)
                            inTitle = !close;
                        if (tag == TG_META && !close) {
                            int vs, vn;
                            if (Attr(d, &d->tok[d->ntok - 1], "http-equiv", &vs, &vn) && EqA(s + vs, vn, "htm-help"))
                                d->metaTok = d->ntok - 1;
                        }
                    }
                    i = k < d->nsrc ? k + 1 : k;
                    if (!close && (tag == TG_SCRIPT || tag == TG_STYLE)) {
                        i = Find(d, i, tag == TG_SCRIPT ? "</script" : "</style");
                    }
                    continue;
                }
            }
        }
        // text up to the next tag (a '<' that does not open one is text)
        start = d->npool;
        {
            int j = i;
            while (j < d->nsrc && !(s[j] == '<' && j > i))
                j++;
            while (i < j) {
                if (s[i] == '&')
                    i = Entity(d, i, j);
                else if (!PoolPut(d, s[i++]))
                    break;
            }
        }
        if (d->npool > start) {
            if (!AddTok(d, TK_TEXT, 0, 0, start, d->npool - start))
                break;
            if (inTitle && d->titleS < 0) {
                int a = start, b = d->npool;
                while (a < b && IsWs(d->pool[a]))
                    a++;
                while (b > a && IsWs(d->pool[b - 1]))
                    b--;
                if (b > a) {
                    d->titleS = a;
                    d->titleN = b - a;
                }
            }
        }
    }
    return d;
}

void hp_free(hp_doc *d)
{
    if (!d)
        return;
    free(d->src);
    free(d->pool);
    free(d->lpool);
    free(d->tok);
    free(d->item);
    free(d->anc);
    free(d);
}

//------------------------------------------------------------------------------
// layout

typedef struct {
    hp_doc *d;
    const hp_metrics *m;
    int measure;            // only widths: no items, no anchors
    int x0, x1, top;        // box
    int indent;
    int y, x;               // line top, pen
    int lineFirst;          // first item of the line
    int content, words;     // line has items / words
    int space;              // whitespace seen since the last word
    int gap;                // vertical space owed before the next line
    int asc, desc;          // line extent around the baseline
    int maxw;               // widest line (from x0)
    int bold, italic, under, script, link, hide;
    int size[MAX_SIZES], nsize;
    int lstOrd[MAX_DEPTH], lstN[MAX_DEPTH], depth;
    int ddIndent;
} hp_lay;

static void Flow(hp_lay *L, int t0, int t1);

static int CurFont(const hp_lay *L)
{
    int size = L->size[L->nsize - 1];
    int bits = 0;
    if (L->script && size > 0)
        size--;
    if (L->bold)
        bits |= HP_FONT_BOLD;
    if (L->italic)
        bits |= HP_FONT_ITALIC;
    if (L->under || L->link >= 0)
        bits |= HP_FONT_UNDERLINE;
    return HP_FONT(size, bits);
}

static void FontMetrics(const hp_lay *L, int font, int *h, int *a)
{
    L->m->font(L->m->ctx, font, h, a);
}

static int NormalHeight(const hp_lay *L)
{
    int h, a;
    FontMetrics(L, HP_FONT(1, 0), &h, &a);
    return h;
}

static hp_item *Emit(hp_lay *L, int kind, int font, int x, int w, int h, int rel, int s, int n)
{
    hp_doc *d = L->d;
    hp_item *it;
    if (L->measure)
        return NULL;
    if (!Grow((void **)&d->item, &d->citem, d->nitem + 1, sizeof(hp_item)))
        return NULL;
    it = &d->item[d->nitem++];
    it->kind = kind;
    it->font = font;
    it->link = L->link;
    it->x = x;
    it->w = w;
    it->h = h;
    it->y = rel;            // relative to the baseline until EndLine
    it->s = s;
    it->n = n;
    return it;
}

static void BeginLine(hp_lay *L)
{
    if (L->content)
        return;
    if (L->y > L->top)
        L->y += L->gap;
    L->gap = 0;
    L->x = L->x0 + L->indent;
    L->lineFirst = L->d->nitem;
    L->asc = L->desc = 0;
    L->content = 1;
    L->words = 0;
}

static void Extent(hp_lay *L, int asc, int desc)
{
    if (asc > L->asc)
        L->asc = asc;
    if (desc > L->desc)
        L->desc = desc;
}

static void EndLine(hp_lay *L)
{
    int i, base;
    if (!L->content)
        return;
    base = L->y + L->asc;
    if (!L->measure)
        for (i = L->lineFirst; i < L->d->nitem; i++)
            L->d->item[i].y += base;
    if (L->x - L->x0 > L->maxw)
        L->maxw = L->x - L->x0;
    L->y += L->asc + L->desc;
    L->content = L->words = L->space = 0;
}

static void Block(hp_lay *L, int gap)
{
    EndLine(L);
    L->space = 0;
    if (gap > L->gap)
        L->gap = gap;
}

// places a box of width w that wraps like a word; returns its x
static int Place(hp_lay *L, int font, int w)
{
    int sw = 0;
    if (L->space && L->words) {
        hp_char sp = ' ';
        sw = L->m->width(L->m->ctx, font, &sp, 1);
    }
    if (L->content && L->words && L->x + sw + w > L->x1) {
        EndLine(L);
        sw = 0;
    }
    BeginLine(L);
    L->x += sw;
    L->space = 0;
    L->words++;
    L->x += w;
    return L->x - w;
}

static void Word(hp_lay *L, int s, int n)
{
    int font = CurFont(L), h, a, w, x, shift = 0;
    FontMetrics(L, font, &h, &a);
    w = L->m->width(L->m->ctx, font, L->d->pool + s, n);
    x = Place(L, font, w);
    if (L->script > 0)
        shift = -(a * 2 / 3);
    else if (L->script < 0)
        shift = a / 4;
    Extent(L, a - shift, h - a + shift);
    Emit(L, HP_TEXT, font, x, w, h, -a + shift, s, n);
}

static void Text(hp_lay *L, int s, int n)
{
    const hp_char *p = L->d->pool;
    int i = s, end = s + n;
    if (L->hide)
        return;
    while (i < end) {
        int j;
        if (IsWs(p[i])) {
            L->space = 1;
            i++;
            continue;
        }
        j = i;
        while (j < end && !IsWs(p[j]))
            j++;
        Word(L, i, j - i);
        i = j;
    }
}

static void Image(hp_lay *L, const hp_tok *t)
{
    int vs, vn, w, h, x;
    if (L->hide || !Attr(L->d, t, "src", &vs, &vn) || !vn)
        return;
    w = AttrInt(L->d, t, "width", -1);
    h = AttrInt(L->d, t, "height", -1);
    if (w < 0 || h < 0) {
        int iw = 0, ih = 0;
        if (!L->m->image || !L->m->image(L->m->ctx, L->d->src + vs, vn, &iw, &ih))
            return;
        if (w < 0)
            w = iw;
        if (h < 0)
            h = ih;
    }
    x = Place(L, CurFont(L), w);
    Extent(L, h, 0);
    Emit(L, HP_IMAGE, 0, x, w, h, -h, vs, vn);
}

static void Bullet(hp_lay *L)
{
    hp_doc *d = L->d;
    int font = HP_FONT(1, 0), h, a, w, x, s = d->nlpool, n = 0;
    hp_char buf[12];
    int lvl = L->depth - 1;
    hp_item *it;
    Block(L, 0);
    BeginLine(L);
    if (L->lstOrd[lvl]) {
        int v = L->lstN[lvl], k = 0;
        hp_char tmp[8];
        do {
            tmp[k++] = (hp_char)('0' + v % 10);
            v /= 10;
        } while (v && k < 7);
        while (k)
            buf[n++] = tmp[--k];
        buf[n++] = '.';
    } else {
        // bullet, then en dash for nested lists (CE fonts lack U+25E6)
        buf[n++] = (hp_char)((lvl & 1) ? 0x2013 : 0x2022);
    }
    if (!L->measure) {
        if (!Grow((void **)&d->lpool, &d->clpool, d->nlpool + n, sizeof(hp_char)))
            return;
        memcpy(d->lpool + s, buf, (size_t)n * sizeof(hp_char));
        d->nlpool += n;
    }
    FontMetrics(L, font, &h, &a);
    w = L->m->width(L->m->ctx, font, buf, n);
    x = L->x0 + L->indent - w - 5;
    if (x < L->x0)
        x = L->x0;
    Extent(L, a, h - a);
    it = Emit(L, HP_BULLET, font, x, w, h, -a, s, n);
    if (it)
        it->link = -1;
}

static void Anchor(hp_lay *L, int vs, int vn)
{
    hp_doc *d = L->d;
    hp_anc *a;
    if (L->measure || !vn)
        return;
    if (!Grow((void **)&d->anc, &d->canc, d->nanc + 1, sizeof(hp_anc)))
        return;
    a = &d->anc[d->nanc++];
    a->s = vs;
    a->n = vn;
    a->y = L->content ? L->y : L->y + (L->y > L->top ? L->gap : 0);
}

static void PushSize(hp_lay *L, int size)
{
    if (size < 0)
        size = 0;
    if (size > 3)
        size = 3;
    if (L->nsize < MAX_SIZES)
        L->size[L->nsize++] = size;
    else
        L->size[MAX_SIZES - 1] = size;
}

static void PopSize(hp_lay *L)
{
    if (L->nsize > 1)
        L->nsize--;
}

static int FontSize(const hp_lay *L, const hp_tok *t)
{
    // HTML sizes 1-7, 3 = normal; "+n"/"-n" relative to 3
    int vs, vn, i, v = 0, sign = 0, html;
    const hp_char *s = L->d->src;
    if (!Attr(L->d, t, "size", &vs, &vn) || !vn)
        return L->size[L->nsize - 1];
    i = vs;
    if (s[i] == '+' || s[i] == '-')
        sign = s[i++] == '+' ? 1 : -1;
    while (i < vs + vn && s[i] >= '0' && s[i] <= '9')
        v = v * 10 + (s[i++] - '0');
    html = sign ? 3 + sign * v : v;
    if (html <= 2)
        return 0;
    if (html == 3)
        return 1;
    if (html == 4)
        return 2;
    return 3;
}

static void Dec(int *v)
{
    if (*v > 0)
        (*v)--;
}

// one table cell, laid out (or measured) in a box [x0, x0 + w)
static int Cell(const hp_lay *L, int t0, int t1, int x0, int w, int y, int th, int measure, int *maxw)
{
    hp_lay S = *L;
    S.measure = measure;
    S.x0 = x0;
    S.x1 = x0 + w;
    S.top = S.y = y;
    S.indent = 0;
    S.depth = 0;
    S.ddIndent = 0;
    S.content = S.words = S.space = 0;
    S.gap = 0;
    S.maxw = 0;
    if (th)
        S.bold++;
    Flow(&S, t0, t1);
    EndLine(&S);
    if (maxw)
        *maxw = S.maxw;
    return S.y;
}

typedef struct { int t0, t1, row, col, th; } hp_cell;

static int Table(hp_lay *L, int t)
{
    hp_doc *d = L->d;
    int e, k, nest = 0, row = -1, col = 0, ncell = 0, ccell = 0, ncols = 0, nrows;
    int pad, spacing, avail, sumNat = 0, sumMin = 0, c, r, x, y;
    int *nat = NULL, *mn = NULL, *w = NULL;
    hp_cell *cell = NULL, *cur = NULL;
    const hp_tok *tt = &d->tok[t];

    for (e = t + 1; e < d->ntok; e++) {
        const hp_tok *k2 = &d->tok[e];
        if (k2->type == TK_TAG && k2->tag == TG_TABLE) {
            if (!k2->close)
                nest++;
            else if (nest-- == 0)
                break;
        }
    }
    nest = 0;
    for (k = t + 1; k < e; k++) {
        const hp_tok *k2 = &d->tok[k];
        if (k2->type != TK_TAG)
            continue;
        if (k2->tag == TG_TABLE) {
            nest += k2->close ? -1 : 1;
            continue;
        }
        if (nest)
            continue;
        // any cell or row boundary ends the open cell
        if ((k2->tag == TG_TD || k2->tag == TG_TH || k2->tag == TG_TR || k2->tag == TG_CAPTION) && cur && cur->t1 < 0)
            cur->t1 = k;
        if (k2->tag == TG_TR && !k2->close) {
            row++;
            col = 0;
        } else if ((k2->tag == TG_TD || k2->tag == TG_TH) && !k2->close) {
            if (row < 0)
                row = 0;
            if (!Grow((void **)&cell, &ccell, ncell + 1, sizeof(hp_cell)))
                break;
            cur = &cell[ncell++];
            cur->t0 = k + 1;
            cur->t1 = -1;
            cur->row = row;
            cur->col = col++;
            cur->th = k2->tag == TG_TH;
            if (col > ncols)
                ncols = col;
        }
    }
    if (cur && cur->t1 < 0)
        cur->t1 = e;
    nrows = row + 1;

    Block(L, 0);
    if (!ncell || !ncols)
        goto done;
    nat = (int *)calloc((size_t)ncols * 3, sizeof(int));
    if (!nat)
        goto done;
    mn = nat + ncols;
    w = mn + ncols;
    pad = AttrInt(d, tt, "cellpadding", 2);
    spacing = AttrInt(d, tt, "cellspacing", 2);
    if (pad > 8)
        pad = 8;
    if (spacing > 8)
        spacing = 8;
    for (k = 0; k < ncell; k++) {
        int a, b;
        Cell(L, cell[k].t0, cell[k].t1, 0, HUGE_WIDTH, 0, cell[k].th, 1, &a);
        Cell(L, cell[k].t0, cell[k].t1, 0, 1, 0, cell[k].th, 1, &b);
        if (a > nat[cell[k].col])
            nat[cell[k].col] = a;
        if (b > mn[cell[k].col])
            mn[cell[k].col] = b;
    }
    avail = L->x1 - (L->x0 + L->indent) - ncols * 2 * pad - (ncols - 1) * spacing;
    for (c = 0; c < ncols; c++) {
        sumNat += nat[c];
        sumMin += mn[c];
    }
    if (sumNat <= avail || sumMin >= avail || sumNat == sumMin) {
        for (c = 0; c < ncols; c++)
            w[c] = sumNat <= avail ? nat[c] : mn[c];
    } else {
        // columns that fit in an even share keep their natural width; the
        // others share the rest between their minimum and natural widths
        int share = avail / ncols, rest = avail, restNat = 0, restMin = 0;
        for (c = 0; c < ncols; c++) {
            if (nat[c] <= share) {
                w[c] = nat[c];
                rest -= nat[c];
            } else {
                w[c] = -1;
                restNat += nat[c];
                restMin += mn[c];
            }
        }
        for (c = 0; c < ncols; c++) {
            if (w[c] >= 0)
                continue;
            if (rest <= restMin || restNat == restMin)
                w[c] = mn[c];
            else
                w[c] = mn[c] + (int)((long)(rest - restMin) * (nat[c] - mn[c]) / (restNat - restMin));
        }
    }

    if (L->y > L->top)
        L->y += L->gap;
    L->gap = 0;
    y = L->y;
    for (r = 0; r < nrows; r++) {
        int bottom = y;
        for (k = 0; k < ncell; k++) {
            int b;
            if (cell[k].row != r)
                continue;
            x = L->x0 + L->indent;
            for (c = 0; c < cell[k].col; c++)
                x += w[c] + 2 * pad + spacing;
            b = Cell(L, cell[k].t0, cell[k].t1, x + pad, w[cell[k].col], y + pad, cell[k].th, L->measure, NULL) + pad;
            if (b > bottom)
                bottom = b;
        }
        y = bottom + (r + 1 < nrows ? spacing : 0);
    }
    L->y = y;
    x = L->indent;
    for (c = 0; c < ncols; c++)
        x += w[c] + 2 * pad + (c ? spacing : 0);
    if (x > L->maxw)
        L->maxw = x;

done:
    free(nat);
    free(cell);
    Block(L, NormalHeight(L) / 2);
    return e;
}

static void Tag(hp_lay *L, int *pt)
{
    hp_doc *d = L->d;
    const hp_tok *t = &d->tok[*pt];
    int close = t->close, vs, vn;
    int para = NormalHeight(L) / 2;

    switch (t->tag) {
    case TG_HEAD:
        L->hide = close ? (L->hide & ~1) : (L->hide | 1);
        break;
    case TG_TITLE:
        L->hide = close ? (L->hide & ~2) : (L->hide | 2);
        break;
    case TG_BODY:
        L->hide = 0;
        break;
    case TG_B: case TG_STRONG:
        if (close) Dec(&L->bold); else L->bold++;
        break;
    case TG_I: case TG_EM: case TG_CITE: case TG_VAR:
        if (close) Dec(&L->italic); else L->italic++;
        break;
    case TG_U:
        if (close) Dec(&L->under); else L->under++;
        break;
    case TG_FONT:
        if (close) PopSize(L); else PushSize(L, FontSize(L, t));
        break;
    case TG_SUP:
        L->script = close ? 0 : 1;
        break;
    case TG_SUB:
        L->script = close ? 0 : -1;
        break;
    case TG_BR:
        if (L->hide)
            break;
        if (L->content) {
            EndLine(L);
        } else {
            if (L->y > L->top)
                L->y += L->gap;
            L->gap = 0;
            L->y += NormalHeight(L);
        }
        L->space = 0;
        break;
    case TG_P: case TG_BLOCKQUOTE: case TG_PRE:
        Block(L, para);
        break;
    case TG_DIV: case TG_CENTER: case TG_CAPTION: case TG_TR: case TG_TD: case TG_TH:
        Block(L, 0);
        break;
    case TG_HR: {
        hp_item *it;
        if (close)
            break;
        Block(L, 0);
        if (L->y > L->top)
            L->y += L->gap;
        L->gap = 0;
        it = Emit(L, HP_RULE, 0, L->x0 + L->indent, L->x1 - L->x0 - L->indent, 1, 0, 0, 0);
        if (it) {
            it->y = L->y + 3;   // not on a line: absolute already
            it->link = -1;
        }
        L->y += 7;
        break;
    }
    case TG_H1: case TG_H2: case TG_H3: case TG_H4: case TG_H5: case TG_H6:
        Block(L, para);
        if (close) {
            PopSize(L);
            Dec(&L->bold);
        } else {
            PushSize(L, t->tag <= TG_H2 ? 3 : (t->tag == TG_H3 ? 2 : 1));
            L->bold++;
        }
        break;
    case TG_UL: case TG_OL:
        if (close) {
            Block(L, 0);
            if (L->depth > 0)
                L->depth--;
            L->indent = L->depth * LIST_INDENT + L->ddIndent;
            if (!L->depth)
                Block(L, para);
        } else {
            Block(L, L->depth ? 0 : para);
            if (L->depth < MAX_DEPTH) {
                L->lstOrd[L->depth] = t->tag == TG_OL;
                L->lstN[L->depth] = AttrInt(d, t, "start", 1) - 1;
                L->depth++;
            }
            L->indent = L->depth * LIST_INDENT + L->ddIndent;
        }
        break;
    case TG_LI:
        if (close) {
            Block(L, 0);
            break;
        }
        if (!L->depth) {
            L->lstOrd[0] = 0;
            L->lstN[0] = 0;
            L->depth = 1;
            L->indent = LIST_INDENT + L->ddIndent;
        }
        L->lstN[L->depth - 1]++;
        if (!L->hide)
            Bullet(L);
        break;
    case TG_DL:
    case TG_DT:
        if (L->ddIndent) {
            L->ddIndent -= LIST_INDENT;
            L->indent -= LIST_INDENT;
        }
        Block(L, t->tag == TG_DL ? para : 0);
        break;
    case TG_DD:
        Block(L, 0);
        if (!close && !L->ddIndent) {
            L->ddIndent = LIST_INDENT;
            L->indent += LIST_INDENT;
        } else if (close && L->ddIndent) {
            L->ddIndent = 0;
            L->indent -= LIST_INDENT;
        }
        break;
    case TG_TABLE:
        if (!close && !L->hide)
            *pt = Table(L, *pt);
        else
            Block(L, 0);
        break;
    case TG_A:
        if (close) {
            L->link = -1;
            break;
        }
        if (Attr(d, t, "name", &vs, &vn))
            Anchor(L, vs, vn);
        else if (Attr(d, t, "id", &vs, &vn))
            Anchor(L, vs, vn);
        if (Attr(d, t, "href", &vs, &vn) && vn)
            L->link = *pt;
        break;
    case TG_IMG:
        Image(L, t);
        break;
    default:
        break;
    }
}

static void Flow(hp_lay *L, int t0, int t1)
{
    int t;
    for (t = t0; t < t1 && t < L->d->ntok; t++) {
        const hp_tok *tk = &L->d->tok[t];
        if (tk->type == TK_TEXT)
            Text(L, tk->s, tk->n);
        else
            Tag(L, &t);
    }
}

int hp_layout(hp_doc *d, const hp_metrics *m, int width)
{
    hp_lay L;
    memset(&L, 0, sizeof L);
    d->nitem = 0;
    d->nanc = 0;
    d->nlpool = 0;
    L.d = d;
    L.m = m;
    L.x0 = MARGIN;
    L.x1 = width - MARGIN;
    if (L.x1 < L.x0 + 40)
        L.x1 = L.x0 + 40;
    L.top = L.y = MARGIN;
    L.size[0] = 1;
    L.nsize = 1;
    L.link = -1;
    Flow(&L, 0, d->ntok);
    EndLine(&L);
    d->height = L.y + 2 * MARGIN;
    return d->height;
}

//------------------------------------------------------------------------------
// queries

const hp_item *hp_items(const hp_doc *d, int *count)
{
    *count = d->nitem;
    return d->item;
}

const hp_char *hp_item_text(const hp_doc *d, const hp_item *it, int *n)
{
    *n = it->n;
    if (it->kind == HP_TEXT)
        return d->pool + it->s;
    if (it->kind == HP_BULLET)
        return d->lpool + it->s;
    if (it->kind == HP_IMAGE)
        return d->src + it->s;
    *n = 0;
    return NULL;
}

const hp_char *hp_title(const hp_doc *d, int *n)
{
    if (d->titleS < 0)
        return NULL;
    *n = d->titleN;
    return d->pool + d->titleS;
}

const hp_char *hp_contents(const hp_doc *d, int *n)
{
    int vs, vn;
    if (d->metaTok < 0 || !Attr(d, &d->tok[d->metaTok], "content", &vs, &vn) || !vn)
        return NULL;
    *n = vn;
    return d->src + vs;
}

int hp_anchor(const hp_doc *d, const hp_char *name, int n)
{
    int i, best = -1, bestN = 0;
    for (i = 0; i < d->nanc; i++)
        if (EqW(d->src + d->anc[i].s, d->anc[i].n, name, n))
            return d->anc[i].y;
    // no exact match: the longest anchor the name starts with, so a link with
    // a stray plural (geometry1.htm#dynamic_attributes -> dynamic_attribute)
    // still lands on its section
    for (i = 0; i < d->nanc; i++) {
        int an = d->anc[i].n;
        if (an >= 4 && an < n && an > bestN && EqW(d->src + d->anc[i].s, an, name, an)) {
            best = d->anc[i].y;
            bestN = an;
        }
    }
    return best;
}

int hp_hit(const hp_doc *d, int x, int y)
{
    int i;
    for (i = 0; i < d->nitem; i++) {
        const hp_item *it = &d->item[i];
        if (it->link >= 0 && x >= it->x - 3 && x < it->x + it->w + 3 && y >= it->y - 1 && y < it->y + it->h + 1)
            return it->link;
    }
    return -1;
}

const hp_char *hp_href(const hp_doc *d, int link, int *n)
{
    int vs, vn;
    if (link < 0 || link >= d->ntok || !Attr(d, &d->tok[link], "href", &vs, &vn))
        return NULL;
    *n = vn;
    return d->src + vs;
}

//------------------------------------------------------------------------------
// the Help Contents page (hpcore.h)

int hp_lnk_target(const unsigned char *bytes, int n, hp_char *out, int cap)
{
    int i = 0, len = 0, e, k;
    while (i < n && bytes[i] >= '0' && bytes[i] <= '9' && len < 100000)
        len = len * 10 + (bytes[i++] - '0');
    if (i == 0 || i >= n || bytes[i] != '#')
        return 0;
    i++;
    e = n;                                  // the count is not trusted: take what is there
    if (len > 0 && i + len < e)
        e = i + len;
    while (e > i && (bytes[e - 1] == '\r' || bytes[e - 1] == '\n' || bytes[e - 1] == ' ' || bytes[e - 1] == 0))
        e--;
    if (e - i >= 2 && bytes[i] == '"' && bytes[e - 1] == '"') {
        i++;
        e--;
    }
    if (e - i > 5 && (bytes[i] == 'h' || bytes[i] == 'H' || bytes[i] == 'f' || bytes[i] == 'F') && bytes[i + 4] == ':') {
        static const char *const schemes[2] = { "help:", "file:" };
        int s;
        for (s = 0; s < 2; s++) {
            for (k = 0; k < 5; k++)
                if ((bytes[i + k] | 0x20) != schemes[s][k])
                    break;
            if (k == 5) {
                i += 5;
                break;
            }
        }
    }
    if (e <= i || e - i >= cap)
        return 0;
    for (k = 0; i + k < e; k++)
        out[k] = bytes[i + k];
    out[k] = 0;
    return k;
}

static int contents_put(unsigned char *b, int o, const hp_char *s, int text)
{
    for (; *s; s++) {
        unsigned int c = *s;
        if (c == '<' || c == '>' || c == '"') {
            c = ' ';
        } else if (c == '&' && text) {
            if (b)
                memcpy(b + o, "&amp;", 5);
            o += 5;
            continue;
        }
        if (c < 0x80) {
            if (b)
                b[o] = (unsigned char)c;
            o++;
        } else if (c < 0x800) {
            if (b) {
                b[o] = (unsigned char)(0xC0 | (c >> 6));
                b[o + 1] = (unsigned char)(0x80 | (c & 0x3F));
            }
            o += 2;
        } else {
            if (b) {
                b[o] = (unsigned char)(0xE0 | (c >> 12));
                b[o + 1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
                b[o + 2] = (unsigned char)(0x80 | (c & 0x3F));
            }
            o += 3;
        }
    }
    return o;
}

static int contents_lit(unsigned char *b, int o, const char *s)
{
    int n = (int)strlen(s);
    if (b)
        memcpy(b + o, s, n);
    return o + n;
}

static int contents_build(unsigned char *b, const hp_char *const *names, const hp_char *const *targets, int count)
{
    int o = 0, i, k, anchor;
    o = contents_lit(b, o, "\xEF\xBB\xBF<html><head><meta http-equiv=\"Htm-Help\" content=\"mainhelp.htm#Help_Contents\">"
                           "<title>Help Contents</title></head><body>"
                           "<p><a name=\"Help_Contents\"></a><b>Help Contents</b></p>\r\n");
    for (i = 0; i < count; i++) {
        anchor = 0;
        for (k = 0; targets[i][k]; k++)
            if (targets[i][k] == '#')
                anchor = 1;
        o = contents_lit(b, o, "<a href=\"");
        o = contents_put(b, o, targets[i], 0);
        if (!anchor)
            o = contents_lit(b, o, "#Main_Contents");
        o = contents_lit(b, o, "\">");
        o = contents_put(b, o, names[i], 1);
        o = contents_lit(b, o, "</a><br>\r\n");
    }
    o = contents_lit(b, o, "</body></html>\r\n");
    return o;
}

unsigned char *hp_contents_page(const hp_char *const *names, const hp_char *const *targets, int count, int *size)
{
    int n = contents_build(NULL, names, targets, count);
    unsigned char *b = (unsigned char *)malloc(n + 1);
    if (!b)
        return NULL;
    contents_build(b, names, targets, count);
    b[n] = 0;
    *size = n;
    return b;
}
