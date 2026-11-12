#!/usr/bin/env python3
"""add_classpad_usbhost.py - a USB host for the CE USB function driver on
casio-emu's ClassPad profile (bsp/classpad50/SRC/DRIVERS/USBFN, docs/08).

casio-emu's USB controller model and its host (enumeration, the BOT/SCSI
script) were shaped by Casio's fx-CG100 OS and are switched off on the
ClassPad. With HLE_USB_CE=1 (and HLE_USB=1 HLE_USB_PLUG=1) on the ClassPad
profile this patch turns the host on and moves the model the few steps the
CE driver, written for the real controller, needs:

  - the interrupt is the SH7305's own: INTEVT 0xA20, IMR9 bit 0x02, priority
    IPRF 7:4 (stock: 0x900 / IMR2, which is ETMU3's vector)
  - a full packet written to the control FIFO raises the buffer-empty
    interrupt of pipe 0 (the driver waits for it before the next packet of a
    descriptor longer than 64 bytes)
  - a full packet written to D1FIFO is sent by itself, as on the chip (stock:
    only on BVAL); the packet size is what the driver wrote to PIPEMAXP
  - the third byte of a 3-byte FIFO tail, written at port + 2, is taken
  - the whole configuration descriptor is asked for (wTotalLength; stock: 52)
  - no CG100 longword hack on SYSSTS
  - the device dropping its pull-up (the driver changing function) is a
    disconnect: the host enumerates again when the pull-up is back
  - a CDC ACM device (interface class 02 or 0A) gets SET_CONTROL_LINE_STATE
    (DTR | RTS) after SET_CONFIGURATION and its bulk pipes bridged to a pty,
    whose name is printed ("[USB] host: CDC ACM bridge on /dev/ttys..."):
    `screen /dev/ttysNNN` or pppd on it talks to the CE COM port. A mass
    storage device gets the stock BOT script (HLE_USB_BOT=1 ...).

Not modelled, so not exercised by an emulator run: control transfers with an
OUT data stage (SET_LINE_CODING), SET_ADDRESS answered by the controller
(the host still hands it to the guest, which the driver also accepts), VBUS
and resume interrupts, high speed.

Without HLE_USB_CE nothing changes for any profile.

    python3 dbt/emu-patch/add_classpad_usbhost.py [~/Dev/casio-emu]
    cmake --build ~/Dev/casio-emu/build
"""
import os
import sys

MARK = 'wince-classpad usbhost'


def sub(s, old, new, what):
    assert s.count(old) == 1, what + ' changed'
    return s.replace(old, new, 1)


def patch_usb(s):
    s = sub(s, '#include <string.h>\n#if defined(_WIN32)',
            '#include <string.h>\n'
            '#if !defined(_WIN32)   /* ' + MARK + ': the CDC ACM bridge\'s pty */\n'
            '#include <fcntl.h>\n#include <unistd.h>\n'
            '#endif\n'
            '#if defined(_WIN32)', 'includes')

    s = sub(s, 'static interruptData usbInterrupt = { .code = 0x900, .IMR = IMR2, .IMRbits = 0x01 };',
            '/* ' + MARK + ': a guest that drives the controller as the chip documents it\n'
            ' * (the CE USB function driver), on the ClassPad profile with HLE_USB_CE=1 */\n'
            'static int usbCeGuest(void) {\n'
            '  static int v = -1;\n'
            '  if (v < 0) v = (machineIsClasspad() && getenv("HLE_USB_CE")) ? 1 : 0;\n'
            '  return v;\n'
            '}\n'
            'static u16 usbCePipeMaxp[10];    /* PIPEMAXP as the guest set it, per pipe */\n'
            'static u16 usbCeTotal;           /* wTotalLength of the configuration descriptor */\n'
            'static int usbCeCdc;             /* the device is CDC ACM: bridge its bulk pipes */\n'
            'static interruptData usbCeInterrupt = { .code = 0xa20, .IMR = IMR9, .IMRbits = 0x02 };\n'
            'static interruptData usbInterrupt = { .code = 0x900, .IMR = IMR2, .IMRbits = 0x01 };',
            'usbInterrupt')

    s = sub(s, 'void usbRaiseIrq(void) {\n',
            'void usbRaiseIrq(void) {\n'
            '  if (usbCeGuest()) { generateIntcInterrupt(&usbCeInterrupt); return; }   /* ' + MARK + ' */\n',
            'usbRaiseIrq')

    s = sub(s, '  if (!force && (readMemory(0xa4080088u, 1) & 0x01u)) return;',
            '  if (!force && (usbCeGuest() ? (readMemory(0xa40800a4u, 1) & 0x02u)   /* ' + MARK + ': IMR9 */\n'
            '                              : (readMemory(0xa4080088u, 1) & 0x01u))) return;',
            'usbRaiseLevelIrq')

    s = sub(s, '  int total = ctrlBuf[2] | (ctrlBuf[3] << 8);\n',
            '  int total = ctrlBuf[2] | (ctrlBuf[3] << 8);\n'
            '  usbCeTotal = (u16) total; usbCeCdc = 0;   /* ' + MARK + ' */\n',
            'usbParseConfigDescriptor')
    s = sub(s, '      ifaceProto = ctrlBuf[pos + 7];\n',
            '      ifaceProto = ctrlBuf[pos + 7];\n'
            '      if (usbCeGuest() && (ifaceClass == 0x02 || ifaceClass == 0x0a)) usbCeCdc = 1;   /* ' + MARK + ' */\n',
            'interface descriptor')

    s = sub(s, 'static void usbCaptureD1Fifo(u32 value, u32 size) {',
            '/* ' + MARK + ': the CDC ACM bridge. The device\'s bulk IN goes to a pty, what the\n'
            ' * pty has goes to its bulk OUT in packets shorter than a full one. */\n'
            'static int usbCeCdcFd = -2;\n'
            'static void usbCeCdcOpen(void) {\n'
            '  if (usbCeCdcFd != -2) return;\n'
            '  usbCeCdcFd = -1;\n'
            '#if !defined(_WIN32)\n'
            '  { int fd = posix_openpt(O_RDWR | O_NOCTTY);\n'
            '    if (fd >= 0 && !grantpt(fd) && !unlockpt(fd) && !fcntl(fd, F_SETFL, O_NONBLOCK)) {\n'
            '      usbCeCdcFd = fd;\n'
            '      printf("[USB] host: CDC ACM bridge on %s\\n", ptsname(fd));\n'
            '      fflush(stdout);\n'
            '    } else {\n'
            '      if (fd >= 0) close(fd);\n'
            '      printf("[USB] host: CDC ACM device, no pty for the bridge\\n");\n'
            '    } }\n'
            '#else\n'
            '  printf("[USB] host: CDC ACM device (no bridge on this platform)\\n");\n'
            '#endif\n'
            '}\n'
            'static void usbCeCdcIn(const u8* data, int len) {\n'
            '  usbCeCdcOpen();\n'
            '#if !defined(_WIN32)\n'
            '  if (usbCeCdcFd >= 0 && len > 0) { ssize_t n = write(usbCeCdcFd, data, (size_t) len); (void) n; }\n'
            '#else\n'
            '  (void) data; (void) len;\n'
            '#endif\n'
            '}\n'
            'static void usbCeCdcPoll(void) {\n'
            '  usbCeCdcOpen();\n'
            '#if !defined(_WIN32)\n'
            '  if (usbCeCdcFd < 0 || d0OutLen != 0 || usbBulkOutPipe <= 0 || cpu.isBranchDelaySlot) return;\n'
            '  if (!(shadow[R_BRDYENB >> 1] & (1u << usbBulkOutPipe))) return;    /* the device is not reading */\n'
            '  { u8 buf[63];\n'
            '    ssize_t n = read(usbCeCdcFd, buf, sizeof buf);\n'
            '    if (n > 0) usbQueueBulkOut(buf, (int) n, "CDC OUT"); }\n'
            '#endif\n'
            '}\n'
            '\n'
            'static void usbCaptureD1Fifo(u32 value, u32 size) {',
            'usbCaptureD1Fifo')

    s = sub(s, 'static void usbCompleteD1Packet(void) {\n',
            'static void usbCompleteD1Packet(void) {\n'
            '  if (usbCeCdc) {   /* ' + MARK + ': serial data, not a CSW or a Protocol 7 packet */\n'
            '    if (usbD1Pipe == usbBulkInPipe) usbCeCdcIn(d1InBuf, d1InLen);\n'
            '    d1InLen = 0;\n'
            '    if (usbD1Pipe > 0) shadow[R_BEMPSTS >> 1] |= (u16) (1u << usbD1Pipe);\n'
            '    usbRefreshPipeInts();\n'
            '    usbRaiseLevelIrq();\n'
            '    return;\n'
            '  }\n',
            'usbCompleteD1Packet')

    s = sub(s, '  if (off == R_SYSSTS && size == 4) {',
            '  if (off == R_SYSSTS && size == 4 && !usbCeGuest()) {   /* ' + MARK + ' */',
            'usbRead SYSSTS')

    s = sub(s, '  if (off >= USB_SPAN) return;\n  switch (off) {\n    case R_INTSTS0:',
            '  if (off >= USB_SPAN) return;\n'
            '  if (usbCeGuest() && (off == R_CFIFO + 2 || off == R_D1FIFO + 2)) off -= 2;   /* ' + MARK + ': the third byte of a FIFO tail */\n'
            '  switch (off) {\n    case R_INTSTS0:',
            'usbWrite')

    s = sub(s, '        if (ctrlBufLen < (int) sizeof(ctrlBuf)) ctrlBuf[ctrlBufLen++] = (u8) (value >> (i * 8));\n      return;',
            '        if (ctrlBufLen < (int) sizeof(ctrlBuf)) ctrlBuf[ctrlBufLen++] = (u8) (value >> (i * 8));\n'
            '      if (usbCeGuest() && ctrlBufLen && !(ctrlBufLen % 64)) {   /* ' + MARK + ': a full DCP packet went out */\n'
            '        shadow[R_BEMPSTS >> 1] |= 1u;\n'
            '        usbRefreshPipeInts();\n'
            '        usbRaiseLevelIrq();\n'
            '      }\n'
            '      return;',
            'R_CFIFO write')

    s = sub(s, '    case R_D1FIFO:\n      usbCaptureD1Fifo(value, size);\n      return;',
            '    case R_D1FIFO:\n      usbCaptureD1Fifo(value, size);\n'
            '      if (usbCeGuest() && usbD1Pipe > 0 && usbD1Pipe < 10 && usbCePipeMaxp[usbD1Pipe] &&\n'
            '          d1InLen >= usbCePipeMaxp[usbD1Pipe]) usbCompleteD1Packet();   /* ' + MARK + ': a full packet sends itself */\n'
            '      return;\n'
            '    case R_PIPEMAXP: {   /* ' + MARK + ' */\n'
            '      int pipe = shadow[R_PIPESEL >> 1] & 0x0f;\n'
            '      if (pipe < 10) usbCePipeMaxp[pipe] = (u16) (value & 0x07ffu);\n'
            '      shadow[off >> 1] = (u16) value;\n'
            '      return;\n'
            '    }',
            'R_D1FIFO write')

    s = sub(s, 'static int plugged(void) {\n  if (residentIsCg50() || machineIsClasspad()) return 0;\n',
            'static int plugged(void) {\n  if (residentIsCg50() || (machineIsClasspad() && !usbCeGuest())) return 0;   /* ' + MARK + ' */\n',
            'plugged')
    s = sub(s, '  if (residentIsCg50() || machineIsClasspad()) return;\n',
            '  if (residentIsCg50() || (machineIsClasspad() && !usbCeGuest())) return;   /* ' + MARK + ' */\n',
            'pollUsb')

    s = sub(s, '    { 0x41, 0x01, 0x0000, 0x0000, 0,  0, "VENDOR_ENABLE(P7)" },\n  };',
            '    { 0x41, 0x01, 0x0000, 0x0000, 0,  0, "VENDOR_ENABLE(P7)" },\n'
            '    { 0x21, 0x22, 0x0003, 0x0000, 0,  0, "SET_CONTROL_LINE_STATE(DTR|RTS)" },   /* ' + MARK + ': step 5 of a CDC ACM device */\n'
            '  };',
            'seq')
    s = sub(s, '  const int nseq = (p7ConfigureRunFile() && !usbMscMode) ? nseqFull : nseqBase;',
            '  const int nseq = usbCeCdc ? nseqBase + 1 : (p7ConfigureRunFile() && !usbMscMode) ? nseqFull - 1 : nseqBase;\n'
            '#define USB_CE_STEP(n) (&seq[(usbCeCdc && (n) == nseqBase) ? nseqFull - 1 : (n)])   /* ' + MARK + ' */',
            'nseq')
    old = '      const SetupStep* s = &seq[estep];\n      shadow[R_USBREQ >> 1]'
    s = sub(s, old, '      const SetupStep* s = USB_CE_STEP(estep);\n      shadow[R_USBREQ >> 1]', 'step issue')
    s = sub(s, '      shadow[R_USBLENG >> 1] = s->wLen;',
            '      shadow[R_USBLENG >> 1] = (usbCeGuest() && estep == 3 && usbCeTotal > s->wLen) ? usbCeTotal : s->wLen;   /* ' + MARK + ' */',
            'wLength')
    s = sub(s, '      if (ctrlDone) {\n        const SetupStep* s = &seq[estep];',
            '      if (ctrlDone) {\n        const SetupStep* s = USB_CE_STEP(estep);', 'step completion')
    s = sub(s, '        printf("[USB] host: SETUP[%d] %s TIMEOUT (no CCPL)\\n", estep, seq[estep].name);',
            '        printf("[USB] host: SETUP[%d] %s TIMEOUT (no CCPL)\\n", estep, USB_CE_STEP(estep)->name);', 'step timeout')
    s = sub(s, '  if (!hostArmed && plugged() && (shadow[R_SYSCFG >> 1] & 0x0010u) && !cpu.isBranchDelaySlot) {\n',
            '  if (usbCeGuest() && hostArmed && !(shadow[R_SYSCFG >> 1] & 0x0010u) && !cpu.isBranchDelaySlot) {   /* ' + MARK + ': the device took its pull-up away (a change of function) */\n'
            '    hostArmed = 0; didReset = 0; estep = -1; epending = 0; enumCompletePrinted = 0;\n'
            '    usbDvsq = 0; usbMscMode = 0; usbCeCdc = 0; usbBulkOutPipe = 0; usbBulkInPipe = 0; d0OutLen = 0;\n'
            '    shadow[R_DVSTCTR >> 1] &= ~0x0007u;\n'
            '    printf("[USB] host: device pull-up removed, waiting for it to come back\\n");\n'
            '  }\n'
            '  if (!hostArmed && plugged() && (shadow[R_SYSCFG >> 1] & 0x0010u) && !cpu.isBranchDelaySlot) {\n',
            'attach')
    s = sub(s, '  if (didReset && estep >= nseq && !enumCompletePrinted) {',
            '  if (usbCeCdc && didReset && estep >= nseq) usbCeCdcPoll();   /* ' + MARK + ' */\n'
            '  if (didReset && estep >= nseq && !enumCompletePrinted) {',
            'enumeration complete')
    return s


def patch_intc(s):
    s = sub(s, '    case 0x900: return !(IMR[IMR2]&0x01);\n',
            '    case 0x900: return !(IMR[IMR2]&0x01);\n'
            '    case 0xa20: return !(IMR[IMR9]&0x02);   /* ' + MARK + ': the USB function controller */\n',
            'deliveryEnabled')
    s = sub(s, '    case 0x900:return (IPR[IPRE]>>4)&15;\n',
            '    case 0x900:return (IPR[IPRE]>>4)&15;\n'
            '    case 0xa20:return (IPR[IPRF]>>4)&15;   /* ' + MARK + ' */\n',
            'classpadPriority')
    return s


def main(argv):
    root = os.path.expanduser(argv[1] if len(argv) > 1 else '~/Dev/casio-emu')
    for rel, fn in (('src/hardware/usb/usb.c', patch_usb), ('src/hardware/intc/intc.c', patch_intc)):
        path = os.path.join(root, rel)
        s = open(path).read()
        if MARK in s:
            print('already patched', path)
            continue
        open(path, 'w').write(fn(s))
        print('patched', path)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
