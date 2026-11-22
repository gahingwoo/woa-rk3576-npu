# tools/m3gen: Linux host, gcc. Needs the charsiu submodule (git submodule
# update --init external/charsiu).
#
#   make             build/m3gen-header and build/m3gen
#   make header      regenerate tools/rknputest/m3_streams.h
#   make regs        regenerate include/rknpu_regcmd_allow.h (M4)
#   make check       compare rknputest's int8 matmul with charsiu's (CI)
#
# SPDX-License-Identifier: GPL-2.0-only

TOP     := ../..
CHARSIU := $(TOP)/external/charsiu
B       := build
COMMIT  := $(shell git -C $(CHARSIU) rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
CPPFLAGS := -I$(CHARSIU)/include -I$(CHARSIU)/src -I$(TOP)/include -I$(TOP)/tools/rknputest \
            -DCHARSIU_COMMIT=\"$(COMMIT)\" -DCHARSIU_BUILD=\"m3gen\"
SRC     := m3gen.c $(CHARSIU)/src/job.c $(CHARSIU)/src/regcmd.c

all: $(B)/m3gen-header $(B)/m3gen

$(B)/m3gen-header: $(SRC) $(TOP)/tools/rknputest/m3_gemm.h $(TOP)/include/rknpu_regcmd.h
	mkdir -p $(B)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DM3_NO_STREAMS -DRKNPU_RC_NO_ALLOWLIST -o $@ $(SRC) -lm

$(B)/m3gen: $(SRC) $(TOP)/tools/rknputest/m3_gemm.h $(TOP)/tools/rknputest/m3_streams.h $(TOP)/include/rknpu_regcmd.h \
           $(TOP)/include/rknpu_regcmd_allow.h
	mkdir -p $(B)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRC) -lm

header: $(B)/m3gen-header
	$(B)/m3gen-header header > $(TOP)/tools/rknputest/m3_streams.h.tmp
	mv $(TOP)/tools/rknputest/m3_streams.h.tmp $(TOP)/tools/rknputest/m3_streams.h

regs: $(B)/m3gen-header
	$(B)/m3gen-header regs > $(TOP)/include/rknpu_regcmd_allow.h.tmp
	mv $(TOP)/include/rknpu_regcmd_allow.h.tmp $(TOP)/include/rknpu_regcmd_allow.h

check: $(B)/m3gen
	$(B)/m3gen check

clean:
	rm -rf $(B)

.PHONY: all header regs check clean
