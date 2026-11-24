/*
 * rknpu_xport.h - how device_rknpu.c reaches rknpu.sys
 *
 * Three calls. On Windows they are CreateFileW and DeviceIoControl on
 * \\.\RKNPU (xport_win.c); in the host test they are an in-memory model of
 * the driver (test/xport_fake.c) that runs the driver's own stream check.
 * device_rknpu.c sees nothing else of the operating system.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RKNPU_XPORT_H
#define RKNPU_XPORT_H

struct rkx;

/* NULL if the device will not open; *err is then a negative errno. */
struct rkx *rkx_open(const char *path, int *err);
void rkx_close(struct rkx *x);

/*
 * One IOCTL. 0, or a negative errno translated from the driver's NTSTATUS:
 *
 *   -EBUSY    STATUS_DEVICE_BUSY            the core (or the buffer) is in use
 *   -EACCES   STATUS_ACCESS_VIOLATION       a stream broke rknpu_regcmd.h's rules
 *   -E2BIG    STATUS_INVALID_BUFFER_SIZE    the streams do not fit the copy arena
 *   -ENOMEM   STATUS_INSUFFICIENT_RESOURCES no contiguous memory below 4 GiB
 *   -ENOSPC   STATUS_QUOTA_EXCEEDED         RKNPU_BO_MAX_TOTAL reached
 *   -EINVAL   STATUS_INVALID_PARAMETER
 *   -EIO      anything else
 *
 * *got is the number of bytes the driver returned.
 */
int rkx_ioctl(struct rkx *x, unsigned int code, const void *in, unsigned int inlen,
              void *out, unsigned int outlen, unsigned int *got);

/* Sleep about `us` microseconds, or just yield the CPU for 0. */
void rkx_pause(unsigned int us);

/* A spinning thread's pause: the CPU's YIELD hint (M7). */
void rkx_relax(void);

/* A monotonic clock in microseconds. */
unsigned long long rkx_now_us(void);

/* Every write this thread made to a write-combined mapping is out of the CPU. */
void rkx_wmb(void);

/* M6, for the stats line: the process's page faults and peak working set,
 * as a short static string ("" where the host has no such numbers). */
const char *rkx_mem_report(void);

/*
 * M7, CHARSIU_RKNPU_UCACHE: data cache maintenance by VA from this process.
 * rkx_dc_usable() is 1 when the CPU lets user mode execute DC CVAC and DC
 * CIVAC (SCTLR_EL1.UCI), probed once; the other two are then the driver's
 * FINI_BO and PREP_BO on [p, p + n), each ending with a DSB.
 */
#include <stddef.h>
int rkx_dc_usable(void);
void rkx_dc_clean(const void *p, size_t n);
void rkx_dc_clean_inval(const void *p, size_t n);

#endif /* RKNPU_XPORT_H */
