/*
 * fcntl.h for charsiu on Windows: mingw's, plus O_CLOEXEC. (Every file is
 * opened in binary mode: compat_win.c sets _fmode before main, since text
 * mode would turn CR LF into LF inside a model file.)
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CHARSIU_COMPAT_FCNTL_H
#define CHARSIU_COMPAT_FCNTL_H

#include_next <fcntl.h>
#include <io.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif

/*
 * M7: open() goes through compat_win.c, which passes every path to the CRT
 * except /dev/cpu_dma_latency. That one is charsiu's hold on the CPUs while
 * the NPU is open (npudev.c qos_hold); here it is a handle on NUL that takes
 * the Windows counterpart of the hold (compat_win.c) for the rest of the
 * process. CHARSIU_NPU_IDLE=1 keeps charsiu from asking, as on Linux.
 */
int charsiu_compat_open(const char *path, int flags, ...);
#define open(...) charsiu_compat_open(__VA_ARGS__)

#endif
