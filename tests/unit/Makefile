# ═══════════════════════════════════════════════════════════════════════════════
# Helios Unit Tests — PMM
# ═══════════════════════════════════════════════════════════════════════════════
# Compiles and runs PMM buddy allocator tests on the HOST (Linux x86-64).
# Uses system gcc, NOT the cross-compiler.
# ═══════════════════════════════════════════════════════════════════════════════

ROOT_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))/../..
INCLUDE_DIR := $(ROOT_DIR)/src/include

CC_HOST := gcc
CFLAGS_HOST := -std=c2x -Wall -Wextra -Werror -g -O0 \
               -DUNIT_TEST \
               -I$(INCLUDE_DIR)

BUILD_DIR := $(ROOT_DIR)/build/tests/unit

SRCS := test_pmm.c \
        $(ROOT_DIR)/src/kernel/mm/pmm.c

TEST_BIN := $(BUILD_DIR)/test_pmm

.PHONY: all run clean

all: $(TEST_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TEST_BIN): $(SRCS) | $(BUILD_DIR)
	$(CC_HOST) $(CFLAGS_HOST) -o $@ $(SRCS)

run: $(TEST_BIN)
	@echo ""
	@echo "══════════════════════════════════════════════════════"
	@echo "  Running PMM unit tests..."
	@echo "══════════════════════════════════════════════════════"
	@$(TEST_BIN)
	@echo ""
	@echo "══════════════════════════════════════════════════════"
	@echo "  ALL PMM TESTS PASSED"
	@echo "══════════════════════════════════════════════════════"
	@echo ""

clean:
	rm -rf $(BUILD_DIR)
