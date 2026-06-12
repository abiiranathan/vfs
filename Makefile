# ==============================================================================
# Virtual Filesystem (VFS) Makefile
# Compatible with Linux, macOS, BSD, and Unix-like environments.
# ==============================================================================

# Installation base directories (conforming to GNU standards)
PREFIX      ?= /usr/local
EXEC_PREFIX ?= $(PREFIX)
BINDIR      ?= $(EXEC_PREFIX)/bin
LIBDIR      ?= $(EXEC_PREFIX)/lib
INCLUDEDIR  ?= $(PREFIX)/include

# Toolchain configuration
CC          ?= gcc
AR          ?= ar
RANLIB      ?= ranlib
INSTALL     ?= install
MKDIR       ?= mkdir -p
RM          ?= rm -f

# User compilation and linking flags
CFLAGS      ?= -Wall -Wextra -O3 -g -std=c11 -D_GNU_SOURCE -march=native
CPPFLAGS    ?=
LDFLAGS     ?=
LDLIBS      ?=

COMPILER_SUPPORTS_AVX2 := $(shell $(CC) -mavx2 -E - < /dev/null >/dev/null 2>&1 && echo yes || echo no)
ifeq ($(COMPILER_SUPPORTS_AVX2),yes)
    CFLAGS += -mavx2
endif

# Strict overrides to guarantee thread compatibility
override CFLAGS   += -pthread
override LDLIBS   += -lpthread

# Output build targets
LIB_NAME    := libvfs.a
TEST_BIN    := vfs_test

# Source mapping
VFS_SRC     := vfs.c
VFS_OBJ     := vfs.o
TEST_SRC    := vfs_test.c
TEST_OBJ    := vfs_test.o

.PHONY: all test clean install uninstall

# -------------------------------------------------------------------------
# Build Targets
# -------------------------------------------------------------------------

# Default target: compile static library and test binary
all: $(LIB_NAME) $(TEST_BIN)

# Compile system interface object
$(VFS_OBJ): $(VFS_SRC) vfs.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Package static archive
$(LIB_NAME): $(VFS_OBJ)
	$(AR) rcs $@ $^
	$(RANLIB) $@

# Compile test object
$(TEST_OBJ): $(TEST_SRC) vfs.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Build test executable linked against the static archive
$(TEST_BIN): $(TEST_OBJ) $(LIB_NAME)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Run the test suite
test: $(TEST_BIN)
	./$(TEST_BIN)

# -------------------------------------------------------------------------
# Cleanup and Maintenance
# -------------------------------------------------------------------------

clean:
	$(RM) $(VFS_OBJ) $(TEST_OBJ) $(LIB_NAME) $(TEST_BIN)
	$(RM) test_system.vfs

# -------------------------------------------------------------------------
# Installation targets (Supports staging via DESTDIR)
# -------------------------------------------------------------------------

install: all
	$(MKDIR) $(DESTDIR)$(LIBDIR)
	$(MKDIR) $(DESTDIR)$(INCLUDEDIR)
	$(INSTALL) -m 644 $(LIB_NAME) $(DESTDIR)$(LIBDIR)/$(LIB_NAME)
	$(INSTALL) -m 644 vfs.h $(DESTDIR)$(INCLUDEDIR)/vfs.h

uninstall:
	$(RM) $(DESTDIR)$(LIBDIR)/$(LIB_NAME)
	$(RM) $(DESTDIR)$(INCLUDEDIR)/vfs.h