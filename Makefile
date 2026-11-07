CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude
DEBUG_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -O0 -fsanitize=address,undefined -Iinclude
LDFLAGS =
NCURSES_LIBS = $(shell pkg-config --libs ncurses 2>/dev/null || echo "-lncurses")

BUILD_DIR = build

# Simulator core: libc + libm only, no UI dependency.
ENGINE_SRCS = src/engine/sim.c src/engine/workload.c src/engine/sim_api.c src/engine/scenario.c
SCHED_SRCS  = src/sched/schedulers.c
VM_SRCS     = src/vm/replacement.c
LIB_SRCS    = $(ENGINE_SRCS) $(SCHED_SRCS) $(VM_SRCS)
LIB_OBJS    = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(LIB_SRCS))

CLI_SRCS = src/cli/main.c
TUI_SRCS = src/ui/tui.c

STATIC_LIB = $(BUILD_DIR)/libprism.a
CLI_BIN = prism-cli
TUI_BIN = prism-tui

# Every tests/test_*.c becomes its own binary; `make test` runs them all.
TEST_SRCS = $(wildcard tests/test_*.c)
TEST_BINS = $(patsubst tests/%.c, $(BUILD_DIR)/%, $(TEST_SRCS))

.PHONY: all lib cli tui clean test debug sanitize valgrind experiments help

all: $(STATIC_LIB) $(CLI_BIN) $(TUI_BIN)
lib: $(STATIC_LIB)
cli: $(CLI_BIN)
tui: $(TUI_BIN)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)/engine $(BUILD_DIR)/sched $(BUILD_DIR)/vm

# -MMD -MP: rebuild objects when a header changes (sim_t layout changes
# otherwise leave stale objects that crash at runtime).
$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)

$(CLI_BIN): $(CLI_SRCS) $(STATIC_LIB)
	$(CC) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -lm -o $@

$(TUI_BIN): $(TUI_SRCS) $(STATIC_LIB)
	$(CC) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) $(NCURSES_LIBS) -lm -o $@

$(BUILD_DIR)/test_%: tests/test_%.c tests/test_harness.h $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -lm -o $@

test: $(TEST_BINS)
	@echo "Running PRISM Automated Test Suites..."
	@set -e; for t in $(TEST_BINS); do ./$$t; done

debug sanitize: clean
	@echo "Building with Address and UndefinedBehavior Sanitizers..."
	@$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" LDFLAGS="-fsanitize=address,undefined" all $(TEST_BINS)
	@echo "Running tests under sanitizers..."
	@set -e; for t in $(TEST_BINS); do ASAN_OPTIONS=detect_leaks=1 ./$$t; done

valgrind: $(TEST_BINS)
	@echo "Running Valgrind Memory Leak and Cleanliness Check..."
	@set -e; for t in $(TEST_BINS); do valgrind --leak-check=full --error-exitcode=1 ./$$t; done

experiments: $(CLI_BIN)
	@echo "Executing PRISM Course Project Experiments..."
	@./$(CLI_BIN) --all-exp

clean:
	rm -rf $(BUILD_DIR) $(CLI_BIN) $(TUI_BIN)

help:
	@echo "Available targets:"
	@echo "  make              - Build libprism.a, prism-cli, and prism-tui"
	@echo "  make lib|cli|tui  - Build one piece"
	@echo "  make test         - Build and run every tests/test_*.c"
	@echo "  make debug        - Clean build + tests with ASan & UBSan (alias: sanitize)"
	@echo "  make valgrind     - Run tests under valgrind"
	@echo "  make experiments  - Run all three core experiment benchmarks"
	@echo "  make clean        - Remove build artifacts"

-include $(LIB_OBJS:.o=.d)
