# ==============================================================================
# Virtual Filesystem (VFS) Makefile
# Compatible with Linux, macOS, BSD, and Unix-like environments.
# ==============================================================================

# Installation base directories (conforming to GNU standards)
PREFIX          ?= /usr/local
EXEC_PREFIX    ?= $(PREFIX)
BINDIR          ?= $(EXEC_PREFIX)/bin
LIBDIR          ?= $(EXEC_PREFIX)/lib
INCLUDEDIR      ?= $(PREFIX)/include

# Local build directory layout
BUILD_OBJ_DIR   := obj
BUILD_LIB_DIR   := lib
BUILD_BIN_DIR   := bin

# Toolchain configuration
CC              ?= gcc
AR              ?= ar
RANLIB          ?= ranlib
INSTALL         ?= install
MKDIR           ?= mkdir -p
RM              ?= rm -f
RM_DIR          ?= rm -rf

# User compilation and linking flags
CFLAGS          ?= -Wall -Wextra -O3 -g -std=c11 -D_GNU_SOURCE -march=native
CPPFLAGS        ?=
LDFLAGS         ?=
LDLIBS          ?=

COMPILER_SUPPORTS_AVX2 := $(shell $(CC) -mavx2 -E - < /dev/null >/dev/null 2>&1 && echo yes || echo no)
ifeq ($(COMPILER_SUPPORTS_AVX2),yes)
    CFLAGS += -mavx2
endif

# Strict overrides to guarantee thread compatibility
override CFLAGS   += -pthread
override LDLIBS   += -lpthread

# Output build targets
LIB_NAME        := $(BUILD_LIB_DIR)/libvfs.a
TEST_BIN        := $(BUILD_BIN_DIR)/vfs_test
CLI_BIN         := $(BUILD_BIN_DIR)/vfs-cli
PACK_BIN        := $(BUILD_BIN_DIR)/vfs-pack

# Source mapping
VFS_SRC         := vfs.c
VFS_OBJ         := $(BUILD_OBJ_DIR)/vfs.o
TEST_SRC        := vfs_test.c
TEST_OBJ        := $(BUILD_OBJ_DIR)/vfs_test.o
CLI_SRC         := vfs_cli.c
CLI_OBJ         := $(BUILD_OBJ_DIR)/vfs_cli.o
PACK_SRC        := vfs-pack.c
PACK_OBJ        := $(BUILD_OBJ_DIR)/vfs_pack.o

# Optional solidc flags for vfs-pack
PKG_CONFIG      ?= pkg-config
SOLIDC_CFLAGS   := $(shell command -v $(PKG_CONFIG) >/dev/null 2>&1 && $(PKG_CONFIG) --exists solidc 2>/dev/null && $(PKG_CONFIG) --cflags solidc 2>/dev/null)
SOLIDC_LIBS     := $(shell command -v $(PKG_CONFIG) >/dev/null 2>&1 && $(PKG_CONFIG) --exists solidc 2>/dev/null && $(PKG_CONFIG) --libs solidc 2>/dev/null)
SOLIDC_AVAILABLE := $(if $(strip $(SOLIDC_LIBS)),yes,no)

ifneq ($(SOLIDC_AVAILABLE),yes)
$(info Warning: solidc was not found via pkg-config; vfs-pack will be skipped by default)
endif

PACK_EXTRA_CPPFLAGS := $(SOLIDC_CFLAGS)
PACK_EXTRA_LDLIBS   := $(SOLIDC_LIBS)
PACK_TARGETS        := $(if $(SOLIDC_AVAILABLE),$(PACK_BIN),)

.PHONY: all test clean install uninstall

# -------------------------------------------------------------------------
# Build Targets
# -------------------------------------------------------------------------

# Default target: compile static library and all binaries
all: $(LIB_NAME) $(TEST_BIN) $(CLI_BIN) $(PACK_TARGETS)

$(BUILD_OBJ_DIR):
	$(MKDIR) $@

$(BUILD_LIB_DIR):
	$(MKDIR) $@

$(BUILD_BIN_DIR):
	$(MKDIR) $@

# Compile objects
$(VFS_OBJ): $(VFS_SRC) vfs.h | $(BUILD_OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(TEST_OBJ): $(TEST_SRC) vfs.h | $(BUILD_OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(CLI_OBJ): $(CLI_SRC) vfs.h | $(BUILD_OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(PACK_OBJ): $(PACK_SRC) vfs.h | $(BUILD_OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PACK_EXTRA_CPPFLAGS) -c $< -o $@

# Package static archive
$(LIB_NAME): $(VFS_OBJ) | $(BUILD_LIB_DIR)
	$(AR) rcs $@ $^
	$(RANLIB) $@

# Build executables linked against the static archive
$(TEST_BIN): $(TEST_OBJ) $(LIB_NAME) | $(BUILD_BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(CLI_BIN): $(CLI_OBJ) $(LIB_NAME) | $(BUILD_BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(PACK_BIN): $(PACK_OBJ) $(LIB_NAME) | $(BUILD_BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) $(PACK_EXTRA_LDLIBS) -o $@

# Run the test suite
test: $(TEST_BIN)
	./$(TEST_BIN)

# -------------------------------------------------------------------------
# Cleanup and Maintenance
# -------------------------------------------------------------------------

clean:
	$(RM) $(VFS_OBJ) $(TEST_OBJ) $(CLI_OBJ) $(PACK_OBJ) $(LIB_NAME) $(TEST_BIN) $(CLI_BIN) $(PACK_BIN)
	$(RM_DIR) $(BUILD_OBJ_DIR) $(BUILD_LIB_DIR)
	$(RM) test_system.vfs

# -------------------------------------------------------------------------
# Installation targets (Supports staging via DESTDIR)
# -------------------------------------------------------------------------

install: all
	$(MKDIR) $(DESTDIR)$(LIBDIR)
	$(MKDIR) $(DESTDIR)$(INCLUDEDIR)
	$(INSTALL) -m 644 $(LIB_NAME) $(DESTDIR)$(LIBDIR)/libvfs.a
	$(INSTALL) -m 644 vfs.h $(DESTDIR)$(INCLUDEDIR)/vfs.h

uninstall:
	$(RM) $(DESTDIR)$(LIBDIR)/libvfs.a
	$(RM) $(DESTDIR)$(INCLUDEDIR)/vfs.h
