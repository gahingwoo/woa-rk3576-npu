/*
 * stdio.h for charsiu on Windows: the CRT's, with fopen through compat_win.c
 * (M7). charsiu finds the fast CPU cluster by reading
 * /sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_max_freq (llama.c,
 * cpus_fastest_cluster) and pins the decode thread to it; Windows has no
 * sysfs, so that read failed and the thread went wherever the scheduler put
 * it, an A53 as often as an A72. compat_win.c answers those few files from
 * what Windows knows about each processor and passes every other path to
 * the CRT. CHARSIU_WIN_SYSFS=0 turns it off (M6's behaviour).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CHARSIU_COMPAT_STDIO_H
#define CHARSIU_COMPAT_STDIO_H

#include_next <stdio.h>

FILE *charsiu_compat_fopen(const char *path, const char *mode);
#ifndef CHARSIU_COMPAT_NO_FOPEN
#define fopen(p, m) charsiu_compat_fopen(p, m)
#endif

#endif
