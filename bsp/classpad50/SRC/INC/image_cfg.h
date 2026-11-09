//------------------------------------------------------------------------------
//  image_cfg.h - memory layout shared with the translator (dbt/Makefile)
//------------------------------------------------------------------------------
#ifndef __IMAGE_CFG_H
#define __IMAGE_CFG_H

// Guest RAM window the translator gives the kernel (P1 addresses)
#define IMAGE_WINDOW_PA_START           0x8C000000
#define IMAGE_WINDOW_PA_END             0x8C800000

// BOOTARGS page of config.bib
#define IMAGE_SHARE_ARGS_PA_START       0x8C000000
#define IMAGE_SHARE_ARGS_SIZE           0x00001000

// Kernel image start (config.bib NK)
#define IMAGE_WINCE_CODE_PA             0x80EE0000      // NK region: XIP in the NOR, the hole in Casio's OS image (config.bib; translator NK_WINDOW=hole)

// Translator debug ring: the last page of the window, excluded from RAM in
// config.bib. Layout (little-endian words): +0 write count, +4 read count
// (maintained by the translator), +8 magic 'RBGD', +16.. 4080 bytes of storage.
#define CLASSPAD_DBG_RING_PA            0x8C7FF000
#define CLASSPAD_DBG_RING_UA            0xAC7FF000
#define CLASSPAD_DBG_RING_BYTES         4080
#define CLASSPAD_DBG_RING_MAGIC         0x47424452

#endif
