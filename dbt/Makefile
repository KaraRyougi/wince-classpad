# Target build: translator image (big-endian SH4AL-DSP) + little-endian guest
# test + raw .g3a launcher for casio-emu. Needs sh-elf-gcc (fxSDK toolchain).
PREFIX   ?= sh-elf-
CC       := $(PREFIX)gcc
OBJCOPY  := $(PREFIX)objcopy
OBJDUMP  := $(PREFIX)objdump
GUEST    ?= hello
EMU      ?= $(HOME)/Dev/casio-emu
# override BUILD per run (make BUILD=build-x ...) so runs can go in parallel; a
# trailing comment on the assignment would keep its spaces in the value
BUILD    ?= build

# RAM layout (16 MiB at 0x8C000000), the single place these addresses live:
#   0x8C000000-0x8C7FFFFF  guest window: 8 MiB (config.bib BOOTARGS/RAM/DBGRING;
#                          test guests are linked at GUEST_LINK 0x8C100000)
#   0x8C800000-0x8CCBFFFF  translator image (19200 headers, 2900 KiB arena,
#                          32768-entry lookup hash) and its 128 KiB C stack
#                          below the top
#   0x8CCC0000-0x8CFFFFFF  code cache, 3.25 MiB
# Since 2026-11-07 the split follows what fills a cache bank: with 4 MiB /
# 14400 / 2624 KiB, 33 of 34 banks recycled during a My Device load ran out
# of headers with only 56% of their code space used (a block averages ~160
# bytes of host code and ~150 of metadata), so code space went to headers,
# metadata and a larger hash (docs/04 "Layout by what fills a bank").
# The first MiB is CE's since 2026-11-06: it was the launcher's workspace, but
# nothing uses it after the no-return handoff (casio-emu's initial sp
# 0x8C080000 and the HollyHock stub only live there before it; crt0 moves to
# the image stack first). On 2026-11-06 CE briefly had 11.3 MiB (window to
# 0x8CB4FFFF, 2 MiB cache, 9600 headers, 1752 KiB arena); CE went back to
# 8 MiB the same day and the translator got the other 8 MiB: the 2026-11-05
# tables plus the freed MiB as code cache (3 -> 4 MiB).
# Until 2026-11-05 the window was 11.5 MiB with 4096 blocks and a 1 MiB cache;
# the Standard Shell boot hit the 4096-block cap 28 times before POSTINIT
# (115k translations), so the tables doubled at the expense of 3.5 MiB of CE RAM.
# Compact link storage now allows 14400 headers (56 B each) and a 3 MiB
# code cache without reducing the CE RAM window. Tables scale with BLOCKS and
# ARENA_KB (per-block metadata, packed instruction records plus auxiliary targets); the image region must hold
# text + bss + a 128 KiB stack (rt/image.ld asserts it). Override on the
# command line to try other sizes; config.bib's RAM line must agree with
# GUEST_RAM_HI (the window ends 4 KiB earlier at the debug ring).
GUEST_RAM_LO ?= 0x8C000000
GUEST_RAM_HI ?= 0x8C800000
GUEST_LINK   := 0x8C100000
IMAGE_BASE   ?= 0x8C800000
IMAGE_STACK  ?= 0x8CCC0000
CACHE_BASE   ?= 0x8CCC0000
CACHE_SIZE   ?= 0x00340000
BLOCKS       ?= 19200
ARENA_KB     ?= 2900
LAYOUT_DEFS  := -DGUEST_RAM_LO=$(GUEST_RAM_LO)u -DGUEST_RAM_HI=$(GUEST_RAM_HI)u -DGUEST_LINK=$(GUEST_LINK)u -DIMAGE_BASE=$(IMAGE_BASE) \
                -DCACHE_BASE=$(CACHE_BASE)u -DCACHE_SIZE=$(CACHE_SIZE)u -DMAX_BLOCKS=$(BLOCKS) -DMETA_ARENA_BYTES=$(ARENA_KB)*1024u
LAYOUT_LD    := -Wl,--defsym,IMAGE_BASE=$(IMAGE_BASE) -Wl,--defsym,_image_stack_top=$(IMAGE_STACK) -Wl,--defsym,GUEST_RAM_LO=$(GUEST_RAM_LO)

HOST_CFLAGS  := -mb -m4-nofpu -O2 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables \
                -Wall -Wextra -Wno-unused-parameter -I rt -I src -I ../fpuemu/src $(LAYOUT_DEFS) $(EXTRA_CFLAGS)
GUEST_CFLAGS := -ml -m4-nofpu -O2 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -Wall \
                -DGUEST_RAM_LO=$(GUEST_RAM_LO)u -DGUEST_RAM_HI=$(GUEST_RAM_HI)u -DGUEST_LINK=$(GUEST_LINK)u -DMAX_BLOCKS=$(BLOCKS)

RT_SRC := rt/crt0.S rt/stubs.S rt/standby.S rt/main.c rt/rt.c rt/kdev.c rt/lcdcon.c rt/irqpins.c rt/nkbin.c rt/bench.c bench/loops.S src/sh4dec.c src/sh4emit.c src/translate.c \
          ../fpuemu/src/fpuemu.c ../fpuemu/src/softfloat.c

# A Windows CE .bin to run instead of the embedded guest, placed in the emulated
# NOR flash by tools/nkflash.py: a RAMIMAGE build (image inside the guest
# window) goes as is to NK_FLASH_OFF (P1 address 0x80000000 + offset), where
# the runtime finds and copies it; a ROMIMAGE build (image start in the NOR's
# P1 range) is flattened to the bytes the NOR holds at its own address and
# runs in place (XIP). Example: make NKBIN=/abs/path/NK.bin run
NKBIN       ?=
# NOR offsets of the two CE regions (docs/03). The fx-CP400's 64 MiB chip has
# two places for them: the erased hole inside Casio's 21 MiB OS image,
# 0x00EE0000-0x013E0000 (5 MiB), which Casio's OS never writes after an OS
# update, and the eActivity area 0x3860000-0x3FFFFFF (7.625 MiB raw), which
# Casio's OS reformats at every boot that finds something else there.
# NK_WINDOW=hole (default since 2026-11-09): NK (config.bib ROMSTART, the
# image the translator boots) in the hole, EXT (the second XIP region,
# chained to the kernel by the OAL: classpad_extrom.h holds the same two
# numbers) in the eActivity area. NK_WINDOW=eact: the layout of the images
# before that date, NK in the eActivity area and EXT in the hole. NK_FLASH is
# the NK offset as a P1 address; NK_FLASH_OFF/NK_FLASH_MAX can also be given
# directly (an emulator run of a 7 MiB image linked at 0x81000000:
# NK_FLASH_OFF=0x01000000 NK_FLASH_MAX=0x00800000). wince.hhk/nkflash.hhk
# flash \fls0\NK.bin and \fls0\EXT.bin into the two windows; the *_WINDOW_EACT
# define tells them which window is the eActivity volume (theirs to erase).
# EXTBIN=<EXT.bin> adds the EXT region to an emulator run's NOR; the
# translator keeps its blocks over whole-cache flushes like NK's.
NK_WINDOW    ?= hole
ifeq ($(NK_WINDOW),eact)
NK_FLASH_OFF  ?= 0x03860000
NK_FLASH_MAX  ?= 0x007A0000
EXT_FLASH     ?= 0x80EE0000
EXT_FLASH_MAX ?= 0x00500000
HOST_CFLAGS   += -DNK_WINDOW_EACT
else
NK_FLASH_OFF  ?= 0x00EE0000
NK_FLASH_MAX  ?= 0x00500000
EXT_FLASH     ?= 0x83860000
EXT_FLASH_MAX ?= 0x007A0000
HOST_CFLAGS   += -DEXT_WINDOW_EACT
endif
NK_FLASH     := $(shell printf '0x%08X' $$((0x80000000 + $(NK_FLASH_OFF))))
FLASH_DEFS   := -DNK_FLASH=$(NK_FLASH)u -DNK_FLASH_MAX=$(NK_FLASH_MAX)u -DEXT_FLASH=$(EXT_FLASH)u -DEXT_FLASH_MAX=$(EXT_FLASH_MAX)u
HOST_CFLAGS  += $(FLASH_DEFS)
# the extrom guest checks a small test region from inside the translator
ifeq ($(GUEST),extrom)
EXTBIN       ?= $(BUILD)/extrom-test.bin
endif
EXTBIN       ?=
# make DEVICE=1 hhk: a translator without the emulator's debug port (byte
# stores to 0xFFFFF000, undefined on the real SoC); its prints then go only to
# the debug ring. Use a private BUILD dir so emulator builds keep their port.
DEVICE ?= 0
# CONSOLE=1 adds the panel console (rt/lcdcon.c) to a DEVICE=1 build. Off by
# default: on the real fx-CP400 a console build ends in a black screen while
# the same translator without it reaches the desktop (2026-11-05); the cause
# is not known (the emulator shows no difference). The IRQ-pin guard
# (rt/irqpins.c) is in every DEVICE=1 build: it only masks a storming pin.
CONSOLE ?= 0
# make STANDBY_DIAG=1: the R-standby resume path paints one coloured stripe per
# stage on the panel (rt/standby.S, rt.c; docs/06 section 8). Diagnostic only.
STANDBY_DIAG ?= 0
HOST_CFLAGS  += $(if $(filter 1,$(STANDBY_DIAG)),-DDBT_STANDBY_DIAG,)
# make STANDBY_SEARCH=1: no standby loop in the resume stub; every wake-up
# comes back to rt.c, which steps through the wake-source suspects (rt.c).
STANDBY_SEARCH ?= 0
HOST_CFLAGS  += $(if $(filter 1,$(STANDBY_SEARCH)),-DDBT_STANDBY_SEARCH,)
HOST_CFLAGS  += $(if $(filter 1,$(DEVICE)),-DDBT_NO_DEBUG_PORT -DDBT_IRQPIN_GUARD $(if $(filter 1,$(CONSOLE)),-DDBT_LCD_CONSOLE,),)
# Code cache policy (docs/04 "Metadata on demand, second chance"):
# LAZY_META=1 keeps no recovery metadata for ROM blocks until an exception
# lands in one (the block is translated again to rebuild it); SECOND_CHANCE=1
# keeps the blocks used while their bank waited for reuse, moved to its start.
LAZY_META     ?= 0
SECOND_CHANCE ?= 1
HOST_CFLAGS  += $(if $(filter 1,$(LAZY_META)),-DDBT_LAZY_META,) $(if $(filter 1,$(SECOND_CHANCE)),-DDBT_SECOND_CHANCE,)
# KDEV=1: only the kernel functions listed in rt/kernel_dev.h (CE 6.0
# kernel.dll) or rt/kernel_dev50.h (CE 5.0 nk.exe = kernel + OAL), found by
# signature at boot (rt/kdev.c), get the device-aware P4 test; the rest of
# the kernel module is translated like any other code. Regenerate the lists
# with tools/kernel_devfuncs.py NK.bin kern.map -o rt/kernel_dev.h (CE 6.0:
# kern.dll's map) and ... --table kdev_funcs50 -o rt/kernel_dev50.h (CE 5.0:
# kern.exe's map).
KDEV          ?= 1
HOST_CFLAGS  += $(if $(filter 1,$(KDEV)),-DDBT_KDEV,)
# HASH_TAG=1: lookup hash entries carry their ASID and user keys mix the ASID
# into the index (CE processes share virtual addresses), so an address-space
# switch no longer empties the user entries (72% of the C lookups that found
# their block outside the hash), and an IC miss on a user target is served
# from the hash in the stub instead of in C. HASH_ENTRIES: lookup hash size
# (12 bytes each; 32768 since the 19200-header layout, docs/04).
HASH_TAG      ?= 1
# USER_IC_REFILL=1 (with HASH_TAG=1): such a hash hit also refills the site's
# inline cache, so the next jump from there stays in translated code. Without
# it a user IC emptied by an address-space switch was never filled again.
USER_IC_REFILL ?= 1
HASH_ENTRIES  ?= 32768
# TLB_REFILL=1: UTLB misses in translated code are refilled by the translator
# with the kernel's own page-table walk (rt.c tlb_refill: CE 6.0's page
# directories, or CE 5.0's SectionTable/MEMBLOCK walk) instead of an
# exception round trip into the guest's TLBMissHandler; only while VBR +
# 0x400 holds exactly one of those two handlers.
TLB_REFILL    ?= 1
HOST_CFLAGS  += $(if $(filter 1,$(TLB_REFILL)),-DDBT_TLB_REFILL,)
# FAST_HELPERS=1: common helper calls without C. Control-register loads run
# natively inline (DBT_INLINE_DEVICE_READ; stores keep the C helper, PTEH and
# MMUCR have side effects); ldc sr with unchanged bank and ASID, including
# a change of the shadowed MD (DBT_FAST_LDC_SR, _MD); ldtlb on a page without
# translated user code (DBT_FAST_LDTLB); rte to a target in the lookup hash
# (DBT_FAST_RTE). docs/04 has the measurements.
FAST_HELPERS  ?= 1
HOST_CFLAGS  += $(if $(filter 1,$(FAST_HELPERS)),-DDBT_INLINE_DEVICE_READ -DDBT_FAST_LDC_SR -DDBT_FAST_LDC_SR_MD -DDBT_FAST_LDTLB -DDBT_FAST_RTE,)
HOST_CFLAGS  += $(if $(filter 1,$(HASH_TAG)),-DDBT_HASH_ASID_TAG -DDBT_HASH_ASID_MIX -DDBT_USER_IC_HASH_HIT $(if $(filter 1,$(USER_IC_REFILL)),-DDBT_USER_IC_REFILL,),) -DDBT_HASH_ENTRIES=$(HASH_ENTRIES)
NK_PADDED = $(BUILD)/nk-flash.bin
NK_OFF_FILE = $(BUILD)/nk-flash.off
NK_ENV = $(if $(NKBIN)$(EXTBIN),HLE_OSROM=$(abspath $(NK_PADDED)) HLE_OSROM_PAD=$$(cat $(NK_OFF_FILE)),)   # recursive: nkrun sets NKBIN per target
# casio-emu remembers the last machine profile (model.txt), so pin it. The
# ClassPad profile (fx-cp400: 320x528 panel, IIC0 touch sensor, INTC
# priorities) is the target and the default since 2026-11-05; it needs
# dbt/emu-patch/add_classpad_addin.py (and add_sdmr.py for GUEST=standby) applied to the emulator. EMU_MODEL=fx-cg100
# still runs the fx-CG100 profile but it is no longer part of the workflow.
EMU_MODEL ?= fx-cp400
EMU_ENV = CALCEMU_MODEL=$(EMU_MODEL)
# seconds before run-headless.sh kills the emulator (a CE boot needs minutes: make RUN_SECS=300 NKBIN=... run)
RUN_SECS ?= 20
# make DEVALL=1 GUEST=x run: the guest asks for device-aware translation of its whole code (start.S)
GUEST_CFLAGS += $(if $(filter 1,$(DEVALL)),-DDEVAWARE_ALL,)
define NK_PAD
mkdir -p $(BUILD) && python3 ../tools/nkflash.py $(or $(NKBIN),-) $(NK_PADDED) $(NK_FLASH_OFF) $(if $(EXTBIN),--ext $(EXTBIN)) > $(NK_OFF_FILE) && echo "flash offset $$(cat $(NK_OFF_FILE))"
endef

.PHONY: all clean run nkrun nkshot screenshot disasm hhk hhk-check hhkrun flashtest

IMG := $(BUILD)/dbt-$(GUEST)

all: $(IMG).g3a

$(BUILD):
	mkdir -p $(BUILD)

# --- guest (little-endian) ---
GUEST_EXTRA_exc := guest/exc_vectors.S
GUEST_EXTRA_power := guest/tmu_vectors.S
GUEST_EXTRA_standby := guest/tmu_vectors.S
GUEST_EXTRA_tmu := guest/tmu_vectors.S guest/tmu_loops.S
GUEST_EXTRA_bench := bench/loops.S
GUEST_EXTRA_apicall := guest/fault_vectors.S
GUEST_EXTRA_mmu := guest/fault_vectors.S
GUEST_EXTRA_aspace := guest/fault_vectors.S
GUEST_EXTRA_tlbrefill := guest/tlbrefill_vectors.S
GUEST_EXTRA_bank := guest/bank_vectors.S
GUEST_EXTRA_devmem := guest/devmem_asm.S
GUEST_EXTRA_tlbdata := guest/fault_vectors.S guest/tlbdata_asm.S
GUEST_EXTRA_fpudis := guest/fault_vectors.S guest/fpudis_asm.S
GUEST_EXTRA     := $(GUEST_EXTRA_$(GUEST))
GUEST_START_nkimg := guest/nkimg_start.S
GUEST_START_nkxip := guest/nkxip_start.S
GUEST_START     := $(or $(GUEST_START_$(GUEST)),guest/start.S)
GUEST_LD_nkxip  := $(BUILD)/guest_xip.ld
GUEST_LD        := $(or $(GUEST_LD_$(GUEST)),guest/guest.ld)

# guests are rebuilt when DEVALL changes (the flag lives in a stamp file name);
# guests and the image when the RAM layout or the table sizes change
DEVALL_STAMP := $(BUILD)/.devall-$(if $(filter 1,$(DEVALL)),1,0)
$(DEVALL_STAMP): | $(BUILD)
	rm -f $(BUILD)/.devall-*
	touch $@
LAYOUT_STAMP := $(BUILD)/.layout-$(GUEST_RAM_LO)-$(HHK_PAYLOAD)-$(GUEST_RAM_HI)-$(IMAGE_BASE)-$(CACHE_BASE)-$(CACHE_SIZE)-$(BLOCKS)-$(ARENA_KB)-$(NK_FLASH)-$(NK_FLASH_MAX)-$(EXT_FLASH)-$(DEVICE)-$(CONSOLE)-$(IRQPINS)-$(LAZY_META)$(SECOND_CHANCE)$(KDEV)$(HASH_TAG)$(TLB_REFILL)$(FAST_HELPERS)-$(HASH_ENTRIES)
$(LAYOUT_STAMP): | $(BUILD)
	rm -f $(BUILD)/.layout-*
	touch $@

$(BUILD)/$(GUEST).elf: $(GUEST_START) guest/$(GUEST).c $(GUEST_EXTRA) guest/hyper.h guest/mmio.h ../bsp/classpad/SRC/INC/classpad_extrom.h ../bsp/classpad/SRC/INC/classpad_power.h ../bsp/classpad/SRC/INC/classpad_keysc.h ../bsp/classpad/SRC/INC/bsp_base_regs.h ../bsp/classpad/SRC/INC/classpad_touch_bus.h ../bsp/classpad/SRC/INC/classpad_touch.h $(GUEST_LD) $(DEVALL_STAMP) $(LAYOUT_STAMP) | $(BUILD)
	$(CC) $(GUEST_CFLAGS) -T $(GUEST_LD) -o $@ $(GUEST_START) guest/$(GUEST).c $(GUEST_EXTRA)

# the guest wrapped as a CE .bin (records + ROMHDR pointer) for the loader test;
# nkxip is linked at the NOR address (NK_FLASH, through a build-dir copy of
# its linker script) and tools/nkflash.py puts it back flat
NK_IMG_BASE := $(if $(filter nkxip,$(GUEST)),$(NK_FLASH),0x8C100000)
GUEST_CFLAGS += $(if $(filter nkxip,$(GUEST)),-DROM_BASE=$(NK_FLASH)u,)
$(BUILD)/guest_xip.ld: guest/guest_xip.ld | $(BUILD)
	sed 's/0x81000000/$(NK_FLASH)/' $< > $@
$(BUILD)/$(GUEST).nk.bin: $(BUILD)/$(GUEST).bin ../tools/mkbin.py
	python3 ../tools/mkbin.py $< $(NK_IMG_BASE) $(NK_IMG_BASE) $@ --chunk 256

$(BUILD)/$(GUEST).bin: $(BUILD)/$(GUEST).elf
	$(OBJCOPY) -O binary $< $@

# --- a test EXT region for the extrom guest (tools/mkextrom.py; the real one comes from makeimg) ---
$(BUILD)/extrom-test.bin: guest/extrom.txt guest/extrom-data.txt ../tools/mkextrom.py $(LAYOUT_STAMP) | $(BUILD)
	python3 ../tools/mkextrom.py $< $@ --base $(EXT_FLASH) --max $(EXT_FLASH_MAX)

# --- translator image (big-endian) ---
# image files are named after the guest so that switching GUEST= relinks
$(IMG).elf: $(RT_SRC) $(wildcard src/*.h) rt/guest_blob.S rt/image.ld rt/rt.h rt/nkbin.h rt/kdev.h rt/kernel_dev.h rt/kernel_dev50.h rt/ctx_offsets.h rt/lcdcon.h rt/lcdfont.h $(BUILD)/$(GUEST).bin $(LAYOUT_STAMP)
	$(CC) $(HOST_CFLAGS) $(LAYOUT_LD) -DGUEST='"$(BUILD)/$(GUEST).bin"' -T rt/image.ld -o $@ $(RT_SRC) rt/guest_blob.S -lgcc

$(IMG).bin: $(IMG).elf
	$(OBJCOPY) -O binary $< $@

# --- launcher ---
$(IMG)-loader.elf: loader/loader.S loader/loader.ld $(IMG).bin
	$(CC) $(HOST_CFLAGS) -DIMAGE='"$(IMG).bin"' -T loader/loader.ld -o $@ loader/loader.S

$(IMG)-loader.bin: $(IMG)-loader.elf
	$(OBJCOPY) -O binary $< $@

# casio-emu reads the payload from file offset 0x7000 (g3a header size)
$(IMG).g3a: $(IMG)-loader.bin
	( dd if=/dev/zero bs=28672 count=1 2>/dev/null; cat $< ) > $@

# --- HollyHock program for the real fx-CP400 (dbt/hhk, docs/03, loader/README.md) ---
# wince.hhk: the stub (hhk/launcher.c, loaded by HollyHock at 0x8CFF0000) with
# the translator image $(IMG).bin as a second loadable region at IMAGE_BASE;
# the stub quiets the OS's interrupt sources and jumps into it the way the
# .g3a launcher does (hhk/takeover.S). nkflash.hhk writes \fls0\NK.bin and
# \fls0\EXT.bin into the NOR windows at NK_FLASH and EXT_FLASH. Both are plain
# ELF files (also copied as .hh3).
HHK_BASE   := 0x8CFF0000
HHK_LIMIT  := 0x8D000000
# Where HollyHock's own copy puts the translator payload (~64 KiB) while
# Casio's OS still runs (the flasher calls into it after that); takeover then
# moves it to IMAGE_BASE with the OS shut out. 0x8C900000 was IMAGE_BASE on
# the board from 2026-11-05; IMAGE_BASE 0x8C800000 (2026-11-06) never was.
# Must not be below IMAGE_BASE (the copy runs forward; hhk.ld asserts it).
HHK_PAYLOAD ?= 0x8C900000
HHK_VERSION := $(shell git -C .. log -1 --format=%cd --date=short 2>/dev/null || echo dev)
# IRQPINS=mask (default): takeover masks the IRQ pins (INTMSK00) like every
# other interrupt source; keep: leaves them as Casio's OS had them (the touch
# contact interrupt may be one), with the translator's storm guard watching.
IRQPINS    ?= mask
HHK_CFLAGS := $(HOST_CFLAGS) -I hhk -DHHK_APP_VERSION='"$(HHK_VERSION)"' $(if $(filter keep,$(IRQPINS)),-DHHK_IRQPINS_KEEP,)
HHK_LD     := -T hhk/hhk.ld -Wl,--defsym,HHK_BASE=$(HHK_BASE) -Wl,--defsym,HHK_LIMIT=$(HHK_LIMIT) -Wl,--defsym,PAYLOAD_BASE=$(HHK_PAYLOAD) -Wl,--defsym,IMAGE_BASE=$(IMAGE_BASE)
HHK_COMMON := hhk/start.S hhk/util.c hhk/os.c

.PHONY: lcdprobe
lcdprobe: $(BUILD)/lcdprobe.hh3
$(BUILD)/lcdprobe.hh3: $(HHK_COMMON) hhk/lcdprobe.c rt/lcd_io.h hhk/hhk.ld | $(BUILD)
	$(CC) $(HHK_CFLAGS) -DHHK_APP_NAME='"LCD takeover probe"' -DHHK_APP_DESC='"Step through display handoff without booting CE or flashing"' $(HHK_LD) -o $@ $(HHK_COMMON) hhk/lcdprobe.c -lgcc
	python3 ../tools/hhkcheck.py $@
PAYLOAD_H  := $(BUILD)/dbt-$(GUEST)-payload.h
WINCE_HHK  := $(BUILD)/wince.hhk
NKFLASH_HHK := $(BUILD)/nkflash.hhk

$(PAYLOAD_H): $(IMG).bin ../tools/hhkpayload.py
	python3 ../tools/hhkpayload.py $< > $@

# wince.hhk carries the flasher too: NK.bin on the calculator is written to
# the window when it differs from what is there, then CE boots
HHK_FLASH := hhk/nkflash.c hhk/norflash.c
$(WINCE_HHK): $(HHK_COMMON) $(HHK_FLASH) hhk/launcher.c hhk/takeover.S hhk/payload.S hhk/hhk.ld hhk/util.h hhk/os.h hhk/nkflash.h hhk/norflash.h $(IMG).bin $(PAYLOAD_H) $(LAYOUT_STAMP)
	$(CC) $(HHK_CFLAGS) -include $(PAYLOAD_H) -DIMAGE='"$(IMG).bin"' $(HHK_LD) -o $@ $(HHK_COMMON) $(HHK_FLASH) hhk/launcher.c hhk/takeover.S hhk/payload.S -lgcc
	cp $@ $(@:.hhk=.hh3)

$(NKFLASH_HHK): $(HHK_COMMON) $(HHK_FLASH) hhk/hhk.ld hhk/util.h hhk/os.h hhk/nkflash.h hhk/norflash.h $(LAYOUT_STAMP)
	$(CC) $(HHK_CFLAGS) -DNKFLASH_STANDALONE -DHHK_APP_NAME='"CE image flasher"' -DHHK_APP_DESC='"writes fls0/NK.bin (and EXT.bin) into the NOR for Windows CE"' $(HHK_LD) -o $@ $(HHK_COMMON) $(HHK_FLASH) -lgcc
	cp $@ $(@:.hhk=.hh3)

hhk: $(WINCE_HHK) $(NKFLASH_HHK) hhk-check

# the .hhk carries the very bytes the emulator runs (and loads where the layout says)
hhk-check: $(WINCE_HHK) $(NKFLASH_HHK) $(IMG).g3a ../tools/hhkcheck.py
	python3 ../tools/hhkcheck.py $(WINCE_HHK) $(IMG).bin $(IMG).g3a $(HHK_PAYLOAD)
	python3 ../tools/hhkcheck.py $(NKFLASH_HHK)

# the same stub as a casio-emu payload: hhk_takeover (quiesce, copy, cache sync,
# jump) runs the current guest in the emulator. make GUEST=hello hhkrun
HHK_EMU_SRC := hhk/start.S hhk/util.c hhk/launcher.c hhk/takeover.S hhk/payload.S
$(BUILD)/dbt-$(GUEST)-hhk.elf: $(HHK_EMU_SRC) hhk/hhk-emu.ld $(IMG).bin $(PAYLOAD_H)
	$(CC) $(HOST_CFLAGS) -I hhk -DHHK_EMU -include $(PAYLOAD_H) -DIMAGE='"$(IMG).bin"' -T hhk/hhk-emu.ld -o $@ $(HHK_EMU_SRC) -lgcc

$(BUILD)/dbt-$(GUEST)-hhk.g3a: $(BUILD)/dbt-$(GUEST)-hhk.elf
	$(OBJCOPY) -O binary $< $(<:.elf=.bin)
	( dd if=/dev/zero bs=28672 count=1 2>/dev/null; cat $(<:.elf=.bin) ) > $@

hhkrun: $(BUILD)/dbt-$(GUEST)-hhk.g3a
	$(if $(NKBIN)$(EXTBIN),$(NK_PAD))
	-$(EMU_ENV) $(NK_ENV) $(EMU)/run-headless.sh $(abspath $<) $(RUN_SECS)

# norflash.c against the emulator's NOR command decoder (HLE_FLASH_CMD=1)
$(BUILD)/flashtest.elf: hhk/start.S hhk/util.c hhk/norflash.c hhk/flashtest.c hhk/hhk-emu.ld | $(BUILD)
	$(CC) $(HOST_CFLAGS) -I hhk -DHHK_EMU -T hhk/hhk-emu.ld -o $@ hhk/start.S hhk/util.c hhk/norflash.c hhk/flashtest.c -lgcc

$(BUILD)/flashtest.g3a: $(BUILD)/flashtest.elf
	$(OBJCOPY) -O binary $< $(<:.elf=.bin)
	( dd if=/dev/zero bs=28672 count=1 2>/dev/null; cat $(<:.elf=.bin) ) > $@

flashtest: $(BUILD)/flashtest.g3a
	-$(EMU_ENV) HLE_FLASH_CMD=1 $(EMU)/run-headless.sh $(abspath $<) 10

# per-guest emulator environment: the standby guest needs an injected Clear
# press (chord 20, held) while it sleeps in R-standby (ticks = 1/32768 s)
RUN_ENV_standby := HLE_CB_CHORD=20 HLE_CB_CHORD_AT=250000 HLE_CB_CHORD_HOLD=100000
RUN_ENV ?= $(RUN_ENV_$(GUEST))
run: $(IMG).g3a $(if $(filter extrom,$(GUEST)),$(BUILD)/extrom-test.bin)
	$(if $(NKBIN)$(EXTBIN),$(NK_PAD))
	-$(EMU_ENV) $(RUN_ENV) $(NK_ENV) $(EMU)/run-headless.sh $(abspath $(IMG).g3a) $(RUN_SECS)

# run the current guest through the .bin loader (image in emulated flash)
nkrun: NKBIN = $(BUILD)/$(GUEST).nk.bin
nkrun: $(IMG).g3a $(BUILD)/$(GUEST).nk.bin
	$(NK_PAD)
	-$(EMU_ENV) $(NK_ENV) $(EMU)/run-headless.sh $(abspath $(IMG).g3a) $(RUN_SECS)

# run headless with the emulator dumping the presented frame (PPM) and convert the
# last one to $(IMG).png; HLE_FORCE_R61524 presents the LCD controller's pixels
# nkshot: the same for a CE image (NKBIN=..., RUN_SECS=n) -> build/nk.png
nkshot: $(IMG).g3a
	$(if $(NKBIN)$(EXTBIN),$(NK_PAD))
	-$(EMU_ENV) $(NK_ENV) HLE_FBDUMP=$(abspath $(BUILD)/nk.ppm) HLE_FORCE_R61524=1 $(EMU)/run-headless.sh $(abspath $(IMG).g3a) $(RUN_SECS)
	python3 ../tools/ppm2png.py $(BUILD)/nk.ppm $(BUILD)/nk.png

screenshot: $(IMG).g3a
	-$(EMU_ENV) HLE_FBDUMP=$(abspath $(IMG).ppm) HLE_FORCE_R61524=1 $(EMU)/run-headless.sh $(abspath $(IMG).g3a) 25
	python3 ../tools/ppm2png.py $(IMG).ppm $(IMG).png

disasm: $(IMG).elf $(BUILD)/$(GUEST).elf
	$(OBJDUMP) -d $(IMG).elf > $(IMG).dis
	$(OBJDUMP) -d -EL $(BUILD)/$(GUEST).elf > $(BUILD)/$(GUEST).dis

clean:
	rm -rf $(BUILD)
