CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude
DEBUG_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -O0 -fsanitize=address,undefined -Iinclude
LDFLAGS = 
NCURSES_LIBS = $(shell pkg-config --libs ncurses 2>/dev/null || echo "-lncurses")

BUILD_DIR = build
BIN_DIR = bin

ENGINE_SRCS = src/engine/sim.c src/engine/workload.c
SCHED_SRCS  = src/sched/schedulers.c
VM_SRCS     = src/vm/replacement.c
LIB_SRCS    = $(ENGINE_SRCS) $(SCHED_SRCS) $(VM_SRCS)
LIB_OBJS    = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(LIB_SRCS))

CLI_SRCS = src/cli/main.c
TUI_SRCS = src/ui/tui.c
TEST_SRCS = tests/test_prism.c

STATIC_LIB = $(BUILD_DIR)/libprism.a
CLI_BIN = prism-cli
TUI_BIN = prism-tui
TEST_BIN = $(BUILD_DIR)/test_prism

.PHONY: all clean test debug sanitize valgrind help

all: $(STATIC_LIB) $(CLI_BIN) $(TUI_BIN)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)/engine $(BUILD_DIR)/sched $(BUILD_DIR)/vm $(BUILD_DIR)/cli $(BUILD_DIR)/ui

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)

$(CLI_BIN): $(CLI_SRCS) $(STATIC_LIB)
	$(CC) $(CFLAGS) $< $(STATIC_LIB) -o $@

$(TUI_BIN): $(TUI_SRCS) $(STATIC_LIB)
	$(CC) $(CFLAGS) $< $(STATIC_LIB) $(NCURSES_LIBS) -o $@

$(TEST_BIN): $(TEST_SRCS) $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(STATIC_LIB) -o $@

test: $(TEST_BIN)
	@echo "Running PRISM Automated Test Suite..."
	@./$(TEST_BIN)

debug sanitize: clean
	@echo "Building with Address and UndefinedBehavior Sanitizers..."
	@$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" LDFLAGS="-fsanitize=address,undefined" all $(TEST_BIN)
	@echo "Running tests under sanitizers..."
	@./$(TEST_BIN)

valgrind: $(TEST_BIN)
	@echo "Running Valgrind Memory Leak and Cleanliness Check..."
	valgrind --leak-check=full --error-exitcode=1 ./$(TEST_BIN)

experiments: $(CLI_BIN)
	@echo "Executing PRISM Course Project Experiments..."
	@./$(CLI_BIN) --all-exp

clean:
	rm -rf $(BUILD_DIR) $(CLI_BIN) $(TUI_BIN)

help:
	@echo "Available targets:"
	@echo "  make              - Build libprism.a, prism-cli, and prism-tui"
	@echo "  make test         - Run assert-based test suite"
	@echo "  make sanitize     - Build and run with ASan & UBSan"
	@echo "  make valgrind     - Run valgrind memory leak verification"
	@echo "  make experiments  - Run all three core experiment benchmarks"
	@echo "  make clean        - Remove build artifacts"
	@echo "  make tui 				 - Rebuild the TUI objected files"

