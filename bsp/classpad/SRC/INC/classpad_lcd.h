//------------------------------------------------------------------------------
//  classpad_lcd.h - panel geometry and the two LCD protocols
//
//  The controller has no memory-mapped frame buffer: pixels are streamed
//  through a 16-bit data port at 0xB4000000 (P2), with Port R bit 4
//  (PRDR 0xA405013C, 8-bit) selecting command/register (0) or data (1).
//  Under the translator 16-bit stores are byte-swapped like memory, so the
//  driver pre-swaps every halfword it writes (docs/04, guest/mmio.h); byte
//  accesses need nothing.
//
//  Two panels share the port; classpad_ddi.cpp picks one from the registry
//  ("Panel") or the display height:
//
//  LCD_PANEL_DCS - the fx-CP400 panel as casio-emu's fx-cp400 profile models
//  it (src/hardware/classpad/classpad.c, verified against the ClassPad OS's
//  LCD_SendCommand / LCD_SetDrawingBounds): a DCS command set. A command is
//  the low byte of a 16-bit write with RS low; its parameters follow as
//  16-bit writes with RS high, one byte each. CASET 0x2A / PASET 0x2B take
//  start high, start low, end high, end low; RAMWR 0x2C starts the memory
//  write and every following 16-bit data write is one RGB565 pixel (a 32-bit
//  write is two, high half first), advancing column-first inside the window
//  and wrapping to its start. Nothing is shown before SLPOUT 0x11 and DISPON
//  0x29 (the emulator presents a blank frame until both were sent); DISPOFF
//  0x28 and SLPIN 0x10 blank and sleep, GRAM is retained. 320x528, column 0
//  is the first visible column (the modelled panel variant answers ID 0x16,
//  the OS's zero-X-offset path). Only these commands are modelled; COLMOD
//  0x3A is sent for a real panel and ignored by the emulator.
//
//  LCD_PANEL_R61524 - the fx-CG100 panel as casio-emu's fx-cg100 profile
//  emulates it (the `lcd` guest's protocol): register index with RS low,
//  value with RS high; 396 GRAM columns x 224 rows, visible window columns
//  6..389 and rows 0..215 (384x216); the window registers 0x210/0x211 take
//  mirrored columns (395 - x). Kept so the image still runs on that profile.
//------------------------------------------------------------------------------
#ifndef __CLASSPAD_LCD_H
#define __CLASSPAD_LCD_H

#define LCD_PANEL_R61524    0               // fx-CG100 (registry "Panel" value)
#define LCD_PANEL_DCS       1               // fx-CP400

#define LCD_CP400_WIDTH     320
#define LCD_CP400_HEIGHT    528
#define LCD_CG100_WIDTH     384
#define LCD_CG100_HEIGHT    216
#define LCD_CG100_COL0      6               // first visible GRAM column
#define LCD_CG100_GRAM_COLS 396
#define LCD_MAX_WIDTH       384
#define LCD_MAX_HEIGHT      528
#define LCD_PANEL_MIN_ROWS  500             // a display this tall is the fx-CP400 (the touch driver's rule too)

#define LCD_DATA_PORT       0xB4000000      // 16-bit, P2
#define LCD_PRDR            0xA405013C      // Port R data register, bit 4 = RS

// R61524 registers (LCD_PANEL_R61524)
#define LCD_REG_HADDR       0x200
#define LCD_REG_VADDR       0x201
#define LCD_REG_DATA        0x202
#define LCD_REG_HSTART      0x210
#define LCD_REG_HEND        0x211
#define LCD_REG_VSTART      0x212
#define LCD_REG_VEND        0x213

// DCS commands (LCD_PANEL_DCS)
#define LCD_DCS_SLPIN       0x10
#define LCD_DCS_SLPOUT      0x11
#define LCD_DCS_DISPOFF     0x28
#define LCD_DCS_DISPON      0x29
#define LCD_DCS_CASET       0x2A            // column window: start high, start low, end high, end low
#define LCD_DCS_PASET       0x2B            // row window, same four bytes
#define LCD_DCS_RAMWR       0x2C            // then one RGB565 pixel per 16-bit write
#define LCD_DCS_COLMOD      0x3A            // pixel format, 0x55 = 16 bpp
#define LCD_DCS_RDID        0xDA            // reads 0x16 on the modelled panel
#define LCD_DCS_COLMOD_16BPP 0x55

static __inline USHORT LcdSwap16(USHORT v) { return (USHORT)((v >> 8) | (v << 8)); }

// RS low, one 16-bit write (a register index or a command byte), RS high
static __inline void LcdSelect(USHORT reg)
{
    volatile UCHAR *prdr = (volatile UCHAR *)LCD_PRDR;
    *prdr = (UCHAR)(*prdr & ~0x10);
    *(volatile USHORT *)LCD_DATA_PORT = LcdSwap16(reg);
    *prdr = (UCHAR)(*prdr | 0x10);
}
static __inline void LcdWriteData(USHORT v)
{
    *(volatile USHORT *)LCD_DATA_PORT = LcdSwap16(v);
}
// R61524: register write
static __inline void LcdWriteReg(USHORT reg, USHORT v)
{
    LcdSelect(reg);
    LcdWriteData(v);
}
// DCS: command, then parameter bytes (one per 16-bit write)
static __inline void LcdCommand(UCHAR cmd) { LcdSelect(cmd); }
static __inline void LcdParam(UCHAR v)     { LcdWriteData(v); }

#endif
