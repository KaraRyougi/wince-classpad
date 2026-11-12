CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c99
TEST    := fpuemu/tests/test_host
SRC     := fpuemu/src/softfloat.c fpuemu/src/fpuemu.c
HDR     := fpuemu/src/softfloat.h fpuemu/src/fpuemu.h
DBT_SRC  := dbt/src/sh4dec.c dbt/src/sh4emit.c dbt/src/translate.c $(SRC)
DBT_TEST := dbt/tests/test_translate

.PHONY: test clean

PEGHELP  := bsp/classpad50/SRC/APPS/PEGHELP
# Math Xpander's \Windows files (stage.py's --xpander); the test skips them when absent
XPANDER_WINDOWS ?= $(HOME)/Downloads/Xpander_SH3/files/Windows
# an fx-CP400 chip image for the storage-memory reader's test; the test skips the real volume when absent
FUGUE_NOR ?= $(HOME)/Downloads/nor-fx-cp400.bin
test: $(TEST) $(DBT_TEST) $(DBT_TEST)_icrefill bsp/tests/test_touch_bus bsp/tests/test_timer bsp/tests/test_timer50 bsp/tests/test_extrom bsp/tests/extrom-test.bin bsp/tests/test_peghelp bsp/tests/test_lcdrot bsp/tests/test_battery bsp/tests/test_fugue bsp/tests/test_fuguewr bsp/tests/test_clock
	./$(TEST)
	./$(DBT_TEST)
	./$(DBT_TEST)_icrefill
	./bsp/tests/test_touch_bus
	./bsp/tests/test_timer
	./bsp/tests/test_timer50
	./bsp/tests/test_peghelp "$(XPANDER_WINDOWS)"
	./bsp/tests/test_extrom bsp/tests/extrom-test.bin
	./bsp/tests/test_lcdrot
	./bsp/tests/test_battery
	./bsp/tests/test_clock
	./bsp/tests/test_fugue "$(FUGUE_NOR)"
	./bsp/tests/test_fuguewr "$(FUGUE_NOR)"

$(DBT_TEST): dbt/tests/test_translate.c $(DBT_SRC) dbt/src/sh4dec.h dbt/src/sh4emit.h dbt/src/translate.h $(HDR)
	$(CC) $(CFLAGS) -Idbt/src -Ifpuemu/src -o $@ dbt/tests/test_translate.c $(DBT_SRC)

# the same with the inline cache's block-index word (the target build's default, dbt/Makefile USER_IC_REFILL)
$(DBT_TEST)_icrefill: dbt/tests/test_translate.c $(DBT_SRC) dbt/src/sh4dec.h dbt/src/sh4emit.h dbt/src/translate.h $(HDR)
	$(CC) $(CFLAGS) -DDBT_USER_IC_REFILL -Idbt/src -Ifpuemu/src -o $@ dbt/tests/test_translate.c $(DBT_SRC)

$(TEST): fpuemu/tests/test_host.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -Ifpuemu/src -o $@ fpuemu/tests/test_host.c $(SRC) -lm

clean:
	rm -f $(TEST) $(DBT_TEST) $(DBT_TEST)_icrefill bsp/tests/test_touch_bus bsp/tests/test_timer bsp/tests/test_timer50 bsp/tests/test_extrom bsp/tests/extrom-test.bin bsp/tests/test_peghelp bsp/tests/test_lcdrot bsp/tests/test_battery bsp/tests/test_fugue bsp/tests/test_fuguewr bsp/tests/test_clock

bsp/tests/test_touch_bus: bsp/tests/test_touch_bus.c bsp/classpad/SRC/INC/classpad_touch_bus.h bsp/classpad/SRC/INC/classpad_touch.h
	$(CC) $(CFLAGS) -o $@ $<

bsp/tests/test_timer: bsp/tests/test_timer.c bsp/classpad/SRC/OAL/OALLIB/timer.c bsp/classpad/SRC/INC/bsp_base_regs.h
	$(CC) $(CFLAGS) -Wno-unused-parameter -o $@ $<

# peghelp.exe's HTML core is plain C89, so the host builds it too
bsp/tests/test_peghelp: bsp/tests/test_peghelp.c $(PEGHELP)/hpcore.c $(PEGHELP)/hpcore.h
	$(CC) $(CFLAGS) -o $@ bsp/tests/test_peghelp.c $(PEGHELP)/hpcore.c

bsp/tests/test_timer50: bsp/tests/test_timer50.c bsp/classpad50/SRC/OAL/OALLIB/timer.c bsp/classpad50/SRC/INC/bsp_base_regs.h
	$(CC) $(CFLAGS) -Wno-unused-parameter -o $@ $<

bsp/tests/test_clock: bsp/tests/test_clock.c bsp/classpad50/SRC/INC/classpad_clock.h
	$(CC) $(CFLAGS) -o $@ bsp/tests/test_clock.c

bsp/tests/test_battery: bsp/tests/test_battery.c bsp/classpad50/SRC/INC/classpad_battery.h
	$(CC) $(CFLAGS) -o $@ bsp/tests/test_battery.c

bsp/tests/test_lcdrot: bsp/tests/test_lcdrot.c bsp/classpad50/SRC/INC/classpad_lcdrot.h
	$(CC) $(CFLAGS) -o $@ bsp/tests/test_lcdrot.c

bsp/tests/test_fugue: bsp/tests/test_fugue.c bsp/classpad50/SRC/INC/classpad_fugue.h
	$(CC) $(CFLAGS) -o $@ $<

bsp/tests/test_fuguewr: bsp/tests/test_fuguewr.c bsp/classpad50/SRC/INC/classpad_fuguewr.h bsp/classpad50/SRC/INC/classpad_fugue.h bsp/classpad50/SRC/INC/classpad_fuguetrim.h
	$(CC) $(CFLAGS) -o $@ $<

bsp/tests/test_extrom: bsp/tests/test_extrom.c bsp/classpad50/SRC/INC/classpad_extrom.h
	$(CC) $(CFLAGS) -o $@ $<

# the extrom guest's two-file region (the full Xpander one needs the app's files)
bsp/tests/extrom-test.bin: dbt/guest/extrom.txt dbt/guest/extrom-data.txt tools/mkextrom.py
	python3 tools/mkextrom.py $< $@
