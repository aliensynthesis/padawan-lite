#
# Copyright 2026 Alien Synthesis
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
#   1. Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#
#   2. Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in
#      the documentation and/or other materials provided with the
#      distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER "AS IS" AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
# BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
# OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
# EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

# Padawan-Lite v1.2 - Linux build (GNU make).
# Per CLAUDE.md: strict C89 with pedantic warnings, no compiler extensions.

CC      ?= gcc
CFLAGS   = -std=c89 -Wall -Wpedantic -Wextra -Werror -Iinclude
LDFLAGS  =

# Tests need access to the platform stub's test-helper header
# (x25_stub.h); core library + bridge do not, so we keep that include
# path out of the base CFLAGS for cleaner separation.
TEST_CFLAGS = $(CFLAGS) -Iplatform/linux

LIB_SRC      = src/x3.c src/x28_signals.c src/x29_messages.c src/personality.c src/pad.c
LIB_OBJ      = $(LIB_SRC:.c=.o)

# Platform stub - used only by the test binaries. The padawan-lite binary
# itself uses the bridge's TCP-backed X.25 implementation, not the stub.
# libpadawancore.a explicitly excludes the stub so external consumers can
# provide their own x25.h implementation.
PLATFORM_SRC = platform/linux/x25_stub.c
PLATFORM_OBJ = $(PLATFORM_SRC:.c=.o)

TEST_SRC = $(wildcard tests/test_*.c)
TEST_BIN = $(TEST_SRC:.c=)

LIB_NAME = libpadawancore.a

# Telnet bridge - implements include/x25.h via TCP. The padawan-lite binary
# couples the PAD library with the bridge. Bridge files include only
# Padawan-Lite's PUBLIC headers, so the bridge directory can be extracted to
# a separate project later (see memory/project_bridge_target.md).
BRIDGE_DIR        = bridge
BRIDGE_LIB_SRC    = $(BRIDGE_DIR)/x25_telnet_bridge.c \
                    $(BRIDGE_DIR)/user_telnet.c \
                    $(BRIDGE_DIR)/pcp.c
BRIDGE_LIB_OBJ    = $(BRIDGE_LIB_SRC:.c=.o)
BRIDGE_MAIN_SRC   = $(BRIDGE_DIR)/main.c
BRIDGE_CFLAGS     = $(CFLAGS) -I$(BRIDGE_DIR)

PADAWAN_BIN = padawan-lite

.PHONY: all test clean lib

all: $(PADAWAN_BIN) $(TEST_BIN) $(LIB_NAME)

lib: $(LIB_NAME)

$(LIB_NAME): $(LIB_OBJ)
	$(AR) rcs $@ $(LIB_OBJ)

# padawan-lite = PAD library + telnet bridge + interactive driver.
# Links libpadawancore.a as if it were an external consumer of the library
# so the bridge directory remains extraction-ready.
$(PADAWAN_BIN): $(BRIDGE_MAIN_SRC) $(BRIDGE_LIB_OBJ) $(LIB_NAME)
	$(CC) $(BRIDGE_CFLAGS) -o $@ $(BRIDGE_MAIN_SRC) $(BRIDGE_LIB_OBJ) $(LIB_NAME)

$(BRIDGE_DIR)/%.o: $(BRIDGE_DIR)/%.c
	$(CC) $(BRIDGE_CFLAGS) -c -o $@ $<

test: $(TEST_BIN)
	@status=0; \
	for t in $(TEST_BIN); do \
		printf '==> %s\n' "$$t"; \
		./$$t || status=1; \
	done; \
	exit $$status

# Bridge-side tests need access to bridge headers + the corresponding
# bridge .o(s). More specific than the generic tests/test_% rule below,
# so make picks this one for the named targets.
tests/test_user_telnet: tests/test_user_telnet.c $(BRIDGE_DIR)/user_telnet.o
	$(CC) $(BRIDGE_CFLAGS) -o $@ $< $(BRIDGE_DIR)/user_telnet.o

tests/test_%: tests/test_%.c $(LIB_OBJ) $(PLATFORM_OBJ)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(LIB_OBJ) $(PLATFORM_OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(LIB_OBJ) $(PLATFORM_OBJ) $(BRIDGE_LIB_OBJ) \
	      $(TEST_BIN) $(PADAWAN_BIN) $(LIB_NAME)
