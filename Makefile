CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c99
TEST    := fpuemu/tests/test_host
SRC     := fpuemu/src/softfloat.c fpuemu/src/fpuemu.c
HDR     := fpuemu/src/softfloat.h fpuemu/src/fpuemu.h
DBT_SRC  := dbt/src/sh4dec.c dbt/src/sh4emit.c dbt/src/translate.c $(SRC)
DBT_TEST := dbt/tests/test_translate

.PHONY: test clean

test: $(TEST) $(DBT_TEST)
	./$(TEST)
	./$(DBT_TEST)

$(DBT_TEST): dbt/tests/test_translate.c $(DBT_SRC) dbt/src/sh4dec.h dbt/src/sh4emit.h dbt/src/translate.h $(HDR)
	$(CC) $(CFLAGS) -Idbt/src -Ifpuemu/src -o $@ dbt/tests/test_translate.c $(DBT_SRC)

$(TEST): fpuemu/tests/test_host.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -Ifpuemu/src -o $@ fpuemu/tests/test_host.c $(SRC) -lm

clean:
	rm -f $(TEST) $(DBT_TEST)
