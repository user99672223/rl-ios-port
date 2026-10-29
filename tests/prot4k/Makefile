# prot4k tests.  Everything builds with: clang -std=c11 -Wall -Wextra -Werror
#
#   make            build + run all tests (unit, randomized model, wine-mirror sync)
#   make negative   build mutated copies of the library and require that the tests FAIL on each
#   make clean
CC      = clang
LIBDIR  := ../../runtime/prot4k
BUILD   := build
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O1 -g -I$(LIBDIR) -I.
# the mutation harness points LIBDIR_OVERRIDE at a mutated copy
ifdef LIBDIR_OVERRIDE
LIBDIR  := $(LIBDIR_OVERRIDE)
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O1 -g -I$(LIBDIR) -I.
BUILD   := $(BUILD_OVERRIDE)
endif

LIBSRC  := $(LIBDIR)/prot4k.c $(LIBDIR)/prot4k_space.c
HDRS    := $(wildcard $(LIBDIR)/*.h) check.h mock_host.h

.PHONY: all test negative clean sync
all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_unit: test_unit.c mock_host.c $(LIBSRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_unit.c mock_host.c $(LIBSRC)

$(BUILD)/test_random: test_random.c mock_host.c $(LIBSRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_random.c mock_host.c $(LIBSRC)

test: $(BUILD)/test_unit $(BUILD)/test_random sync
	$(BUILD)/test_unit
	$(BUILD)/test_random

# the copy of the library embedded in the Wine patch must be byte-identical
sync:
	./check_wine_mirror.sh

negative:
	./negative_check.sh

clean:
	rm -rf build
