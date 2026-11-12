/* cez.h - an image file read as a plain byte stream, packed or not (cez.c) */
#ifndef HHK_CEZ_H
#define HHK_CEZ_H
#include "util.h"

int  cez_open(int fd);               /* 0, or -1: a packed file that is damaged, or no memory */
u32  cez_size(void);                 /* bytes of the (unpacked) file */
int  cez_packed(void);
int  cez_seek(u32 pos);              /* 0, or -1 past the end */
int  cez_read(void *buf, u32 n);     /* exactly n bytes from the position: 0, or -1 */
void cez_close(void);                /* frees the buffers; the fd stays the caller's */

/* what tools/cepack.py wrote down about the file ("CEZ2"), so that nothing
 * has to be unpacked to know it: the B000FF records and their CRC-32s */
u32  cez_records(void);              /* 0: no table (a plain file, CEZ1, or not B000FF) */
void cez_record(u32 i, u32 *addr, u32 *len, u32 *crc);
int  cez_file_crc(u32 *crc);         /* 1: the CRC-32 of the unpacked file is known */
u32  cez_crc32(u32 crc, const void *p, u32 n);   /* start with 0, feed pieces */
#endif
