# Target build: translator image (big-endian SH4AL-DSP) + little-endian guest
# test + raw .g3a launcher for casio-emu. Needs sh-elf-gcc (fxSDK toolchain).
PREFIX   ?= sh-elf-
CC       := $(PREFIX)gcc
OBJCOPY  := $(PREFIX)objcopy
OBJDUMP  := $(PREFIX)objdump
GUEST    ?= hello
EMU      ?= $(HOME)/Dev/casio-emu
BUILD    := build

# RAM layout (16 MiB at 0x8C000000), the single place these addresses live:
#   0x8C000000-0x8C0FFFFF  launcher workspace (casio-emu starts us with sp=0x8C080000)
#   0x8C100000-0x8CC7FFFF  guest window: 11.5 MiB (test guests linked at 0x8C100000)
#   0x8CC80000-0x8CDFFFFF  translator image (text+data ~60 KiB, bss 1.2 MiB) and its C stack below the top
#   0x8CE00000-0x8CFFFFFF  code cache, 2 MiB
GUEST_RAM_LO := 0x8C100000
GUEST_RAM_HI := 0x8CC80000
IMAGE_BASE   := 0x8CC80000
IMAGE_STACK  := 0x8CE00000
CACHE_BASE   := 0x8CE00000
CACHE_SIZE   := 0x00200000
LAYOUT_DEFS  := -DGUEST_RAM_LO=$(GUEST_RAM_LO)u -DGUEST_RAM_HI=$(GUEST_RAM_HI)u -DIMAGE_BASE=$(IMAGE_BASE) \
                -DCACHE_BASE=$(CACHE_BASE)u -DCACHE_SIZE=$(CACHE_SIZE)u
LAYOUT_LD    := -Wl,--defsym,IMAGE_BASE=$(IMAGE_BASE) -Wl,--defsym,_image_stack_top=$(IMAGE_STACK)

HOST_CFLAGS  := -mb -m4-nofpu -O2 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables \
                -Wall -Wextra -Wno-unused-parameter -I rt -I src -I ../fpuemu/src $(LAYOUT_DEFS) $(EXTRA_CFLAGS)
GUEST_CFLAGS := -ml -m4-nofpu -O2 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -Wall \
                -DGUEST_RAM_LO=$(GUEST_RAM_LO)u -DGUEST_RAM_HI=$(GUEST_RAM_HI)u

RT_SRC := rt/crt0.S rt/stubs.S rt/main.c rt/rt.c rt/nkbin.c rt/bench.c bench/loops.S src/sh4dec.c src/sh4emit.c src/translate.c \
          ../fpuemu/src/fpuemu.c ../fpuemu/src/softfloat.c

# A Windows CE .bin to run instead of the embedded guest: placed in the emulated
# NOR flash at NK_FLASH_OFF (P1 address 0x80000000 + offset), where the runtime
# looks for it first. Example: make NKBIN=path/to/NK.bin run
NKBIN       ?=
NK_FLASH_OFF ?= 0x01000000
# casio-emu copies flash files in 32-bit units, so the image is padded to a multiple of 4 first
NK_PADDED = $(BUILD)/nk-padded.bin
NK_ENV = $(if $(NKBIN),HLE_OSROM=$(abspath $(NK_PADDED)) HLE_OSROM_PAD=$(NK_FLASH_OFF),)   # recursive: nkrun sets NKBIN per target
define NK_PAD
python3 -c 'import sys;d=open(sys.argv[1],"rb").read();open(sys.argv[2],"wb").write(d+b"\0"*(-len(d)%4))' $(NKBIN) $(NK_PADDED)
endef

.PHONY: all clean run nkrun screenshot disasm

IMG := $(BUILD)/dbt-$(GUEST)

all: $(IMG).g3a

$(BUILD):
	mkdir -p $(BUILD)

# --- guest (little-endian) ---
GUEST_EXTRA_exc := guest/exc_vectors.S
GUEST_EXTRA_tmu := guest/tmu_vectors.S guest/tmu_loops.S
GUEST_EXTRA_bench := bench/loops.S
GUEST_EXTRA_apicall := guest/fault_vectors.S
GUEST_EXTRA_mmu := guest/fault_vectors.S
GUEST_EXTRA_aspace := guest/fault_vectors.S
GUEST_EXTRA     := $(GUEST_EXTRA_$(GUEST))
GUEST_START_nkimg := guest/nkimg_start.S
GUEST_START     := $(or $(GUEST_START_$(GUEST)),guest/start.S)

$(BUILD)/$(GUEST).elf: $(GUEST_START) guest/$(GUEST).c $(GUEST_EXTRA) guest/hyper.h guest/mmio.h guest/guest.ld | $(BUILD)
	$(CC) $(GUEST_CFLAGS) -T guest/guest.ld -o $@ $(GUEST_START) guest/$(GUEST).c $(GUEST_EXTRA)

# the guest wrapped as a CE .bin (records + ROMHDR pointer) for the loader test
$(BUILD)/$(GUEST).nk.bin: $(BUILD)/$(GUEST).bin ../tools/mkbin.py
	python3 ../tools/mkbin.py $< 0x8C100000 0x8C100000 $@ --chunk 256

$(BUILD)/$(GUEST).bin: $(BUILD)/$(GUEST).elf
	$(OBJCOPY) -O binary $< $@

# --- translator image (big-endian) ---
# image files are named after the guest so that switching GUEST= relinks
$(IMG).elf: $(RT_SRC) rt/guest_blob.S rt/image.ld rt/rt.h rt/nkbin.h rt/ctx_offsets.h $(BUILD)/$(GUEST).bin
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

run: $(IMG).g3a
	$(if $(NKBIN),$(NK_PAD))
	-$(NK_ENV) $(EMU)/run-headless.sh $(abspath $(IMG).g3a) 20

# run the current guest through the .bin loader (image in emulated flash)
nkrun: NKBIN = $(BUILD)/$(GUEST).nk.bin
nkrun: $(IMG).g3a $(BUILD)/$(GUEST).nk.bin
	$(NK_PAD)
	-$(NK_ENV) $(EMU)/run-headless.sh $(abspath $(IMG).g3a) 20

# run headless with the emulator dumping the presented frame (PPM) and convert the
# last one to $(IMG).png; HLE_FORCE_R61524 presents the LCD controller's pixels
screenshot: $(IMG).g3a
	-HLE_FBDUMP=$(abspath $(IMG).ppm) HLE_FORCE_R61524=1 $(EMU)/run-headless.sh $(abspath $(IMG).g3a) 25
	python3 ../tools/ppm2png.py $(IMG).ppm $(IMG).png

disasm: $(IMG).elf $(BUILD)/$(GUEST).elf
	$(OBJDUMP) -d $(IMG).elf > $(IMG).dis
	$(OBJDUMP) -d -EL $(BUILD)/$(GUEST).elf > $(BUILD)/$(GUEST).dis

clean:
	rm -rf $(BUILD)
