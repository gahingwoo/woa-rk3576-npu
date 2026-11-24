/*
 * sched.h for charsiu on Windows: mingw's, plus the Linux CPU affinity calls
 * llama.c uses to keep its threads on the big cores. Up to 64 CPUs, one
 * Windows processor group. compat_win.c has the code.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CHARSIU_COMPAT_SCHED_H
#define CHARSIU_COMPAT_SCHED_H

#include_next <sched.h>
#include <stddef.h>
#include <string.h>

#define CPU_SETSIZE 64
typedef struct { unsigned long long bits; } cpu_set_t;

#define CPU_ZERO(s)      ((s)->bits = 0)
#define CPU_SET(c, s)    ((c) < CPU_SETSIZE ? (void)((s)->bits |= 1ull << (c)) : (void)0)
#define CPU_CLR(c, s)    ((c) < CPU_SETSIZE ? (void)((s)->bits &= ~(1ull << (c))) : (void)0)
#define CPU_ISSET(c, s)  ((c) < CPU_SETSIZE ? (int)(((s)->bits >> (c)) & 1) : 0)
#define CPU_COUNT(s)     __builtin_popcountll((s)->bits)
#define CPU_EQUAL(a, b)  ((a)->bits == (b)->bits)

/* pid 0 only, as charsiu calls them; on Linux that is the calling thread */
int sched_setaffinity(int pid, size_t size, const cpu_set_t *set);
int sched_getaffinity(int pid, size_t size, cpu_set_t *set);
int sched_getcpu(void);

#endif
