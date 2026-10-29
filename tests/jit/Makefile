OUT ?= ../../build/jit
CC = clang
CFLAGS = -std=c11 -O1 -g -Wall -Wextra -Werror -pthread
SRC = ../../runtime/jit/jit.c ../../runtime/jit/jit_linux.c test_jit.c
$(OUT)/test_jit: $(SRC) ../../runtime/jit/jit.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRC)
$(OUT)/test_fex_hooks: ../../runtime/jit/jit.c ../../runtime/jit/jit_linux.c ../../runtime/jit/jit_fex_hooks.c test_fex_hooks.c ../../runtime/jit/jit.h ../../runtime/jit/jit_fex_hooks.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ ../../runtime/jit/jit.c ../../runtime/jit/jit_linux.c ../../runtime/jit/jit_fex_hooks.c test_fex_hooks.c
test: $(OUT)/test_jit $(OUT)/test_fex_hooks
	$(OUT)/test_jit
	$(OUT)/test_fex_hooks
$(OUT):
	mkdir -p $@
.PHONY: test
