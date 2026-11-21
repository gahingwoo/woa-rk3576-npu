# SPDX-License-Identifier: GPL-2.0-only
#
# make CHARSIU=/path/to/charsiu
#
# Builds static aarch64 binaries (the board has no compiler): natively on an
# aarch64 host, or with CROSS=aarch64-linux-gnu- elsewhere. m0_bypass links
# charsiu's own emitter (src/regcmd.c, src/job.c), so the register stream is
# the one charsiu submits through rocket.

CHARSIU ?= ../../../charsiu
CROSS   ?=
CC      := $(CROSS)gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11

all: m0_peek m0_bypass

m0_peek: m0_peek.c
	$(CC) $(CFLAGS) -static -o $@ $<

m0_bypass: m0_bypass.c $(CHARSIU)/src/regcmd.c $(CHARSIU)/src/job.c
	@test -f $(CHARSIU)/include/charsiu.h || { echo "set CHARSIU= to a charsiu checkout"; exit 1; }
	$(CC) $(CFLAGS) -I$(CHARSIU)/include -static -o $@ $^ -lm

clean:
	rm -f m0_peek m0_bypass

.PHONY: all clean
