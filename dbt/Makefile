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
                -Wall -Wextra -Wno-unused-parameter -I rt -I src -I ../fpuemu/src
GUEST_CFLAGS := -ml -m4-nofpu -O2 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -Wall

RT_SRC := rt/crt0.S rt/stubs.S rt/main.c rt/rt.c src/sh4dec.c src/sh4emit.c src/translate.c \
          ../fpuemu/src/fpuemu.c ../fpuemu/src/softfloat.c

.PHONY: all clean run disasm

all: $(BUILD)/dbt.g3a

$(BUILD):
	mkdir -p $(BUILD)

# --- guest (little-endian) ---
GUEST_EXTRA_exc := guest/exc_vectors.S
GUEST_EXTRA_tmu := guest/tmu_vectors.S guest/tmu_loops.S
GUEST_EXTRA     := $(GUEST_EXTRA_$(GUEST))

$(BUILD)/$(GUEST).elf: guest/start.S guest/$(GUEST).c $(GUEST_EXTRA) guest/hyper.h guest/mmio.h guest/guest.ld | $(BUILD)
	$(CC) $(GUEST_CFLAGS) -T guest/guest.ld -o $@ guest/start.S guest/$(GUEST).c $(GUEST_EXTRA)

$(BUILD)/$(GUEST).bin: $(BUILD)/$(GUEST).elf
	$(OBJCOPY) -O binary $< $@

# --- translator image (big-endian) ---
$(BUILD)/dbt.elf: $(RT_SRC) rt/guest_blob.S rt/image.ld rt/rt.h rt/ctx_offsets.h $(BUILD)/$(GUEST).bin
	$(CC) $(HOST_CFLAGS) -DGUEST='"$(BUILD)/$(GUEST).bin"' -T rt/image.ld -o $@ $(RT_SRC) rt/guest_blob.S -lgcc

$(BUILD)/dbt.bin: $(BUILD)/dbt.elf
	$(OBJCOPY) -O binary $< $@

# --- launcher ---
$(BUILD)/loader.elf: loader/loader.S loader/loader.ld $(BUILD)/dbt.bin
	$(CC) $(HOST_CFLAGS) -DIMAGE='"$(BUILD)/dbt.bin"' -T loader/loader.ld -o $@ loader/loader.S

$(BUILD)/loader.bin: $(BUILD)/loader.elf
	$(OBJCOPY) -O binary $< $@

# casio-emu reads the payload from file offset 0x7000 (g3a header size)
$(BUILD)/dbt.g3a: $(BUILD)/loader.bin
	( dd if=/dev/zero bs=28672 count=1 2>/dev/null; cat $< ) > $@

run: $(BUILD)/dbt.g3a
	-$(EMU)/run-headless.sh $(abspath $(BUILD)/dbt.g3a) 20

disasm: $(BUILD)/dbt.elf $(BUILD)/$(GUEST).elf
	$(OBJDUMP) -d $(BUILD)/dbt.elf > $(BUILD)/dbt.dis
	$(OBJDUMP) -d -EL $(BUILD)/$(GUEST).elf > $(BUILD)/$(GUEST).dis

clean:
	rm -rf $(BUILD)
