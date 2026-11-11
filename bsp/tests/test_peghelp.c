// Host tests for peghelp.exe's HTML core (bsp/classpad50/SRC/APPS/PEGHELP/hpcore.c)
// with fixed-pitch metrics:
//   test_peghelp                 built-in cases
//   test_peghelp DIR             also every *.htm in DIR (e.g. Math Xpander's
//                                \Windows files): every link target and anchor
//                                must resolve, no words overlap on a line
//   test_peghelp -d FILE [W]     print FILE's layout as text lines (width W)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>
#include "../classpad50/SRC/APPS/PEGHELP/hpcore.h"

static int g_fail;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static void Font(void *ctx, int font, int *h, int *a)
{
    static const int hs[4] = { 9, 12, 14, 17 };
    (void)ctx;
    *h = hs[HP_FONT_SIZE(font)];
    *a = *h * 3 / 4;
}

static int Width(void *ctx, int font, const hp_char *s, int n)
{
    int per = 4 + HP_FONT_SIZE(font) + ((font & HP_FONT_BOLD) ? 1 : 0);
    (void)ctx; (void)s;
    return n * per;
}

static int Image(void *ctx, const hp_char *src, int n, int *w, int *h)
{
    (void)ctx; (void)src; (void)n;
    *w = 22;
    *h = 21;
    return 1;
}

static const hp_metrics g_m = { NULL, Font, Width, Image };

static void ToAscii(const hp_char *s, int n, char *out, int cap)
{
    int i, o = 0;
    for (i = 0; i < n && o + 1 < cap; i++)
        out[o++] = s[i] < 0x80 ? (char)s[i] : (s[i] == 0xA0 ? ' ' : '~');
    out[o] = 0;
}

static unsigned char *ReadFile(const char *path, int *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *n = (int)len;
    return b;
}

static hp_doc *ParseStr(const char *html)
{
    return hp_parse((const unsigned char *)html, (int)strlen(html));
}

static void Dump(hp_doc *d)
{
    int n, i, j, used[4096];
    const hp_item *it = hp_items(d, &n);
    memset(used, 0, sizeof used);
    // print items grouped by line top, in order
    for (i = 0; i < n && i < 4096; i++) {
        char line[512];
        int col = 0;
        if (used[i])
            continue;
        memset(line, ' ', sizeof line);
        for (j = i; j < n && j < 4096; j++) {
            int len, k, x;
            char a[256];
            const hp_char *s;
            if (used[j] || it[j].y + it[j].h / 2 < it[i].y || it[j].y > it[i].y + it[i].h / 2)
                continue;
            used[j] = 1;
            x = it[j].x / 4;
            if (it[j].kind == HP_RULE) {
                for (k = 0; k < it[j].w / 4 && x + k < 500; k++)
                    line[x + k] = '-';
                if (x + k > col) col = x + k;
                continue;
            }
            s = hp_item_text(d, &it[j], &len);
            if (it[j].kind == HP_IMAGE)
                snprintf(a, sizeof a, "[img]");
            else
                ToAscii(s, len, a, sizeof a);
            for (k = 0; a[k] && x + k < 500; k++)
                line[x + k] = a[k];
            if (x + k > col) col = x + k;
        }
        line[col] = 0;
        printf("%5d %s\n", it[i].y, line);
    }
}

static int Anchor(hp_doc *d, const char *name)
{
    hp_char w[128];
    int i, n = (int)strlen(name);
    for (i = 0; i < n && i < 128; i++)
        w[i] = (hp_char)(unsigned char)name[i];
    return hp_anchor(d, w, n);
}

static int CountText(hp_doc *d, const char *word)
{
    int n, i, c = 0;
    const hp_item *it = hp_items(d, &n);
    for (i = 0; i < n; i++) {
        int len;
        char a[256];
        const hp_char *s = hp_item_text(d, &it[i], &len);
        if (it[i].kind != HP_TEXT)
            continue;
        ToAscii(s, len, a, sizeof a);
        if (!strcmp(a, word))
            c++;
    }
    return c;
}

static const hp_item *FindText(hp_doc *d, const char *word)
{
    int n, i;
    const hp_item *it = hp_items(d, &n);
    for (i = 0; i < n; i++) {
        int len;
        char a[256];
        const hp_char *s = hp_item_text(d, &it[i], &len);
        if (it[i].kind != HP_TEXT && it[i].kind != HP_BULLET)
            continue;
        ToAscii(s, len, a, sizeof a);
        if (!strcmp(a, word))
            return &it[i];
    }
    return NULL;
}

// words on one line never overlap and stay inside the view
static void CheckGeometry(hp_doc *d, const char *what, int width)
{
    int n, i;
    const hp_item *it = hp_items(d, &n);
    for (i = 0; i < n; i++) {
        CHECK(it[i].x >= 0, "%s: item %d at x %d", what, i, it[i].x);
        if (it[i].kind == HP_TEXT && it[i].w < width - 8)
            CHECK(it[i].x + it[i].w <= width, "%s: item %d ends at %d > %d", what, i, it[i].x + it[i].w, width);
        if (i && it[i].kind == HP_TEXT && it[i - 1].kind == HP_TEXT && it[i].y == it[i - 1].y)
            CHECK(it[i].x >= it[i - 1].x + it[i - 1].w, "%s: items %d and %d overlap", what, i - 1, i);
    }
}

static void Builtin(void)
{
    hp_doc *d;
    const hp_item *a, *b;
    int n, h;
    const hp_char *t;

    d = ParseStr("<html><head><title> Hello  </title></head><body>One two&nbsp;three &amp; &#151; &#x03c0;</body></html>");
    CHECK(d != NULL, "parse");
    t = hp_title(d, &n);
    CHECK(t && n == 5 && t[0] == 'H', "title");
    hp_layout(d, &g_m, 320);
    CHECK(CountText(d, "Hello") == 0, "title text is not in the body");
    CHECK(CountText(d, "two three") == 1, "nbsp glues words");
    CHECK(CountText(d, "&") == 1, "&amp;");
    a = FindText(d, "~");
    CHECK(a != NULL, "decoded entities");
    hp_free(d);

    // wrapping: 40 five-letter words at 5 px per char, 104 px wide view
    {
        char buf[1024] = "";
        int i, lines = 0, lastY = -1;
        const hp_item *it;
        for (i = 0; i < 40; i++)
            strcat(buf, "abcde ");
        d = ParseStr(buf);
        hp_layout(d, &g_m, 104);
        CheckGeometry(d, "wrap", 104);
        it = hp_items(d, &n);
        CHECK(n == 40, "40 words, got %d", n);
        for (i = 0; i < n; i++)
            if (it[i].y != lastY) {
                lines++;
                lastY = it[i].y;
            }
        // 96 px of text per line: 3 words (25 + 5 + 25 + 5 + 25 = 85, a 4th would be 115)
        CHECK(lines == 14, "lines %d", lines);
        hp_free(d);
    }

    // anchors, links and the Htm-Help meta
    d = ParseStr("<meta http-equiv=\"Htm-Help\" content=\"x.htm#Main\">"
                 "<a name=\"Main\"></a>Top<p>para<br><br><A HREF=\"x.htm#Sub\">go there</a>"
                 "<p><a name=sub>Sub</a> text");
    h = hp_layout(d, &g_m, 320);
    CHECK(Anchor(d, "Main") == 4, "anchor Main at %d", Anchor(d, "Main"));
    CHECK(Anchor(d, "SUB") > 40, "anchor Sub at %d", Anchor(d, "SUB"));
    CHECK(Anchor(d, "nope") == -1, "missing anchor");
    t = hp_contents(d, &n);
    CHECK(t && n == 10 && t[0] == 'x', "contents");
    CHECK(Anchor(d, "Subsection") == -1, "prefix fallback needs 4 chars");
    CHECK(Anchor(d, "Mainly") == 4, "prefix fallback");
    a = FindText(d, "go");
    CHECK(a && a->link >= 0 && (a->font & HP_FONT_UNDERLINE), "link word");
    if (a) {
        CHECK(hp_hit(d, a->x + 1, a->y + 1) == a->link, "hit");
        t = hp_href(d, a->link, &n);
        CHECK(t && n == 9 && t[6] == 'S', "href");
    }
    CHECK(hp_hit(d, 1, h - 1) == -1, "no link at the bottom");
    b = FindText(d, "there");
    CHECK(a && b && b->lead == b->x - (a->x + a->w) && b->lead > 0, "a link's underline runs over the space between its words");
    CHECK(a && a->lead == 0, "no underline before a link");
    if (b)
        CHECK(hp_hit(d, b->x - 1, b->y + 1) == b->link, "the space inside a link hits it");
    hp_free(d);

    // only whitespace inside the link (or <u>) is underlined
    d = ParseStr("x <a href=a.htm>one</a> <a href=b.htm>two</a> y <u>p q</u> <a href=c.htm>m </a>n");
    hp_layout(d, &g_m, 320);
    a = FindText(d, "one");
    CHECK(a && a->lead == 0, "space before a link");
    a = FindText(d, "two");
    CHECK(a && a->lead == 0, "space between two links");
    a = FindText(d, "y");
    CHECK(a && a->lead == 0, "space after a link");
    a = FindText(d, "q");
    CHECK(a && a->lead > 0, "space inside <u>");
    a = FindText(d, "n");
    CHECK(a && a->lead == 0, "trailing link space before plain text");
    hp_free(d);

    // lists: bullets left of the text, nesting indents, ordered numbers
    d = ParseStr("<ul><li>one<ul><li>inner</ul><li>two</ul><ol><li>a<li>b</ol>");
    hp_layout(d, &g_m, 320);
    a = FindText(d, "one");
    b = FindText(d, "inner");
    CHECK(a && b && b->x > a->x, "nested list indents");
    CHECK(FindText(d, "2.") != NULL, "ordered numbering");
    a = FindText(d, "two");
    b = FindText(d, "one");
    CHECK(a && b && a->x == b->x && a->y > b->y, "list item after a nested list");
    hp_free(d);

    // tables: columns side by side, rows below each other, header bold
    d = ParseStr("<table cellpadding=4><tr><th>Field</th><th>Description</th></tr>"
                 "<tr><td>Horiz: Min</td><td>Lower boundary of the <i>x</i> axis</td></tr>"
                 "<tr><td>Vert</td><td>Upper</td></tr></table>after");
    hp_layout(d, &g_m, 320);
    CheckGeometry(d, "table", 320);
    a = FindText(d, "Field");
    b = FindText(d, "Description");
    CHECK(a && b && a->y == b->y && b->x > a->x + a->w, "header cells side by side");
    CHECK(a && (a->font & HP_FONT_BOLD), "th is bold");
    a = FindText(d, "Lower");
    b = FindText(d, "Upper");
    CHECK(a && b && b->y > a->y && a->x == b->x, "rows stack, columns align");
    a = FindText(d, "after");
    CHECK(a && b && a->y > b->y && a->x == 4, "text after the table");
    hp_free(d);

    // a narrow view squeezes the long column, not the short one
    d = ParseStr("<table><tr><td>Key</td><td>a long description that has to wrap over several lines</td></tr></table>");
    hp_layout(d, &g_m, 160);
    CheckGeometry(d, "narrow table", 160);
    a = FindText(d, "Key");
    b = FindText(d, "lines");
    CHECK(a && b && b->y > a->y, "second column wraps");
    hp_free(d);

    // sup/sub raise and lower, images take their size
    d = ParseStr("x<sup>2</sup> y<sub>i</sub> <img src=\"k.2BP\"> <img src=a.bmp width=5 height=6>");
    hp_layout(d, &g_m, 320);
    a = FindText(d, "x");
    b = FindText(d, "2");
    CHECK(a && b && b->y < a->y && b->x == a->x + a->w, "sup is raised and glued");
    {
        const hp_item *it = hp_items(d, &n);
        int i, imgs = 0;
        for (i = 0; i < n; i++)
            if (it[i].kind == HP_IMAGE) {
                imgs++;
                CHECK((it[i].w == 22 && it[i].h == 21) || (it[i].w == 5 && it[i].h == 6), "image size");
            }
        CHECK(imgs == 2, "two images, got %d", imgs);
    }
    hp_free(d);

    // comments, doctype, scripts and stray '<' do not show
    d = ParseStr("<!DOCTYPE x><!-- <b>hidden</b> --->a < b<script>var q='<p>';</script>c");
    hp_layout(d, &g_m, 320);
    CHECK(CountText(d, "hidden") == 0 && CountText(d, "var") == 0, "comment/script hidden");
    CHECK(CountText(d, "<") == 1 && CountText(d, "c") == 1, "stray <");
    hp_free(d);
}

static int EndsWith(const char *s, const char *e)
{
    size_t a = strlen(s), b = strlen(e);
    return a >= b && !strcasecmp(s + a - b, e);
}

static void SplitHref(const hp_char *h, int n, char *file, char *anchor)
{
    int i, f = 0, a = 0, inAnchor = 0;
    for (i = 0; i < n; i++) {
        if (h[i] == '#' && !inAnchor) {
            inAnchor = 1;
            continue;
        }
        if (inAnchor) {
            if (a < 127) anchor[a++] = (char)h[i];
        } else if (f < 255) {
            file[f++] = (char)h[i];
        }
    }
    file[f] = 0;
    anchor[a] = 0;
}

static hp_doc *Load(const char *dir, const char *name)
{
    char path[1024];
    DIR *dd;
    struct dirent *e;
    unsigned char *b;
    int n;
    hp_doc *d = NULL;
    // case-insensitive file name, like CE's file system
    dd = opendir(dir);
    if (!dd)
        return NULL;
    while ((e = readdir(dd)))
        if (!strcasecmp(e->d_name, name)) {
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            b = ReadFile(path, &n);
            if (b) {
                d = hp_parse(b, n);
                free(b);
            }
            break;
        }
    closedir(dd);
    if (d)
        hp_layout(d, &g_m, 300);
    return d;
}

static void Files(const char *dir)
{
    DIR *dd = opendir(dir);
    struct dirent *e;
    int files = 0, links = 0;
    if (!dd) {
        printf("skip: no %s\n", dir);
        return;
    }
    while ((e = readdir(dd))) {
        hp_doc *d;
        int n, i, len;
        const hp_item *it;
        const hp_char *t;
        if (!EndsWith(e->d_name, ".htm"))
            continue;
        d = Load(dir, e->d_name);
        CHECK(d != NULL, "%s: parse", e->d_name);
        if (!d)
            continue;
        files++;
        CHECK(hp_title(d, &len) != NULL, "%s: title", e->d_name);
        CheckGeometry(d, e->d_name, 300);
        t = hp_contents(d, &len);
        if (t) {
            char f[256], a[128];
            SplitHref(t, len, f, a);
            CHECK(!*a || Anchor(d, a) >= 0, "%s: Htm-Help anchor %s", e->d_name, a);
        }
        it = hp_items(d, &n);
        for (i = 0; i < n; i++) {
            char f[256], a[128];
            const hp_char *h;
            hp_doc *target;
            if (it[i].link < 0 || (i && it[i - 1].link == it[i].link))
                continue;
            h = hp_href(d, it[i].link, &len);
            CHECK(h != NULL, "%s: href", e->d_name);
            if (!h)
                continue;
            links++;
            SplitHref(h, len, f, a);
            target = *f && strcasecmp(f, e->d_name) ? Load(dir, f) : d;
            CHECK(target != NULL, "%s: link to missing %s", e->d_name, f);
            if (target && *a)
                CHECK(Anchor(target, a) >= 0, "%s: link to %s#%s: no anchor", e->d_name, *f ? f : "", a);
            if (target && target != d)
                hp_free(target);
        }
        hp_free(d);
    }
    closedir(dd);
    printf("%d help files, %d links checked\n", files, links);
}

// The Help Contents page: shortcut targets as Microsoft's files and the
// shell write them, and the page built from them.
static void ContentsPage(void)
{
    static const struct { const char *bytes; const char *target; } lnk[] = {
        { "18#\\windows\\wince.htm", "\\windows\\wince.htm" },               // PUBLIC\WCESHELLFE\OAK\FILES\wincehlp.lnk
        { "17#\\windows\\word.htm\r\n", "\\windows\\word.htm" },
        { "20#\"\\Windows\\ctpnl.htm\"", "\\Windows\\ctpnl.htm" },           // the shell quotes
        { "23#help:\\windows\\rnetw.htm", "\\windows\\rnetw.htm" },
        { "9#math1.htm#findroot and more", "math1.htm" },                  // the count ends it
        { "0#\\windows\\copy.htm", "\\windows\\copy.htm" },
        { "\\windows\\copy.htm", NULL },
        { "12", NULL },
        { "5#", NULL },
    };
    static const hp_char n0[] = { 'S', 'e', 't', 't', 'i', 'n', 'g', 's', 0 }, t0[] = { '\\', 'w', '\\', 'c', '.', 'h', 't', 'm', 0 };
    static const hp_char n1[] = { 'A', ' ', '&', ' ', 'B', '<', 0 }, t1[] = { 'm', '.', 'h', 't', 'm', '#', 't', 'o', 'p', 0 };
    const hp_char *names[2], *targets[2];
    hp_char out[64];
    char a[128];
    unsigned char *b;
    hp_doc *d;
    const hp_item *it;
    const hp_char *t;
    unsigned i;
    int n, count, links = 0, k;

    for (i = 0; i < sizeof lnk / sizeof lnk[0]; i++) {
        n = hp_lnk_target((const unsigned char *)lnk[i].bytes, (int)strlen(lnk[i].bytes), out, 64);
        ToAscii(out, n, a, sizeof a);
        if (lnk[i].target)
            CHECK(n == (int)strlen(lnk[i].target) && !strcmp(a, lnk[i].target), "shortcut %u: target '%s'", i, a);
        else
            CHECK(n == 0, "shortcut %u: not a shortcut, got '%s'", i, a);
    }
    CHECK(hp_lnk_target((const unsigned char *)"18#\\windows\\wince.htm", 21, out, 8) == 0, "a target longer than the buffer");

    names[0] = n0; targets[0] = t0;
    names[1] = n1; targets[1] = t1;
    b = hp_contents_page(names, targets, 2, &n);
    CHECK(b != NULL && n > 0, "contents page");
    d = hp_parse(b, n);
    free(b);
    hp_layout(d, &g_m, 300);
    t = hp_title(d, &n);
    ToAscii(t, t ? n : 0, a, sizeof a);
    CHECK(!strcmp(a, "Help Contents"), "contents title '%s'", a);
    t = hp_contents(d, &n);
    ToAscii(t, t ? n : 0, a, sizeof a);
    CHECK(!strcmp(a, "mainhelp.htm#Help_Contents"), "contents meta '%s'", a);
    CHECK(Anchor(d, "help_contents") >= 0, "the Help_Contents anchor");
    CHECK(CountText(d, "Settings") == 1 && CountText(d, "&") == 1 && CountText(d, "B") == 1, "one line per shortcut");
    it = hp_items(d, &count);
    for (k = 0; k < count; k++) {
        if (it[k].link < 0 || (k > 0 && it[k - 1].link == it[k].link))
            continue;
        t = hp_href(d, it[k].link, &n);
        ToAscii(t, n, a, sizeof a);
        CHECK(!strcmp(a, links == 0 ? "\\w\\c.htm#Main_Contents" : "m.htm#top"), "link %d goes to '%s'", links, a);
        links++;
    }
    CHECK(links == 2, "%d links in the contents page", links);
    hp_free(d);

    b = hp_contents_page(names, targets, 0, &n);       // an empty Help folder: the heading alone
    d = hp_parse(b, n);
    free(b);
    hp_layout(d, &g_m, 300);
    CHECK(CountText(d, "Help") == 1 && CountText(d, "Contents") == 1, "empty contents page");
    hp_free(d);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "-d")) {
        int n;
        unsigned char *b = ReadFile(argv[2], &n);
        hp_doc *d;
        if (!b) {
            printf("cannot read %s\n", argv[2]);
            return 1;
        }
        d = hp_parse(b, n);
        hp_layout(d, &g_m, argc > 3 ? atoi(argv[3]) : 300);
        Dump(d);
        hp_free(d);
        free(b);
        return 0;
    }
    Builtin();
    ContentsPage();
    if (argc > 1)
        Files(argv[1]);
    printf(g_fail ? "test_peghelp: %d FAILED\n" : "test_peghelp: all passed\n", g_fail);
    return g_fail != 0;
}
