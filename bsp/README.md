# bsp/ - Platform Builder BSP for SH7305 (to do)

Needs Windows with Visual Studio 2005 and Platform Builder 6.0 R3 (SH4 is
dropped in Windows Embedded Compact 7). Planned layout, following the PB
convention:

- `oal/`: startup.src (entry, SR/VBR setup), timer (TMU), intr (INTC), serial
  debug (SCIF), KITL, power, `OEMInit`, `OEMGetExtensionDRAM`.
- `oal/fpu/`: copies of `../../fpuemu/src/*.c` compiled into the kernel, plus
  the exception-path glue described in docs/02-fpu-emulation.md.
- `drivers/display`: flat framebuffer DDI (`ddi_flat` sample as a base).
- `drivers/keybd`, `drivers/touch`: KEYSC and ADC.
- `files/`: platform.bib, platform.reg, config.bib with RAMSTART 0x8C000000.

Keep the BSP sources C89-clean and free of float/double: the SH4 compiler
would emit FPU instructions and the kernel would trap on itself.
