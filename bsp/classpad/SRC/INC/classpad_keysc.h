//------------------------------------------------------------------------------
//  classpad_keysc.h - SH7305 KEYSC key scan controller (fx-CG100, fx-CP400)
//
//  Register map and the programming sequence come from casio-emu
//  (src/hardware/keyboard/keyboard.c) and the fx-CG100 OS 2.00 keyboard
//  initialisation (casio-emu analysis/cg100-200/keysc-os200-notes.md). All
//  registers are 16-bit: access them through MMIO_R16/MMIO_W16 (the
//  translator byte-swaps 16-bit loads and stores).
//
//  The matrix is 12 KOUT rows x 8 KIN columns held in six KIUDATA words:
//  word w bit b <-> row 2w + (b >> 3), column b & 7; a set bit is a pressed
//  key. The driver's scan code is the bit position in that 96-bit image,
//  scancode = 16 * word + bit = 8 * row + column. casio-emu's setKeydown
//  "basic key codes" (HLE_CB_KEY=<code>, the skins) are row = code % 10,
//  column = code / 10 - 1: KEYSC_CODE_TO_SCAN converts them.
//
//  Interrupt: INTEVT 0xBE0, mask IMR5 bit 7, priority IPRF bits 15:12. The
//  controller raises it on a scan completion (KIUCNTREG scan-start write) and
//  on every key edge while the row and column are enabled. The emulator
//  drops an edge raised while the source is masked (between the ISR and
//  InterruptDone), so the driver also polls the matrix on a short timeout.
//------------------------------------------------------------------------------
#ifndef __CLASSPAD_KEYSC_H
#define __CLASSPAD_KEYSC_H

#define KEYSC_KIUDATA(n)        (SH7305_REG_PA_KEYSC + 0x00 + 2 * (n))  // n = 0..5, key image
#define KEYSC_KIUCNTREG         (SH7305_REG_PA_KEYSC + 0x0C)    // bit 15: scan start
#define KEYSC_KIAUTOFIXREG      (SH7305_REG_PA_KEYSC + 0x0E)    // auto key bounce
#define KEYSC_KIUMODEREG        (SH7305_REG_PA_KEYSC + 0x10)    // scan mode
#define KEYSC_KIUSTATEREG       (SH7305_REG_PA_KEYSC + 0x12)    // bit 0: scan idle (ClassPad; not emulated for the fx-CG100)
#define KEYSC_KIUINTREG         (SH7305_REG_PA_KEYSC + 0x14)    // interrupt setting / status
#define KEYSC_KIUWSETREG        (SH7305_REG_PA_KEYSC + 0x16)    // scan wait time
#define KEYSC_KIUINTERVALREG    (SH7305_REG_PA_KEYSC + 0x18)    // scan interval time
#define KEYSC_KOUTPINSET        (SH7305_REG_PA_KEYSC + 0x1A)    // KOUT line function (bit n = row n scanned)
#define KEYSC_KINPINSET         (SH7305_REG_PA_KEYSC + 0x1C)    // KIN line function (bit n = column n sensed)

#define KEYSC_WORDS             6
#define KEYSC_ROWS              12
#define KEYSC_COLUMNS           8
#define KEYSC_SCANCODES         (KEYSC_WORDS * 16)              // 96

#define KEYSC_CNT_SCAN_START    0x8000

// Values the fx-CG100 OS 2.00 writes (in this order) when it brings the
// controller up; the emulator only interprets KOUTPINSET/KINPINSET and the
// scan-start bit.
#define KEYSC_INT_CLEAR         0x00FF
#define KEYSC_INT_ENABLE        0x4800
#define KEYSC_AUTOFIX_VALUE     0x8042
#define KEYSC_INTERVAL_VALUE    0x00C8
#define KEYSC_WSET_VALUE        0x0000
#define KEYSC_KOUT_ALL          0x0FFF
#define KEYSC_KIN_ALL           0x00FF
#define KEYSC_MODE_VALUE        0x0200

#define KEYSC_SCANCODE(word, bit)   ((UINT32)((word) * 16 + (bit)))
#define KEYSC_CODE_TO_SCAN(code)    ((UINT32)(8 * ((code) % 10) + ((code) / 10 - 1)))

// Layout manager PDD mask of the KEYSC PDD (0x0100..0x0800 are reserved for
// OEM use, laymgr.h); shared by the PDD and its device layouts.
#define KEYBD_PDD_CLASSPAD      0x0100

#endif
