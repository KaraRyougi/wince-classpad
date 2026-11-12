/* cez.c - an image file in the storage memory read as a plain byte stream,
 * whether it is the file Platform Builder wrote (B000FF) or that file packed
 * by tools/cepack.py ("CEZ2", or the older "CEZ1": blocks of CEZ_BLOCK
 * bytes, each a raw deflate stream of its own, behind a table of their
 * lengths and, in CEZ2, a table of the B000FF records with their CRC-32s;
 * the format is in the tool). The flasher (nkflash.c) seeks and reads as it did on the plain
 * file; a packed one is unpacked a block at a time into one buffer, so a
 * seek costs at most one block and the whole thing needs CEZ_BLOCK + 4 KiB
 * of memory on top of the table.
 *
 * The inflate is a small one of the usual kind: canonical Huffman codes
 * decoded through a 9-bit table, with the longer codes walked bit by bit. */
#include <stdlib.h>
#include <unistd.h>
#include "cez.h"

#define CEZ_BLOCK   32768u
#define CEZ_RAW     0x80000000u
#define CEZ_MAXBLK  4096u           /* 128 MiB: far beyond any window */
#define INBUF       4096u

static struct {
    int fd, packed;
    u32 size, pos;                  /* of the unpacked stream */
    u32 nblocks;
    u32 *len;                       /* per block: stored length | CEZ_RAW */
    u32 *off;                       /* per block: where it starts in the file */
    u8 *blk;                        /* the block unpacked last */
    u32 cur, curlen;                /* its number (nblocks: none) and length */
    u8 *in;
    u32 nrec;                       /* CEZ2: the record table, 3 words each */
    u32 *rec;
    u32 crc; int has_crc;
} z = { -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static int read_all(int fd, void *buf, u32 n)
{
    u8 *p = buf;
    while (n) {
        int r = (int)read(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (u32)r;
    }
    return 0;
}

/* --- inflate --- */

static struct {
    u32 left;                       /* bytes of the block still in the file */
    u32 have, at;                   /* in z.in */
    u32 bits; int nbits;
    int zeros;                      /* bytes made up past the end */
    int bad;
} in;

static void need(int n)
{
    while (in.nbits < n) {
        u32 b = 0;
        if (in.at == in.have && in.left && !in.bad) {
            u32 c = in.left > INBUF ? INBUF : in.left;
            if (read_all(z.fd, z.in, c)) in.bad = 1;
            else { in.have = c; in.at = 0; in.left -= c; }
        }
        if (in.at < in.have) b = z.in[in.at++];
        else in.zeros++;            /* a peek past the end is fine as long as it is not used */
        in.bits |= b << in.nbits;
        in.nbits += 8;
    }
}

static u32 take(int n)
{
    u32 v;
    if (!n) return 0;
    need(n);
    v = in.bits & ((1u << n) - 1);
    in.bits >>= n; in.nbits -= n;
    return v;
}

#define FAST 9
typedef struct {
    u16 count[16], symbol[288];
    u16 fast[1 << FAST];            /* symbol << 4 | length; 0: a longer code */
} huff;

static int build(huff *h, const u8 *lens, int n)
{
    u16 offs[16], next[16];
    int i, len, left = 1, code = 0;
    for (len = 0; len < 16; len++) h->count[len] = 0;
    for (i = 0; i < n; i++) h->count[lens[i]]++;
    for (i = 0; i < (1 << FAST); i++) h->fast[i] = 0;
    if (h->count[0] == n) return 0;                     /* no codes: fine until one is used */
    for (len = 1; len < 16; len++) {
        left <<= 1; left -= h->count[len];
        if (left < 0) return -1;                        /* over-subscribed */
    }
    offs[1] = 0;
    for (len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (len = 1; len < 16; len++) { next[len] = (u16)code; code = (code + h->count[len]) << 1; }
    for (i = 0; i < n; i++) {
        len = lens[i];
        if (!len) continue;
        h->symbol[offs[len]++] = (u16)i;
        if (len <= FAST) {
            int c = next[len], r = 0, b;
            for (b = 0; b < len; b++) r |= ((c >> b) & 1) << (len - 1 - b);   /* the stream has the code's top bit first */
            for (; r < (1 << FAST); r += 1 << len) h->fast[r] = (u16)(i << 4 | len);
        }
        next[len]++;
    }
    return 0;
}

static int decode(const huff *h)
{
    int code = 0, first = 0, index = 0, len, e;
    need(15);
    e = h->fast[in.bits & ((1 << FAST) - 1)];
    if (e) { in.bits >>= e & 15; in.nbits -= e & 15; return e >> 4; }
    for (len = 1; len <= 15; len++) {
        int count = h->count[len];
        code |= (int)((in.bits >> (len - 1)) & 1);
        if (code - count < first) { in.bits >>= len; in.nbits -= len; return h->symbol[index + (code - first)]; }
        index += count; first += count;
        first <<= 1; code <<= 1;
    }
    return -1;
}

static huff lencode, distcode;

static int codes(u8 *out, u32 cap, u32 *n)
{
    static const u16 lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const u8 lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const u16 dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const u8 dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    u32 o = *n;
    for (;;) {
        int sym = decode(&lencode);
        if (sym < 0 || in.bad) return -1;
        if (sym < 256) {
            if (o >= cap) return -1;
            out[o++] = (u8)sym;
        } else if (sym == 256) {
            break;
        } else {
            u32 len, dist;
            sym -= 257;
            if (sym >= 29) return -1;
            len = lbase[sym] + take(lext[sym]);
            sym = decode(&distcode);
            if (sym < 0 || sym >= 30) return -1;
            dist = dbase[sym] + take(dext[sym]);
            if (dist > o || len > cap - o) return -1;
            for (; len; len--, o++) out[o] = out[o - dist];
        }
    }
    *n = o;
    return 0;
}

static int fixed_tables(void)
{
    u8 lens[288];
    int i;
    for (i = 0; i < 144; i++) lens[i] = 8;
    for (; i < 256; i++) lens[i] = 9;
    for (; i < 280; i++) lens[i] = 7;
    for (; i < 288; i++) lens[i] = 8;
    if (build(&lencode, lens, 288)) return -1;
    for (i = 0; i < 30; i++) lens[i] = 5;
    return build(&distcode, lens, 30);
}

static int dynamic_tables(void)
{
    static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    u8 lens[320];
    int nlen = (int)take(5) + 257, ndist = (int)take(5) + 1, ncode = (int)take(4) + 4, i;
    if (nlen > 286 || ndist > 30) return -1;
    for (i = 0; i < ncode; i++) lens[order[i]] = (u8)take(3);
    for (; i < 19; i++) lens[order[i]] = 0;
    if (build(&lencode, lens, 19)) return -1;
    for (i = 0; i < nlen + ndist; ) {
        int sym = decode(&lencode), rep, v = 0;
        if (sym < 0 || in.bad) return -1;
        if (sym < 16) { lens[i++] = (u8)sym; continue; }
        if (sym == 16) { if (!i) return -1; v = lens[i - 1]; rep = 3 + (int)take(2); }
        else if (sym == 17) rep = 3 + (int)take(3);
        else rep = 11 + (int)take(7);
        if (i + rep > nlen + ndist) return -1;
        while (rep--) lens[i++] = (u8)v;
    }
    if (!lens[256]) return -1;
    if (build(&distcode, lens + nlen, ndist)) return -1;    /* before lencode: it holds the code lengths' code */
    return build(&lencode, lens, nlen);
}

/* srclen bytes of the file from its position, one deflate stream: the bytes written or -1 */
static int inflate(u32 srclen, u8 *out, u32 cap)
{
    u32 n = 0;
    int last;
    in.left = srclen; in.have = in.at = 0; in.bits = 0; in.nbits = 0; in.zeros = 0; in.bad = 0;
    do {
        last = (int)take(1);
        switch (take(2)) {
        case 0: {
            u32 len, nlen;
            in.bits >>= in.nbits & 7; in.nbits -= in.nbits & 7;
            len = take(16); nlen = take(16);
            if (len != (~nlen & 0xFFFF) || len > cap - n) return -1;
            for (; len; len--) out[n++] = (u8)take(8);
            break;
        }
        case 1:
            if (fixed_tables() || codes(out, cap, &n)) return -1;
            break;
        case 2:
            if (dynamic_tables() || codes(out, cap, &n)) return -1;
            break;
        default:
            return -1;
        }
        if (in.bad || in.zeros * 8 > in.nbits) return -1;   /* ran past the end of the block */
    } while (!last);
    return (int)n;
}

/* --- the stream --- */

void cez_close(void)
{
    if (z.len) free(z.len);
    if (z.off) free(z.off);
    if (z.blk) free(z.blk);
    if (z.in) free(z.in);
    if (z.rec) free(z.rec);
    z.len = z.off = z.rec = 0; z.blk = z.in = 0;
    z.fd = -1; z.packed = 0; z.nrec = 0; z.has_crc = 0;
}

int cez_open(int fd)
{
    u8 head[16], w[12];
    u32 i, bsize, at, v2;
    off_t end = lseek(fd, 0, SEEK_END);
    cez_close();
    if (end < 0 || lseek(fd, 0, SEEK_SET) != 0) return -1;
    z.fd = fd; z.pos = 0; z.size = (u32)end;
    if (z.size < 16 || read_all(fd, head, 16)) return lseek(fd, 0, SEEK_SET) == 0 ? 0 : -1;
    v2 = !memcmp(head, "CEZ2", 4);
    if (!v2 && memcmp(head, "CEZ1", 4)) return lseek(fd, 0, SEEK_SET) == 0 ? 0 : -1;
    z.size = le32(head + 4); bsize = le32(head + 8); z.nblocks = le32(head + 12);
    if (v2) {
        if (read_all(fd, w, 8)) goto bad;
        z.nrec = le32(w); z.crc = le32(w + 4); z.has_crc = 1;
        if (z.nrec > 4096) goto bad;
    }
    if (bsize != CEZ_BLOCK || z.nblocks > CEZ_MAXBLK || z.nblocks != (z.size + CEZ_BLOCK - 1) / CEZ_BLOCK) goto bad;
    z.len = malloc((z.nblocks + 1) * sizeof *z.len);
    z.off = malloc((z.nblocks + 1) * sizeof *z.off);
    z.blk = malloc(CEZ_BLOCK);
    z.in = malloc(INBUF);
    if (!z.len || !z.off || !z.blk || !z.in) goto bad;
    at = (v2 ? 24 : 16) + 4 * z.nblocks + 12 * z.nrec;
    if (z.nrec && !(z.rec = malloc(z.nrec * 3 * sizeof *z.rec))) goto bad;
    for (i = 0; i < z.nblocks; i++) {
        if (read_all(fd, w, 4)) goto bad;
        z.len[i] = le32(w);
        z.off[i] = at;
        if ((z.len[i] & ~CEZ_RAW) > CEZ_BLOCK) goto bad;
        at += z.len[i] & ~CEZ_RAW;
    }
    if (at != (u32)end) goto bad;                       /* cut short, or something else */
    for (i = 0; i < z.nrec * 3; i++) {
        if (read_all(fd, w, 4)) goto bad;
        z.rec[i] = le32(w);
    }
    z.packed = 1; z.cur = z.nblocks;
    return 0;
bad:
    cez_close();
    return -1;
}

u32 cez_size(void) { return z.size; }
int cez_packed(void) { return z.packed; }

int cez_seek(u32 pos)
{
    if (pos > z.size) return -1;
    z.pos = pos;
    if (!z.packed) return lseek(z.fd, (off_t)pos, SEEK_SET) < 0 ? -1 : 0;
    return 0;
}

static int load(u32 b)
{
    u32 want = b + 1 < z.nblocks ? CEZ_BLOCK : z.size - b * CEZ_BLOCK, len = z.len[b] & ~CEZ_RAW;
    int n;
    z.cur = z.nblocks;
    if (lseek(z.fd, (off_t)z.off[b], SEEK_SET) < 0) return -1;
    if (z.len[b] & CEZ_RAW) n = read_all(z.fd, z.blk, len) ? -1 : (int)len;
    else n = inflate(len, z.blk, CEZ_BLOCK);
    if (n < 0 || (u32)n != want) return -1;
    z.cur = b; z.curlen = want;
    return 0;
}

int cez_read(void *buf, u32 n)
{
    u8 *p = buf;
    if (n > z.size - z.pos) return -1;
    if (!z.packed) {
        if (read_all(z.fd, p, n)) return -1;
        z.pos += n;
        return 0;
    }
    while (n) {
        u32 b = z.pos / CEZ_BLOCK, o = z.pos % CEZ_BLOCK, c;
        if (b != z.cur && load(b)) return -1;
        c = z.curlen - o < n ? z.curlen - o : n;
        memcpy(p, z.blk + o, c);
        p += c; n -= c; z.pos += c;
    }
    return 0;
}

/* --- what the packer wrote down --- */

u32 cez_records(void) { return z.packed ? z.nrec : 0; }

void cez_record(u32 i, u32 *addr, u32 *len, u32 *crc)
{
    *addr = z.rec[3 * i]; *len = z.rec[3 * i + 1]; *crc = z.rec[3 * i + 2];
}

int cez_file_crc(u32 *crc)
{
    if (!z.packed || !z.has_crc) return 0;
    *crc = z.crc;
    return 1;
}

/* CRC-32 as zlib's (reflected 0xEDB88320) */
u32 cez_crc32(u32 crc, const void *p, u32 n)
{
    static u32 table[256];
    const u8 *b = p;
    u32 i, j, c;
    if (!table[1]) {
        for (i = 0; i < 256; i++) {
            for (c = i, j = 0; j < 8; j++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    for (i = 0; i < n; i++) crc = table[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
