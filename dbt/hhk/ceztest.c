/* ceztest.c - host test of the loader's image reader (cez.c):
 *   ceztest plain packed [damaged...]
 * `packed` (tools/cepack.py) must read back as `plain` byte for byte, in
 * pieces of odd sizes and after seeks; `plain` read through cez must be
 * itself; every `damaged` file must be refused or fail to read, not crash. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "cez.h"

static u8 *slurp(const char *path, u32 *n)
{
    FILE *f = fopen(path, "rb");
    u8 *p;
    long len;
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    p = malloc((size_t)len + 1);
    if (fread(p, 1, (size_t)len, f) != (size_t)len) { perror(path); exit(2); }
    fclose(f);
    *n = (u32)len;
    return p;
}

static int same(const char *path, const u8 *ref, u32 size)
{
    static u8 buf[70000];
    u32 pos = 0, x = 12345, i;
    int fd = open(path, O_RDONLY), bad = 0;
    if (fd < 0 || cez_open(fd)) { printf("ceztest: %s does not open\n", path); return 1; }
    if (cez_size() != size) { printf("ceztest: %s: size %u, not %u\n", path, cez_size(), size); return 1; }
    while (pos < size && !bad) {                        /* front to back in uneven pieces */
        u32 c;
        x = x * 1664525u + 1013904223u;
        c = (x >> 8) % sizeof buf + 1;
        if (c > size - pos) c = size - pos;
        if (cez_read(buf, c) || memcmp(buf, ref + pos, c)) { printf("ceztest: %s differs near %u\n", path, pos); bad = 1; }
        pos += c;
    }
    for (i = 0; i < 300 && !bad && size > 16; i++) {     /* and here and there */
        u32 c;
        x = x * 1664525u + 1013904223u; pos = (x >> 4) % (size - 12);
        x = x * 1664525u + 1013904223u; c = (x >> 8) % 5000 + 1;
        if (c > size - pos) c = size - pos;
        if (cez_seek(pos) || cez_read(buf, c) || memcmp(buf, ref + pos, c)) { printf("ceztest: %s differs after a seek to %u\n", path, pos); bad = 1; }
    }
    if (!bad && (cez_seek(size) || !cez_read(buf, 1))) { printf("ceztest: %s reads past its end\n", path); bad = 1; }
    cez_close(); close(fd);
    return bad;
}

int main(int argc, char **argv)
{
    static u8 buf[32768];
    u32 size, pos;
    u8 *ref;
    int i, bad = 0;
    if (argc < 3) { fprintf(stderr, "usage: ceztest plain packed [damaged...]\n"); return 2; }
    ref = slurp(argv[1], &size);
    bad |= same(argv[1], ref, size);
    bad |= same(argv[2], ref, size);
    {   /* the packed file's record table against the records of the plain file */
        int fd = open(argv[2], O_RDONLY);
        u32 crc, k = 0, at = 15, addr, len, c;
        if (fd < 0 || cez_open(fd)) { printf("ceztest: %s does not open\n", argv[2]); bad = 1; }
        else {
            if (!cez_file_crc(&crc) || crc != cez_crc32(0, ref, size)) { printf("ceztest: the file CRC is missing or wrong\n"); bad = 1; }
            if (size > 15 && !memcmp(ref, "B000FF\n", 7)) {
                for (;; k++) {
                    u32 a = le32(ref + at), l = le32(ref + at + 4), chk = le32(ref + at + 8);
                    at += 12;
                    if (a == 0 && chk == 0) break;
                    if (k >= cez_records()) { bad = 1; break; }
                    cez_record(k, &addr, &len, &c);
                    if (addr != a || len != l || c != cez_crc32(cez_crc32(0, ref + at, l / 2), ref + at + l / 2, l - l / 2)) { printf("ceztest: record %u of the table is wrong\n", k); bad = 1; }
                    at += l;
                }
                if (k != cez_records() || !k) { printf("ceztest: %u records in the table, %u in the file\n", cez_records(), k); bad = 1; }
            } else if (cez_records()) { printf("ceztest: a record table for a file without records\n"); bad = 1; }
        }
        cez_close(); if (fd >= 0) close(fd);
    }
    for (i = 3; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY), ok = 1;
        if (cez_open(fd)) ok = 0;
        else if (!cez_packed()) { printf("ceztest: %s is not taken for a packed file\n", argv[i]); bad = 1; }
        else for (pos = 0; pos < cez_size() && ok; pos += sizeof buf) {
            u32 c = cez_size() - pos < sizeof buf ? cez_size() - pos : sizeof buf;
            if (cez_read(buf, c)) ok = 0;
            else if (pos + c <= size && memcmp(buf, ref + pos, c)) ok = 0;   /* wrong bytes: for the record checksums to catch */
        }
        if (ok) { printf("ceztest: %s reads as the original\n", argv[i]); bad = 1; }
        cez_close(); close(fd);
    }
    if (!bad) printf("ceztest: ok\n");
    return bad;
}
