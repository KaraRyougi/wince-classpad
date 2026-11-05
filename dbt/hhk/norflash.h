/* norflash.h - program the calculator's NOR from a program running in RAM.
 *
 * The fx-CP400 flash is a 64 MiB x16 AMD/Spansion-command-set device with
 * uniform 128 KiB sectors (CFI as casio-emu models it from the OS driver:
 * src/hardware/flash/flash.c). Offsets are chip offsets (P1 0x80000000 +
 * offset, P2 0xA0000000 + offset); commands and polling go through P2.
 * Every operation masks interrupts (the OS's handlers live in the chip, which
 * returns status instead of code while busy) and verifies by reading back.
 * Erase and program are the only operations; nothing here writes outside the
 * sector or range it was given. */
#ifndef NORFLASH_H
#define NORFLASH_H
#include "util.h"

#define NOR_SIZE        0x04000000u
#define NOR_SECTOR      0x00020000u
#define NOR_P1          0x80000000u
#define NOR_P2          0xA0000000u

int  nor_sector_erased(u32 off);                     /* sector holding off is all 0xFF */
int  nor_range_erased(u32 off, u32 len);
int  nor_erase_sector(u32 off);                      /* 0 ok, -1 timeout/verify */
/* program len bytes (off and len even) into erased flash; 0 ok, -1 on a
 * verify mismatch (a sector that was not erased, a bad chip). */
int  nor_program(u32 off, const u8 *src, u32 len);
#endif
