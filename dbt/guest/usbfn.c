/* usbfn.c - the CE USB function driver's register sequences, rehearsed.
 *
 * The CE driver (bsp/classpad50/SRC/DRIVERS/USBFN/sh7305_usbfn.cpp) cannot be
 * built outside Platform Builder, but what it does to the controller can run
 * here: this guest is a small USB mass storage device, polled instead of
 * interrupt driven, that uses the driver's own register definitions and FIFO
 * routines (classpad_usbreg.h) in the driver's order, through the translator
 * like the driver (little-endian code, big-endian bus). Its disk is Casio's
 * storage memory through the reader and the writer of the storage driver
 * (classpad_fugue.h, classpad_fuguewr.h, the translator's NOR service), so a
 * host WRITE(10) ends up as Fugue records in the flash.
 *
 * The host is casio-emu's, with dbt/emu-patch/add_classpad_usbhost.py:
 *   make GUEST=usbfn run      enumeration, INQUIRY, TEST UNIT READY, READ
 *                             CAPACITY, READ(10) of LBA 0, then a raw
 *                             WRITE(10) of a file at LBA 4000
 * The configuration descriptor is padded to 67 bytes on purpose: one full
 * 64-byte packet and a 3-byte tail, the two cases of the control FIFO that
 * a 32-byte descriptor never reaches (and the size of the CDC ACM one).
 *
 * It ends by itself once the host has been quiet for a while and prints
 * PASS/FAIL lines: what it saw, and whether the written file is in the flash. */
#include "hyper.h"
typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
#include "../../bsp/classpad50/SRC/INC/classpad_mmio.h"
#include "../../bsp/classpad50/SRC/INC/classpad_usbreg.h"
#include "../../bsp/classpad50/SRC/INC/classpad_fuguewr.h"

#define HC_NOR_TAG      0x4E4F5200u
#define CHIP_OFF        (CLASSPAD_FUGUE_PA - 0x80000000u)
#define CLUSTERS        CLASSPAD_FUGUE_MAX_CLUSTERS(CLASSPAD_FUGUE_SECTORS)
#define INTC_IMR9       0xA40800A4u
#define PIPE_OUT        3               /* physical endpoint 1 of the driver */
#define PIPE_IN         4               /* physical endpoint 2 */
#define MAXP            64              /* the emulator's host is full speed */
#define WRITE_LBA       4000u

static unsigned hc_nor(unsigned op, unsigned a, unsigned b, unsigned c)
{
    register unsigned r4 __asm__("r4") = HC_NOR_TAG | op; register unsigned r5 __asm__("r5") = a;
    register unsigned r6 __asm__("r6") = b; register unsigned r7 __asm__("r7") = c; register unsigned r0 __asm__("r0");
    __asm__ volatile ("trapa #0xFF" : "=r"(r0) : "r"(r4), "r"(r5), "r"(r6), "r"(r7) : "memory");
    return r0;
}

static int fails;
static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

static CLASSPAD_FUGUE f;
static CLASSPAD_FUGUE_WR w;
static unsigned short loc[CLUSTERS];
static unsigned ver[CLUSTERS];
static unsigned char page[CLASSPAD_FUGUE_PAGE_BYTES], cluster[CLASSPAD_FUGUE_PAGE_BYTES];
static unsigned char io[8 * 512];
static unsigned setups, cbws, reads, writes, written, configured, badFifo;

static int nor_program(void *ctx, unsigned off, const unsigned char *src, unsigned len)
{
    (void)ctx;
    return hc_nor(1, CHIP_OFF + off, (unsigned)src, len) != 0;
}
static int nor_erase(void *ctx, unsigned sector)
{
    (void)ctx;
    return hc_nor(2, CHIP_OFF + sector * CLASSPAD_FUGUE_SECTOR_BYTES, 0, 0) != 0;
}

static const unsigned char dev_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x09, 0x12, 0x01, 0x00, 0, 0, 0, 0, 0, 1
};
static unsigned char cfg_desc[67] = {
    9, 2, 67, 0, 1, 1, 0, 0xC0, 0,
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,
    35, 0x24,                                           /* padding: a class-specific descriptor nobody reads */
};
static const unsigned char cfg_tail[14] = {
    7, 5, 0x01, 2, MAXP, 0, 0,
    7, 5, 0x82, 2, MAXP, 0, 0,
};

static void set_pid(unsigned reg, unsigned pid)
{
    USB_W16(reg, (UINT16)((USB_R16(reg) & ~(CTR_PID | CTR_CCPL)) | pid));
}

static int wait_bemp(unsigned pipe)
{
    unsigned i;
    for (i = 0; i < 4000000u; i++) {
        if (USB_R16(USB_BEMPSTS) & (1u << pipe)) { USB_W16(USB_BEMPSTS, (UINT16)~(1u << pipe)); return 1; }
    }
    return 0;
}

/* the driver's SendPacket, in a loop */
static void send(unsigned pipe, const unsigned char *p, unsigned len)
{
    unsigned port = pipe ? USB_D1FIFO : USB_CFIFO, sel = pipe ? USB_D1FIFOSEL : USB_CFIFOSEL;
    unsigned ctr = pipe ? USB_D1FIFOCTR : USB_CFIFOCTR, reg = pipe ? USB_PIPECTR(pipe) : USB_DCPCTR, n;

    USB_W16(USB_BEMPENB, (UINT16)(USB_R16(USB_BEMPENB) | (1u << pipe)));
    do {
        n = len < MAXP ? len : MAXP;
        USB_W16(sel, (UINT16)(FIFOSEL_MBW32 | FIFOSEL_BIGEND | (pipe ? pipe : FIFOSEL_ISEL)));
        if (!UsbFifoReady(ctr)) { badFifo++; return; }
        UsbFifoWrite(port, p, n);
        if (n < MAXP) USB_W16(ctr, FIFOCTR_BVAL);
        set_pid(reg, PID_BUF);
        p += n; len -= n;
        if (!pipe && n < MAXP) break;                   /* the last packet of a control read is not waited for */
        if (!wait_bemp(pipe)) { badFifo++; return; }
    } while (len);
    USB_W16(USB_BEMPENB, (UINT16)(USB_R16(USB_BEMPENB) & ~(1u << pipe)));
}

/* the driver's ReceivePacket: what the FIFO holds, up to room bytes */
static unsigned receive(unsigned char *p, unsigned room)
{
    unsigned have, n;
    USB_W16(USB_D0FIFOSEL, (UINT16)(FIFOSEL_MBW32 | FIFOSEL_BIGEND | PIPE_OUT));
    if (!UsbFifoReady(USB_D0FIFOCTR)) return 0;
    have = USB_R16(USB_D0FIFOCTR) & FIFOCTR_DTLN;
    n = have < room ? have : room;
    UsbFifoRead(USB_D0FIFO, p, n);
    if (!have) USB_W16(USB_D0FIFOCTR, FIFOCTR_BCLR);
    set_pid(USB_PIPECTR(PIPE_OUT), PID_BUF);
    return n;
}

static void pipe_init(unsigned pipe, unsigned ep, int in)
{
    set_pid(USB_PIPECTR(pipe), PID_NAK);
    USB_W16(USB_PIPESEL, (UINT16)pipe);
    USB_W16(USB_PIPECFG, (UINT16)(PIPECFG_BULK | PIPECFG_SHTNAK | (in ? PIPECFG_DIR_IN : 0) | ep));
    USB_W16(USB_PIPEBUF, (UINT16)((7 << 10) | (8 + (pipe - 1) * 8)));
    USB_W16(USB_PIPEMAXP, MAXP);
    USB_W16(USB_PIPEPERI, 0);
    USB_W16(USB_PIPESEL, 0);
    USB_W16(USB_PIPECTR(pipe), CTR_ACLRM);
    USB_W16(USB_PIPECTR(pipe), 0);
    USB_W16(USB_PIPECTR(pipe), CTR_SQCLR);
}

static void on_setup(void)
{
    unsigned req = USB_R16(USB_USBREQ), type = req & 0xFF, request = req >> 8;
    unsigned value = USB_R16(USB_USBVAL), length = USB_R16(USB_USBLENG), n;

    USB_W16(USB_INTSTS0, (UINT16)~INTSTS0_VALID);
    USB_W16(USB_CFIFOCTR, FIFOCTR_BCLR);
    setups++;
    if (type == 0x80 && request == 6 && (value >> 8) == 1) {
        send(0, dev_desc, length < sizeof dev_desc ? length : sizeof dev_desc);
    } else if (type == 0x80 && request == 6 && (value >> 8) == 2) {
        n = length < sizeof cfg_desc ? length : sizeof cfg_desc;
        send(0, cfg_desc, n);
    } else if (type == 0x00 && request == 9) {          /* SET_CONFIGURATION: the pipes, as the MDD's clients open them */
        pipe_init(PIPE_OUT, 1, 0);
        pipe_init(PIPE_IN, 2, 1);
        USB_W16(USB_BRDYENB, (UINT16)(USB_R16(USB_BRDYENB) | (1u << PIPE_OUT)));
        set_pid(USB_PIPECTR(PIPE_OUT), PID_BUF);
        configured = 1;
    }                                                   /* SET_ADDRESS and the rest: just the status stage */
    USB_W16(USB_DCPCTR, (UINT16)((USB_R16(USB_DCPCTR) & ~CTR_PID) | PID_BUF | CTR_CCPL));
}

static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }

static void csw(const unsigned char *cbw, unsigned status)
{
    unsigned char s[13];
    unsigned i;
    s[0] = 'U'; s[1] = 'S'; s[2] = 'B'; s[3] = 'S';
    for (i = 0; i < 4; i++) s[4 + i] = cbw[4 + i];
    for (i = 0; i < 4; i++) s[8 + i] = 0;
    s[12] = (unsigned char)status;
    send(PIPE_IN, s, 13);
}

/* count 512-byte sectors from io[] to the disk: whole clusters through the writer */
static int disk_write(unsigned lba, unsigned count)
{
    const unsigned char *p, *src = io;
    unsigned i, lc;
    for (; count; count--, lba++, src += 512) {
        lc = lba / 8;
        p = ClasspadFugueCluster(&f, lc);
        for (i = 0; i < CLASSPAD_FUGUE_PAGE_BYTES; i++) cluster[i] = p ? p[i] : 0xFF;
        for (i = 0; i < 512; i++) cluster[(lba % 8) * 512 + i] = src[i];
        if (ClasspadFugueWrite(&w, lc, cluster) != CLASSPAD_FUGUE_OK) return 0;
    }
    return 1;
}

static void on_cbw(const unsigned char *cbw)
{
    const unsigned char *cdb = cbw + 15;
    unsigned lba, count, got, i, n;
    static unsigned char buf[36];

    cbws++;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    switch (cdb[0]) {
    case 0x12:                                          /* INQUIRY */
        buf[1] = 0x80; buf[2] = 2; buf[3] = 2; buf[4] = 31;
        for (i = 0; i < 28; i++) buf[8 + i] = "ClassPadCE guest        0001"[i];
        send(PIPE_IN, buf, 36);
        csw(cbw, 0);
        break;
    case 0x00:                                          /* TEST UNIT READY */
        csw(cbw, 0);
        break;
    case 0x03:                                          /* REQUEST SENSE */
        buf[0] = 0x70; buf[7] = 10;
        send(PIPE_IN, buf, 18);
        csw(cbw, 0);
        break;
    case 0x25:                                          /* READ CAPACITY(10) */
        n = f.blocks - 1;
        buf[0] = (unsigned char)(n >> 24); buf[1] = (unsigned char)(n >> 16); buf[2] = (unsigned char)(n >> 8); buf[3] = (unsigned char)n;
        buf[6] = 2;
        send(PIPE_IN, buf, 8);
        csw(cbw, 0);
        break;
    case 0x28:                                          /* READ(10) */
        lba = be32(cdb + 2); count = (unsigned)cdb[7] << 8 | cdb[8];
        if (count > 8 || lba + count > f.blocks) { csw(cbw, 1); break; }
        ClasspadFugueRead(&f, lba, count, io);
        send(PIPE_IN, io, count * 512);
        reads += count;
        csw(cbw, 0);
        break;
    case 0x2A:                                          /* WRITE(10): the data follows on the same pipe */
        lba = be32(cdb + 2); count = (unsigned)cdb[7] << 8 | cdb[8];
        if (count > 8 || lba + count > f.blocks) { csw(cbw, 1); break; }
        for (got = 0, i = 0; got < count * 512 && i < 4000000u; i++) {
            if (!(USB_R16(USB_BRDYSTS) & (1u << PIPE_OUT))) continue;
            USB_W16(USB_BRDYSTS, (UINT16)~(1u << PIPE_OUT));
            got += receive(io + got, count * 512 - got);
        }
        if (got != count * 512 || !disk_write(lba, count)) { csw(cbw, 1); break; }
        writes += count;
        if (lba <= WRITE_LBA && WRITE_LBA < lba + count) written = 1;
        csw(cbw, 0);
        break;
    default:
        csw(cbw, 1);
        break;
    }
}

void guest_main(void)
{
    static unsigned char cbw[64];
    unsigned sts, idle = 0, n, i, state = 0;
    const unsigned char *p;

    for (i = 0; i < sizeof cfg_tail; i++) cfg_desc[sizeof cfg_desc - sizeof cfg_tail + i] = cfg_tail[i];
    MMIO_W8(INTC_IMR9, 0x02);                           /* polled: the controller's interrupt stays masked */

    check("the storage memory mounts", ClasspadFugueScan(&f, (const void *)CLASSPAD_FUGUE_PA, CLASSPAD_FUGUE_SECTORS, loc, ver, CLUSTERS) == 49536);
    check("and is writable", ClasspadFugueWrInit(&w, &f, page, 0, nor_program, nor_erase));

    /* the driver's PowerOn, without the clock and PHY registers the emulator does not have */
    USB_W16(USB_SYSCFG, (UINT16)(USB_R16(USB_SYSCFG) | SYSCFG_SCKE));
    USB_W16(USB_BUSWAIT, 5);
    USB_W16(USB_SYSCFG, SYSCFG_SCKE | SYSCFG_HSE | SYSCFG_USBE);
    USB_W16(USB_DCPCFG, 0);
    USB_W16(USB_DCPMAXP, 64);
    USB_W16(USB_CFIFOSEL, FIFOSEL_MBW32 | FIFOSEL_BIGEND);
    USB_W16(USB_BRDYENB, 0); USB_W16(USB_BEMPENB, 0);
    USB_W16(USB_BRDYSTS, 0); USB_W16(USB_BEMPSTS, 0);
    USB_W16(USB_INTSTS0, 0);
    USB_W16(USB_INTENB0, INT_DVST | INT_CTRT | INT_BEMP | INT_BRDY);
    check("VBUS is there", (USB_R16(USB_INTSTS0) & INTSTS0_VBSTS) != 0);
    USB_W16(USB_SYSCFG, (UINT16)(USB_R16(USB_SYSCFG) | SYSCFG_DPRPU));

    while (idle < 3000000u) {
        idle++;
        sts = USB_R16(USB_INTSTS0);
        if (sts & INT_DVST) {
            USB_W16(USB_INTSTS0, (UINT16)~INT_DVST);
            if (INTSTS0_DVSQ(sts) > state) state = INTSTS0_DVSQ(sts);
            idle = 0;
        }
        if (sts & INT_CTRT) {
            USB_W16(USB_INTSTS0, (UINT16)~INT_CTRT);
            if (sts & INTSTS0_VALID) on_setup();
            idle = 0;
        }
        if (configured && (USB_R16(USB_BRDYSTS) & (1u << PIPE_OUT))) {
            USB_W16(USB_BRDYSTS, (UINT16)~(1u << PIPE_OUT));
            n = receive(cbw, sizeof cbw);
            if (n == 31 && cbw[0] == 'U' && cbw[1] == 'S' && cbw[2] == 'B' && cbw[3] == 'C') on_cbw(cbw);
            else badFifo++;
            idle = 0;
        }
    }

    check("bus reset, address, configured", state == 3);
    check("five setup packets", setups == 5);
    check("the host got through its commands", cbws >= 5 && reads >= 1);
    check("no FIFO trouble", badFifo == 0);
    check("the host's WRITE(10) arrived", writes > 0 && written);
    ClasspadFugueScan(&f, (const void *)CLASSPAD_FUGUE_PA, CLASSPAD_FUGUE_SECTORS, loc, ver, CLUSTERS);
    p = ClasspadFugueCluster(&f, WRITE_LBA / 8);
    check("and is in the flash: the file starts \"#!/usr/bin/env python3\"",
          p && p[0] == '#' && p[1] == '!' && p[2] == '/' && p[3] == 'u' && p[14] == ' ' && p[15] == 'p');
    p = ClasspadFugueCluster(&f, 0);
    check("the MBR is untouched", p && p[510] == 0x55 && p[511] == 0xAA);
    hc_puts("setups "); hc_puthex(setups); hc_puts(" cbws "); hc_puthex(cbws); hc_puts(" read "); hc_puthex(reads);
    hc_puts(" written "); hc_puthex(writes); hc_putc('\n');
    hc_puts(fails ? "usbfn: FAILED\n" : "usbfn: ok\n");
    hc_exit(fails);
}
