//------------------------------------------------------------------------------
//  image_cfg.h - memory layout shared with the translator (dbt/Makefile)
//------------------------------------------------------------------------------
#ifndef __IMAGE_CFG_H
#define __IMAGE_CFG_H

// Guest RAM window the translator gives the kernel (P1 addresses)
#define IMAGE_WINDOW_PA_START           0x8C100000
#define IMAGE_WINDOW_PA_END             0x8C900000

// BOOTARGS page of config.bib
#define IMAGE_SHARE_ARGS_PA_START       0x8C100000
#define IMAGE_SHARE_ARGS_SIZE           0x00001000

// Kernel image start (config.bib NK)
#define IMAGE_WINCE_CODE_PA             0x81000000      // NK region: XIP in the NOR (config.bib)

// Translator debug ring: the last page of the window, excluded from RAM in
// config.bib. Layout (little-endian words): +0 write count, +4 read count
// (maintained by the translator), +8 magic 'RBGD', +16.. 4080 bytes of storage.
#define CLASSPAD_DBG_RING_PA            0x8C8FF000
#define CLASSPAD_DBG_RING_UA            0xAC8FF000
#define CLASSPAD_DBG_RING_BYTES         4080
#define CLASSPAD_DBG_RING_MAGIC         0x47424452

#endif
