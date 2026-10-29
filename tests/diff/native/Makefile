# Built into $(OUT) (default: build/diff/native under the repo root).
OUT ?= ../../../build/diff/native
CC ?= cc
$(OUT)/runner: runner.c capture.asm | $(OUT)
	nasm -f elf64 capture.asm -o $(OUT)/capture.o
	$(CC) -O2 -Wall -Wextra -o $@ runner.c $(OUT)/capture.o
$(OUT):
	mkdir -p $@
