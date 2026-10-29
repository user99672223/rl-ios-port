OUT ?= ../../build/jit
CC = clang
CFLAGS = -std=c11 -O1 -g -Wall -Wextra -Werror -pthread
SRC = ../../runtime/jit/jit.c ../../runtime/jit/jit_linux.c test_jit.c
$(OUT)/test_jit: $(SRC) ../../runtime/jit/jit.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRC)
test: $(OUT)/test_jit
	$(OUT)/test_jit
$(OUT):
	mkdir -p $@
.PHONY: test
