/*
 * perfcheck.c - host test of rknpu_perf.h: the tick conversion and the
 * structure sizes PCW copies (rknpu.man's counters read them by offset).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stddef.h>
#include "rknpu_perf.h"

static int fails;

static void eq(const char *what, unsigned long long got, unsigned long long want)
{
    if (got != want) {
        printf("FAIL %s: %llu, want %llu\n", what, got, want);
        fails++;
    }
}

int main(void)
{
    /* the QPC rates a Windows on Arm board reports: the arch timer at 24
     * MHz, and the 10 MHz Windows uses on some machines */
    const unsigned long long f24 = 24000000ull, f10 = 10000000ull;
    unsigned long long t, x;

    eq("0 ticks", rknpu_perf_scale(0, f24, 10000000ull), 0);
    eq("no freq", rknpu_perf_scale(123, 0, 10000000ull), 0);
    eq("1 s at 24 MHz in 100 ns", rknpu_perf_scale(f24, f24, 10000000ull), 10000000ull);
    eq("1 s at 24 MHz in us", rknpu_perf_scale(f24, f24, 1000000ull), 1000000ull);
    eq("24 ticks = 1 us", rknpu_perf_scale(24, f24, 1000000ull), 1);
    eq("10 MHz is 100 ns", rknpu_perf_scale(12345, f10, 10000000ull), 12345);
    /* 30 days of busy time: ticks * 10^7 is far past 2^64 here */
    t = 30ull * 86400ull * f24 + 12345ull;
    x = rknpu_perf_scale(t, f24, 10000000ull);
    eq("30 days in 100 ns", x, 30ull * 86400ull * 10000000ull + 12345ull * 10000000ull / f24);
    /* monotonic: one more tick never gives less */
    for (t = 0; t < 100000; t += 7)
        if (rknpu_perf_scale(t + 1, f24, 10000000ull) < rknpu_perf_scale(t, f24, 10000000ull)) {
            printf("FAIL not monotonic at %llu\n", t);
            fails++;
            break;
        }

    /* layouts: no padding the 64-bit fields would make on one compiler only */
    eq("RKNPU_PERF_CORE size", sizeof(RKNPU_PERF_CORE), 7 * 8 + 8 * 4);
    eq("RKNPU_PERF_MEMORY size", sizeof(RKNPU_PERF_MEMORY), 2 * 8 + 2 * 4);
    eq("JobTimeBase offset", offsetof(RKNPU_PERF_CORE, JobTimeBase), 56);

    printf("perfcheck: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
