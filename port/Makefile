# port/: charsiu on Windows on Arm, over rknpu.sys (M4)
#
#   make win         build/win/*.exe for ARM64 Windows, with llvm-mingw
#                    (LLVM_MINGW=its directory, or its bin/ on PATH)
#   make host        build/host/: devtest and charsiu_run-fake, on Linux
#   make check       run devtest (CI)
#   make libm-check  the bundled expf/sinf/cosf/powf against this host's
#                    glibc, every float input; aarch64 Linux only (CI)
#   make linux-ref   build/linux/: charsiu_run and charsiu_matmul for the
#                    board's Linux, static, from the same charsiu commit and
#                    with charsiu's own device.c (rocket): the reference side
#                    of docs/M4.md; aarch64 Linux only (CI)
#   make clean
#
# charsiu itself (external/charsiu) is compiled unchanged. src/device.c is
# replaced by device_rknpu.c, and compat/ supplies the POSIX pieces mingw
# lacks. See docs/M4.md.
#
# SPDX-License-Identifier: GPL-2.0-only

TOP      := ..
CHARSIU  := $(TOP)/external/charsiu
B        := build
COMMIT   := $(shell git -C $(CHARSIU) describe --always --abbrev=12 2>/dev/null || echo unknown)
PORTREV  := $(shell git -C $(TOP) describe --always --dirty --abbrev=12 2>/dev/null || echo unknown)

LLVM_MINGW ?=
WCC      := $(if $(LLVM_MINGW),$(LLVM_MINGW)/bin/)aarch64-w64-mingw32-clang
WOBJDUMP := $(if $(LLVM_MINGW),$(LLVM_MINGW)/bin/)llvm-objdump

# The same arithmetic as charsiu's own Linux build (gcc -std=c11, which does
# not contract a*b+c into an fma): clang contracts by default, so say off.
# Same text from gcc and from clang, CPU path, checked on the host.
CFLAGS_C := -O2 -std=c11 -ffp-contract=off -Wall -Wextra \
            -Wno-void-pointer-to-int-cast -Wno-int-to-void-pointer-cast -Wno-uninitialized
CPPFLAGS_C := -I$(CHARSIU)/include -I$(CHARSIU)/third_party -I$(TOP)/include -I.

# glibc's float math, which is ARM optimized-routines' (port/libm, v25.01):
# built as glibc builds it, with fma contraction, it matches glibc 2.39 on
# every float input (make libm-check). UCRT's expf is not glibc's, and a
# last-bit difference in a softmax can change a greedy token.
CFLAGS_M := -O2 -std=c11 -ffp-contract=fast -fno-builtin -w
LIBM_SRC := libm/expf.c libm/exp2f_data.c libm/sinf.c libm/cosf.c libm/sincosf_data.c \
            libm/powf.c libm/powf_log2_data.c libm/math_errf.c

LLM := $(CHARSIU)/src/gguf.c $(CHARSIU)/src/tokenizer.c $(CHARSIU)/src/llama.c \
       $(CHARSIU)/src/npuquant.c $(CHARSIU)/src/npudev.c $(CHARSIU)/src/npupool.c \
       $(CHARSIU)/src/npufp16.c $(CHARSIU)/src/job.c $(CHARSIU)/src/regcmd.c
DEV_WIN  := device_rknpu.c xport_win.c compat/compat_win.c
DEV_FAKE := device_rknpu.c test/xport_fake.c

WIN_EXE := $(B)/win/charsiu_run.exe $(B)/win/charsiu_matmul.exe $(B)/win/npu_gemm_test.exe

all: win

# ---- Windows ---------------------------------------------------------------

LIBM_WOBJ := $(patsubst libm/%.c,$(B)/win/libm/%.o,$(LIBM_SRC))

$(B)/win/libm/%.o: libm/%.c | $(B)/win
	@mkdir -p $(B)/win/libm
	$(WCC) $(CFLAGS_M) -Ilibm -c $< -o $@

# 16 MiB of stack for every thread, as near Linux's 8 MiB default as matters
WLDFLAGS := -static -Wl,--stack,16777216 -lpthread -lpowrprof
WDEFS    = -Icompat $(CPPFLAGS_C) -DCHARSIU_BUILD=\"$(COMMIT)-win-$(PORTREV)\"

$(B)/win/charsiu_run.exe: $(CHARSIU)/tools/charsiu_run.c $(CHARSIU)/src/vision.c $(CHARSIU)/src/image.c \
                          $(LLM) $(DEV_WIN) $(LIBM_WOBJ) | $(B)/win
	$(WCC) $(CFLAGS_C) $(WDEFS) -o $@ $^ $(WLDFLAGS)

$(B)/win/charsiu_matmul.exe: $(CHARSIU)/tools/charsiu_matmul.c $(CHARSIU)/src/job.c $(CHARSIU)/src/regcmd.c \
                             $(DEV_WIN) $(LIBM_WOBJ) | $(B)/win
	$(WCC) $(CFLAGS_C) $(WDEFS) -o $@ $^ $(WLDFLAGS)

$(B)/win/npu_gemm_test.exe: $(CHARSIU)/tools/npu_gemm_test.c $(LLM) $(DEV_WIN) $(LIBM_WOBJ) | $(B)/win
	$(WCC) $(CFLAGS_C) $(WDEFS) -o $@ $^ $(WLDFLAGS)

# The math must come from libm.o, not from UCRT: no exe may import these.
win: $(WIN_EXE)
	@for e in $(WIN_EXE); do \
	    if $(WOBJDUMP) -p $$e | grep -E '^\s+[0-9]+\s+(expf|sinf|cosf|powf)$$'; then \
	        echo "$$e imports the CRT's float math"; exit 1; fi; \
	done
	@echo "win: $(WIN_EXE) (float math bundled)"

# ---- Linux host ------------------------------------------------------------

HCFLAGS := -O2 -std=c11 -Wall -Wextra -Werror $(CPPFLAGS_C) -DCHARSIU_BUILD=\"host-fake\"

$(B)/host/devtest: test/devtest.c $(DEV_FAKE) $(CHARSIU)/src/job.c $(CHARSIU)/src/regcmd.c \
                   $(TOP)/include/rknpu_ioctl.h $(TOP)/include/rknpu_regcmd.h \
                   $(TOP)/include/rknpu_regcmd_allow.h | $(B)/host
	$(CC) $(HCFLAGS) -o $@ test/devtest.c $(DEV_FAKE) $(CHARSIU)/src/job.c $(CHARSIU)/src/regcmd.c -lm -lpthread

# charsiu_run on the in-memory driver: run a real model through the device
# layer and the driver's checks (the text is garbage: nothing computes)
$(B)/host/charsiu_run-fake: $(CHARSIU)/tools/charsiu_run.c $(CHARSIU)/src/vision.c $(CHARSIU)/src/image.c \
                            $(LLM) $(DEV_FAKE) | $(B)/host
	$(CC) -O2 -std=c11 $(CPPFLAGS_C) -DCHARSIU_BUILD=\"host-fake\" -o $@ $^ -lm -lpthread

host: $(B)/host/devtest $(B)/host/charsiu_run-fake

check: $(B)/host/devtest
	$(B)/host/devtest

$(B)/host/libmcheck: test/libmcheck.c $(LIBM_SRC) | $(B)/host
	for f in $(LIBM_SRC); do \
	    $(CC) $(CFLAGS_M) -Ilibm -c $$f -o $(B)/host/$$(basename $$f .c).lo || exit 1; done
	ld -r -o $(B)/host/aor.lo $(addprefix $(B)/host/,$(notdir $(LIBM_SRC:.c=.lo)))
	objcopy --redefine-sym expf=aor_expf --redefine-sym sinf=aor_sinf --redefine-sym cosf=aor_cosf \
	        --redefine-sym powf=aor_powf $(B)/host/aor.lo $(B)/host/aor2.lo
	$(CC) -O2 -fno-builtin -o $@ test/libmcheck.c $(B)/host/aor2.lo -lm

libm-check: $(B)/host/libmcheck
	$(B)/host/libmcheck

# ---- the Linux reference ---------------------------------------------------

# charsiu's own Makefile flags (gcc -std=c11, so no fma contraction) and its
# own device.c; static, so the board's glibc is not a variable either
LCFLAGS := -O2 -std=c11 -Wall -Wextra -I$(CHARSIU)/include -I$(CHARSIU)/third_party \
           -DCHARSIU_BUILD=\"$(COMMIT)-linux-$(PORTREV)\"

$(B)/linux/charsiu_run: $(CHARSIU)/tools/charsiu_run.c $(CHARSIU)/src/vision.c $(CHARSIU)/src/image.c \
                        $(LLM) $(CHARSIU)/src/device.c | $(B)/linux
	$(CC) $(LCFLAGS) -static -o $@ $^ -lm -lpthread

$(B)/linux/charsiu_matmul: $(CHARSIU)/tools/charsiu_matmul.c $(CHARSIU)/src/job.c $(CHARSIU)/src/regcmd.c \
                           $(CHARSIU)/src/device.c | $(B)/linux
	$(CC) $(LCFLAGS) -static -o $@ $^ -lm

linux-ref: $(B)/linux/charsiu_run $(B)/linux/charsiu_matmul

$(B)/win $(B)/host $(B)/linux:
	mkdir -p $@

clean:
	rm -rf $(B)

.PHONY: all win host check libm-check linux-ref clean
