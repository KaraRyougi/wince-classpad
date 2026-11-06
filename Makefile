CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c99
TEST    := fpuemu/tests/test_host
SRC     := fpuemu/src/softfloat.c fpuemu/src/fpuemu.c
HDR     := fpuemu/src/softfloat.h fpuemu/src/fpuemu.h
DBT_SRC  := dbt/src/sh4dec.c dbt/src/sh4emit.c dbt/src/translate.c $(SRC)
DBT_TEST := dbt/tests/test_translate

.PHONY: test clean

test: $(TEST) $(DBT_TEST) bsp/tests/test_touch_bus bsp/tests/test_timer
	./$(TEST)
	./$(DBT_TEST)
	./bsp/tests/test_touch_bus
	./bsp/tests/test_timer

$(DBT_TEST): dbt/tests/test_translate.c $(DBT_SRC) dbt/src/sh4dec.h dbt/src/sh4emit.h dbt/src/translate.h $(HDR)
	$(CC) $(CFLAGS) -Idbt/src -Ifpuemu/src -o $@ dbt/tests/test_translate.c $(DBT_SRC)

$(TEST): fpuemu/tests/test_host.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -Ifpuemu/src -o $@ fpuemu/tests/test_host.c $(SRC) -lm

clean:
	rm -f $(TEST) $(DBT_TEST) bsp/tests/test_touch_bus bsp/tests/test_timer

bsp/tests/test_touch_bus: bsp/tests/test_touch_bus.c bsp/classpad/SRC/INC/classpad_touch_bus.h bsp/classpad/SRC/INC/classpad_touch.h
	$(CC) $(CFLAGS) -o $@ $<

bsp/tests/test_timer: bsp/tests/test_timer.c bsp/classpad/SRC/OAL/OALLIB/timer.c bsp/classpad/SRC/INC/bsp_base_regs.h
	$(CC) $(CFLAGS) -Wno-unused-parameter -o $@ $<
