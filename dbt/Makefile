# Target build: translator image (big-endian SH4AL-DSP) + little-endian guest
# test + raw .g3a launcher for casio-emu. Needs sh-elf-gcc (fxSDK toolchain).
PREFIX   ?= sh-elf-
CC       := $(PREFIX)gcc
OBJCOPY  := $(PREFIX)objcopy
OBJDUMP  := $(PREFIX)objdump
GUEST    ?= hello
EMU      ?= $(HOME)/Dev/casio-emu
BUILD    := build

HOST_CFLAGS  := -mb -m4-nofpu -O2 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables \
                -Wall -Wextra -Wno-unused-parameter -I rt -I src -I ../fpuemu/src $(EXTRA_CFLAGS)
GUEST_CFLAGS := -ml -m4-nofpu -O2 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -Wall

RT_SRC := rt/crt0.S rt/stubs.S rt/main.c rt/rt.c rt/bench.c bench/loops.S src/sh4dec.c src/sh4emit.c src/translate.c \
          ../fpuemu/src/fpuemu.c ../fpuemu/src/softfloat.c

.PHONY: all clean run disasm

IMG := $(BUILD)/dbt-$(GUEST)

all: $(IMG).g3a

$(BUILD):
	mkdir -p $(BUILD)

# --- guest (little-endian) ---
GUEST_EXTRA_exc := guest/exc_vectors.S
GUEST_EXTRA_tmu := guest/tmu_vectors.S guest/tmu_loops.S
GUEST_EXTRA_bench := bench/loops.S
GUEST_EXTRA     := $(GUEST_EXTRA_$(GUEST))

$(BUILD)/$(GUEST).elf: guest/start.S guest/$(GUEST).c $(GUEST_EXTRA) guest/hyper.h guest/mmio.h guest/guest.ld | $(BUILD)
	$(CC) $(GUEST_CFLAGS) -T guest/guest.ld -o $@ guest/start.S guest/$(GUEST).c $(GUEST_EXTRA)

$(BUILD)/$(GUEST).bin: $(BUILD)/$(GUEST).elf
	$(OBJCOPY) -O binary $< $@

# --- translator image (big-endian) ---
# image files are named after the guest so that switching GUEST= relinks
$(IMG).elf: $(RT_SRC) rt/guest_blob.S rt/image.ld rt/rt.h rt/ctx_offsets.h $(BUILD)/$(GUEST).bin
	$(CC) $(HOST_CFLAGS) -DGUEST='"$(BUILD)/$(GUEST).bin"' -T rt/image.ld -o $@ $(RT_SRC) rt/guest_blob.S -lgcc

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
	-$(EMU)/run-headless.sh $(abspath $(IMG).g3a) 20

disasm: $(IMG).elf $(BUILD)/$(GUEST).elf
	$(OBJDUMP) -d $(IMG).elf > $(IMG).dis
	$(OBJDUMP) -d -EL $(BUILD)/$(GUEST).elf > $(BUILD)/$(GUEST).dis

clean:
	rm -rf $(BUILD)
