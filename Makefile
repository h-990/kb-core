# KB Core — Makefile (Phases 1–3)
#
# Targets:
#   make          — build library and tests
#   make test     — build and run all tests
#   make clean    — remove build artifacts

CC       ?= cc
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror \
            -Wconversion -Wshadow -Wstrict-prototypes \
            -Wdouble-promotion -Wformat=2 \
            -fstack-protector-strong -g
INCLUDES := -Isrc -Itests

# Use Xcode SDK on macOS if available (workaround for CLT linker issues).
XCODE_SDK := $(shell ls -d /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk 2>/dev/null)
ifneq ($(XCODE_SDK),)
    CFLAGS += -isysroot $(XCODE_SDK)
endif

BUILD    := build
SRCDIR   := src/core
TESTDIR  := tests

# ── Sources ────────────────────────────────────────────────────────
CORE_SRC := $(SRCDIR)/kb_path.c \
            $(SRCDIR)/kb_crc32c.c \
            $(SRCDIR)/kb_os_posix.c \
            $(SRCDIR)/kb_superblock.c \
            $(SRCDIR)/kb_segment.c \
            $(SRCDIR)/kb_pool.c \
            $(SRCDIR)/kb_slab.c \
            $(SRCDIR)/kb_header.c \
            $(SRCDIR)/kb_block.c \
            $(SRCDIR)/kb_child.c \
            $(SRCDIR)/kb_extent.c \
            $(SRCDIR)/kb_wal.c \
            $(SRCDIR)/kb_index.c \
            $(SRCDIR)/kb_buffer.c \
            $(SRCDIR)/kb_store.c \
            $(SRCDIR)/kb_verify.c
CORE_OBJ := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(CORE_SRC))

# ── Library ────────────────────────────────────────────────────────
LIB      := $(BUILD)/libkb_core.a

# ── Test executables ───────────────────────────────────────────────
TEST_PATH       := $(BUILD)/test_path
TEST_CRC32C     := $(BUILD)/test_crc32c
TEST_SUPERBLOCK := $(BUILD)/test_superblock
TEST_POOL       := $(BUILD)/test_pool
TEST_SLAB       := $(BUILD)/test_slab
TEST_HEADER     := $(BUILD)/test_header
TEST_CHILD      := $(BUILD)/test_child
TEST_EXTENT     := $(BUILD)/test_extent
TEST_WAL        := $(BUILD)/test_wal
TEST_INDEX      := $(BUILD)/test_index
TEST_BUFFER     := $(BUILD)/test_buffer
TEST_STORE      := $(BUILD)/test_store
TEST_VERIFY     := $(BUILD)/test_verify

TESTS := $(TEST_PATH) $(TEST_CRC32C) $(TEST_SUPERBLOCK) $(TEST_POOL) $(TEST_SLAB) $(TEST_HEADER) $(TEST_CHILD) $(TEST_EXTENT) $(TEST_WAL) $(TEST_INDEX) $(TEST_BUFFER) $(TEST_STORE) $(TEST_VERIFY)

.PHONY: all test clean

all: $(LIB) $(TESTS)

# ── Build rules ────────────────────────────────────────────────────
$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(LIB): $(CORE_OBJ)
	ar rcs $@ $^

$(TEST_PATH): $(TESTDIR)/test_path.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(TEST_CRC32C): $(TESTDIR)/test_crc32c.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(TEST_SUPERBLOCK): $(TESTDIR)/test_superblock.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(TEST_POOL): $(TESTDIR)/test_pool.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(TEST_SLAB): $(TESTDIR)/test_slab.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(TEST_HEADER): $(TESTDIR)/test_header.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(TEST_CHILD): $(TESTDIR)/test_child.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(TEST_EXTENT): $(TESTDIR)/test_extent.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(TEST_WAL): $(TESTDIR)/test_wal.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(TEST_INDEX): $(TESTDIR)/test_index.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(TEST_BUFFER): $(TESTDIR)/test_buffer.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(TEST_STORE): $(TESTDIR)/test_store.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(TEST_VERIFY): $(TESTDIR)/test_verify.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(BUILD):
	mkdir -p $(BUILD)

# ── Test ───────────────────────────────────────────────────────────
test: all
	@echo "──── Path codec ────"
	@$(TEST_PATH)
	@echo "──── CRC32C ────"
	@$(TEST_CRC32C)
	@echo "──── Superblock ────"
	@$(TEST_SUPERBLOCK)
	@echo "──── Pool ────"
	@$(TEST_POOL)
	@echo "──── Slab ────"
	@$(TEST_SLAB)
	@echo "──── Header ────"
	@$(TEST_HEADER)
	@echo "──── Child ────"
	@$(TEST_CHILD)
	@echo "──── Extent ────"
	@$(TEST_EXTENT)
	@echo "──── WAL ────"
	@$(TEST_WAL)
	@echo "──── Index ────"
	@$(TEST_INDEX)
	@echo "──── Buffer ────"
	@$(TEST_BUFFER)
	@echo "──── Store ────"
	@$(TEST_STORE)
	@echo "──── Verify ────"
	@$(TEST_VERIFY)

# ── Tools ──────────────────────────────────────────────────────────
TOOLDIR  := tools
INSPECT  := $(BUILD)/kb_inspect
SERVER   := $(BUILD)/kb_server
GUI_HTML := $(TOOLDIR)/web/index.html
GUI_C    := $(BUILD)/kb_gui_html.c

$(INSPECT): $(TOOLDIR)/kb_inspect.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD) -lkb_core -o $@

$(GUI_C): $(GUI_HTML) $(TOOLDIR)/embed_html.sh | $(BUILD)
	$(TOOLDIR)/embed_html.sh $(GUI_HTML) $(GUI_C)

$(SERVER): $(TOOLDIR)/kb_server.c $(GUI_C) $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(TOOLDIR)/kb_server.c $(GUI_C) -L$(BUILD) -lkb_core -o $@

tools: $(INSPECT) $(SERVER)

gui: $(SERVER)
	@echo "Starting KB Dev GUI at http://localhost:9090"
	@$(SERVER)

# ── Clean ──────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD)
