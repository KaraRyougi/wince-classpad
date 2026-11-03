//------------------------------------------------------------------------------
//  classpad_lcd.h - panel geometry and the R61524 data path
//
//  The controller has no memory-mapped frame buffer: pixels are streamed
//  through a 16-bit data port at 0xB4000000 (P2), with Port R bit 4
//  (PRDR 0xA405013C, 8-bit) selecting register (0) or data (1). Under the
//  translator 16-bit stores are byte-swapped like memory, so the driver
//  pre-swaps every halfword it writes (docs/04, guest/mmio.h); byte accesses
//  need nothing. The register protocol is the one of the `lcd` guest.
//
//  Emulated fx-CG100 panel: 396 GRAM columns x 224 rows, visible window
//  columns 6..389 and rows 0..215 (384x216). The window registers 0x210/0x211
//  take mirrored columns (395 - x). fx-CP400 (320x528) is not wired yet: set
//  the numbers below for it and check its controller's window addressing.
//------------------------------------------------------------------------------
#ifndef __CLASSPAD_LCD_H
#define __CLASSPAD_LCD_H

#define LCD_WIDTH           384
#define LCD_HEIGHT          216
#define LCD_COL0            6               // first visible GRAM column
#define LCD_GRAM_COLS       396
#define LCD_DATA_PORT       0xB4000000      // 16-bit, P2
#define LCD_PRDR            0xA405013C      // Port R data register, bit 4 = RS

#define LCD_REG_HADDR       0x200
#define LCD_REG_VADDR       0x201
#define LCD_REG_DATA        0x202
#define LCD_REG_HSTART      0x210
#define LCD_REG_HEND        0x211
#define LCD_REG_VSTART      0x212
#define LCD_REG_VEND        0x213

static __inline USHORT LcdSwap16(USHORT v) { return (USHORT)((v >> 8) | (v << 8)); }

static __inline void LcdSelect(USHORT reg)
{
    volatile UCHAR *prdr = (volatile UCHAR *)LCD_PRDR;
    *prdr = (UCHAR)(*prdr & ~0x10);
    *(volatile USHORT *)LCD_DATA_PORT = LcdSwap16(reg);
    *prdr = (UCHAR)(*prdr | 0x10);
}
static __inline void LcdWriteReg(USHORT reg, USHORT v)
{
    LcdSelect(reg);
    *(volatile USHORT *)LCD_DATA_PORT = LcdSwap16(v);
}
static __inline void LcdWriteData(USHORT v)
{
    *(volatile USHORT *)LCD_DATA_PORT = LcdSwap16(v);
}

#endif
