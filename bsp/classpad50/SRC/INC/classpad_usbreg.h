//------------------------------------------------------------------------------
//  classpad_usbreg.h - the SH7305 USB module's registers and FIFO ports
//
//  Shared by the CE USB function driver (SRC\DRIVERS\USBFN) and the
//  translator guest that rehearses its register sequences in casio-emu
//  (dbt/guest/usbfn.c). Register layout: gint's include/gint/mpu/usb.h (the
//  controller is of Renesas' USBHS family). Needs classpad_mmio.h and the
//  UINT8/UINT16/UINT32 types.
//
//  FIFO data goes through 32-bit accesses with BIGEND set, so that the first
//  byte in memory is the first on the bus. The translator swaps every
//  16/32-bit access of a little-endian image, which makes a plain copy of
//  memory words exactly that; the registers proper use the pre-swapped MMIO
//  macros. A tail of 1 to 3 bytes is written the way gint does (a byte, a
//  word, a word and a byte at port + 2) and read as one more longword.
//------------------------------------------------------------------------------
#ifndef CLASSPAD_USBREG_H
#define CLASSPAD_USBREG_H

#define USB_BASE            0xA4D80000
#define USB_SYSCFG          0x00
#define USB_BUSWAIT         0x02
#define USB_SYSSTS          0x04
#define USB_DVSTCTR         0x08
#define USB_CFIFO           0x14
#define USB_D0FIFO          0x18
#define USB_D1FIFO          0x1C
#define USB_CFIFOSEL        0x20
#define USB_CFIFOCTR        0x22
#define USB_D0FIFOSEL       0x28
#define USB_D0FIFOCTR       0x2A
#define USB_D1FIFOSEL       0x2C
#define USB_D1FIFOCTR       0x2E
#define USB_INTENB0         0x30
#define USB_INTENB1         0x32
#define USB_BRDYENB         0x36
#define USB_NRDYENB         0x38
#define USB_BEMPENB         0x3A
#define USB_SOFCFG          0x3C
#define USB_INTSTS0         0x40
#define USB_BRDYSTS         0x46
#define USB_NRDYSTS         0x48
#define USB_BEMPSTS         0x4A
#define USB_USBADDR         0x50
#define USB_USBREQ          0x54
#define USB_USBVAL          0x56
#define USB_USBINDX         0x58
#define USB_USBLENG         0x5A
#define USB_DCPCFG          0x5C
#define USB_DCPMAXP         0x5E
#define USB_DCPCTR          0x60
#define USB_PIPESEL         0x64
#define USB_PIPECFG         0x68
#define USB_PIPEBUF         0x6A
#define USB_PIPEMAXP        0x6C
#define USB_PIPEPERI        0x6E
#define USB_PIPECTR(p)      (0x70 + 2 * ((p) - 1))
#define USB_REG_C2          0xC2

#define SYSCFG_SCKE         0x0400
#define SYSCFG_HSE          0x0080
#define SYSCFG_DCFM         0x0040
#define SYSCFG_DRPD         0x0020
#define SYSCFG_DPRPU        0x0010
#define SYSCFG_USBE         0x0001
#define DVSTCTR_RHST        0x0007      // 2 full speed, 3 high speed
#define FIFOSEL_MBW32       0x0800
#define FIFOSEL_BIGEND      0x0100
#define FIFOSEL_ISEL        0x0020      // CFIFO: 1 = writing the DCP
#define FIFOCTR_BVAL        0x8000
#define FIFOCTR_BCLR        0x4000
#define FIFOCTR_FRDY        0x2000
#define FIFOCTR_DTLN        0x0FFF
#define INT_VBINT           0x8000
#define INT_RESM            0x4000
#define INT_DVST            0x1000
#define INT_CTRT            0x0800
#define INT_BEMP            0x0400
#define INT_BRDY            0x0100
#define INTSTS0_VBSTS       0x0080
#define INTSTS0_DVSQ(v)     (((v) >> 4) & 7)
#define INTSTS0_VALID       0x0008
#define CTR_PID             0x0003
#define PID_NAK             0
#define PID_BUF             1
#define PID_STALL           2
#define CTR_CCPL            0x0004
#define CTR_PBUSY           0x0020
#define CTR_SQMON           0x0040
#define CTR_SQCLR           0x0100
#define CTR_ACLRM           0x0200
#define CTR_INBUFM          0x4000
#define CTR_BSTS            0x8000
#define PIPECFG_BULK        0x4000
#define PIPECFG_INT         0x8000
#define PIPECFG_DBLB        0x0200
#define PIPECFG_SHTNAK      0x0080
#define PIPECFG_DIR_IN      0x0010
#define SOFCFG_ENABLE       0x0020      // gint: must be set by hand

// What Casio's OS and gint do around the controller (battif.c has the story)
#define CPG_USBCLKCR        0xA4150014
#define USBCLKCR_STOP       0x00000100
#define PWR_MSTPCR2         0xA4150038
#define MSTPCR2_USB         0x00100000
#define PFC_MSELCRA         0xA4050180
#define MSELCRA_USB_M       0x00C0
#define MSELCRA_USB_OFF     0x0040
#define PFC_MSELCRB         0xA4050182
#define MSELCRB_USB_M       0xC000
#define USB_UPONCR          0xA40501D4
#define UPONCR_ON           0x0600
#define PORT_PUDR           0xA4050162
#define PUDR_USB_CABLE      0x02

#define USB_R16(off)        MMIO_R16(USB_BASE + (off))
#define USB_W16(off, v)     MMIO_W16(USB_BASE + (off), (v))
#define USB_READY_SPINS     2000

static void UsbFifoCopy(void *dst, const void *src, unsigned n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

static void UsbFifoWrite(unsigned port, const unsigned char *p, unsigned n)
{
    volatile UINT32 *f32 = (volatile UINT32 *)CLASSPAD_UA(USB_BASE + port);
    UINT32 v32;
    UINT16 v16;

    if (((unsigned long)p & 3) == 0) {
        const UINT32 *p32 = (const UINT32 *)p;
        for (; n >= 16; n -= 16, p32 += 4) {
            *f32 = p32[0]; *f32 = p32[1]; *f32 = p32[2]; *f32 = p32[3];
        }
        for (; n >= 4; n -= 4) *f32 = *p32++;
        p = (const unsigned char *)p32;
    } else {
        for (; n >= 4; n -= 4, p += 4) { UsbFifoCopy(&v32, p, 4); *f32 = v32; }
    }
    if (n >= 2) {
        UsbFifoCopy(&v16, p, 2);
        *(volatile UINT16 *)f32 = v16;
        if (n == 3) *((volatile UINT8 *)f32 + 2) = p[2];
    } else if (n) {
        *(volatile UINT8 *)f32 = p[0];
    }
}

static void UsbFifoRead(unsigned port, unsigned char *p, unsigned n)
{
    volatile UINT32 *f32 = (volatile UINT32 *)CLASSPAD_UA(USB_BASE + port);
    UINT32 v32;

    if (((unsigned long)p & 3) == 0) {
        UINT32 *p32 = (UINT32 *)p;
        for (; n >= 16; n -= 16, p32 += 4) {
            p32[0] = *f32; p32[1] = *f32; p32[2] = *f32; p32[3] = *f32;
        }
        for (; n >= 4; n -= 4) *p32++ = *f32;
        p = (unsigned char *)p32;
    } else {
        for (; n >= 4; n -= 4, p += 4) { v32 = *f32; UsbFifoCopy(p, &v32, 4); }
    }
    if (n) { v32 = *f32; UsbFifoCopy(p, &v32, n); }
}

// Waits for a FIFO port to follow its select register
static int UsbFifoReady(unsigned ctr)
{
    int i;
    for (i = 0; i < USB_READY_SPINS; i++) {
        if (USB_R16(ctr) & FIFOCTR_FRDY) return 1;
    }
    return 0;
}

#endif
