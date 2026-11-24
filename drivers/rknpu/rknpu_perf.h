/*
 * rknpu_perf.h - the values rknpu.sys hands to PCW for its two counter sets
 *
 * rknpu.man names these structures and their fields; ctrpp turns the
 * manifest into rknpuCounters.h (built into the object directory), whose
 * helpers copy one structure per instance into the consumer's buffer. A
 * field's size must match its counter type: 64 bits for the timer, bulk
 * count, average and large raw count types, 32 bits for raw counts and
 * average bases. tools/perfcheck/check_manifest.py checks that on the host.
 *
 * Plain C types, no kernel headers, so the host test can include it.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RKNPU_PERF_H
#define RKNPU_PERF_H

/* "RK3576 NPU", instances core0 and core1 */
typedef struct _RKNPU_PERF_CORE {
    unsigned long long BusyTime;        /* perf_100nsec_timer: 100 ns a job held the core */
    unsigned long long JobsCompleted;   /* perf_counter_bulk_count: Jobs/sec */
    unsigned long long TasksCompleted;  /* perf_counter_bulk_count: Tasks/sec */
    unsigned long long JobTimeUs;       /* perf_average_bulk over JobTimeBase */
    unsigned long long LatencyUs;       /* perf_average_bulk over LatencyBase */
    unsigned long long JobsSubmitted;   /* perf_counter_large_rawcount */
    unsigned long long JobsCompletedTotal; /* perf_counter_large_rawcount */
    unsigned int       JobTimeBase;     /* perf_average_base: jobs that ended */
    unsigned int       LatencyBase;     /* perf_average_base: jobs completed */
    unsigned int       Timeouts;        /* perf_counter_rawcount, and the rest */
    unsigned int       Resets;
    unsigned int       MmuFaults;
    unsigned int       CbufWaits;
    unsigned int       Running;         /* 1 while a job is on the core */
    unsigned int       Reserved;
} RKNPU_PERF_CORE;

/* "RK3576 NPU Memory", one instance: the device's buffers */
typedef struct _RKNPU_PERF_MEMORY {
    unsigned long long BufferBytes;     /* perf_counter_large_rawcount */
    unsigned long long IovaBytes;       /* perf_counter_large_rawcount: NPU address space in use */
    unsigned int       Buffers;         /* perf_counter_rawcount */
    unsigned int       PageTablePages;  /* perf_counter_rawcount */
} RKNPU_PERF_MEMORY;

/*
 * Ticks of a Freq Hz clock in units of 1/Unit s (Unit 10^7 for 100 ns, 10^6
 * for us), without the overflow of Ticks * Unit: at 24 MHz that product
 * passes 2^64 after about 21 hours of busy time in 100 ns units.
 */
static __inline unsigned long long
rknpu_perf_scale(unsigned long long Ticks, unsigned long long Freq, unsigned long long Unit)
{
    if (Freq == 0)
        return 0;
    return (Ticks / Freq) * Unit + (Ticks % Freq) * Unit / Freq;
}

#endif
