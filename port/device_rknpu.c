/*
 * device_rknpu.c - charsiu's device layer on rknpu.sys
 *
 * Replaces external/charsiu src/device.c, which drives mainline Linux's
 * rocket through DRM accel ioctls. Every other charsiu file is compiled
 * unchanged and sees the same interface (include/charsiu.h, "the device").
 * What rocket does and what this does instead:
 *
 *   rocket                               here (rknpu.sys, M3 + M4 IOCTLs)
 *   ------------------------------------ -------------------------------------
 *   one fd = one scheduler entity;       one handle = one charsiu device; its
 *   its jobs run in order, on whichever  jobs run in order: a job waits for
 *   core is free                         the previous one of the same device,
 *                                        then takes whichever core is free
 *   CREATE_BO + mmap, IOVA per fd        CREATE_BO + MAP_BO; M6: the address
 *                                        is an IOVA in the driver's IOMMU
 *                                        table, shared by all handles (M4,
 *                                        bypass: physical, below 4 GiB)
 *   PREP_BO waits for the BO's fences,   PREP waits for the device's last job
 *   then invalidates the CPU cache       (a superset of the BO's fences), then
 *                                        M6 PREP_BO cleans and invalidates a
 *                                        cached buffer; a write-combined one
 *                                        has no cache to invalidate
 *   FINI_BO cleans the CPU cache         M6 FINI_BO cleans a cached buffer; a
 *                                        write-combined one drains the WC
 *                                        buffer, as M4
 *   SUBMIT: N jobs of tasks, queued      the tasks of all N jobs, in order,
 *                                        as SUBMITs of at most RKNPU_MAX_TASKS
 *                                        tasks and RKNPU_COPY_ARENA_BYTES of
 *                                        stream, each after the last finished;
 *                                        the driver copies and checks every
 *                                        stream (RKNPU_SUBMIT_COPY)
 *   GEM_CLOSE                            FREE_BO, after the device's last job
 *
 * Running N jobs of one submit back to back on one core is what rocket does
 * too: they share one entity, and an entity's jobs never overlap. Waiting for
 * more than a BO's own fences is never wrong, only slower; M5 is where that
 * is priced.
 *
 * Environment (all optional):
 *   CHARSIU_NPU_SPIN_US   poll this long before blocking in PREP. Default 0
 *                         here (device.c: 200): each poll is an IOCTL, and the
 *                         M4 board run won 48 of 6484 polls (1%) while
 *                         spending 206 us of a CPU on each wait. A blocking
 *                         WAIT is woken by the job's interrupt (ISR, DPC,
 *                         event), so 0 is interrupt-driven completion
 *   CHARSIU_RKNPU_CRUMBS  the driver's registry breadcrumbs: "all" on every
 *                         SUBMIT, "off" never; by default the first 16
 *                         SUBMITs of a device and every 4096th (each one is
 *                         a registry flush, which costs milliseconds)
 *   CHARSIU_RKNPU_STATS   1: one line of counters per device at close, and
 *                         (M6) the cache maintenance and the process's page
 *                         faults and peak working set
 *   CHARSIU_ACCEL         the device path (default \\.\RKNPU)
 *   CHARSIU_RKNPU_FUSE    M7, default 1 on an M7 driver: FINI_BO is deferred
 *                         to the device's next SUBMIT (RKNPU_SUBMIT_CLEAN)
 *                         and the PREP_BO after a job rides on its WAIT
 *                         (RKNPU_WAIT_PREP), so a decode call is four IOCTLs
 *                         instead of ten; 0 is M6's one IOCTL each
 *   CHARSIU_RKNPU_UCACHE  M7, default 0: 1 does PREP and FINI in this process
 *                         (DC CIVAC / DC CVAC on the buffer's own mapping,
 *                         no IOCTL at all) if the CPU lets user mode do it,
 *                         which is probed once; falls back to the above
 *
 * With CHARSIU_RKNPU_STATS=1 the close also prints an M7 timing line: the
 * SUBMIT and WAIT IOCTLs' wall time against the driver's own drv_us, and the
 * interrupt-to-DPC and DPC-to-waiter times, so the operating system's share
 * of a call is a measurement and not a remainder.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "charsiu.h"
#include "rknpu_ioctl.h"
#include "rknpu_xport.h"

#ifndef ETIMEDOUT
#define ETIMEDOUT 138
#endif

/* How long a job may take before WAIT stops it: rocket's 500 ms is per job,
 * and a SUBMIT here can carry 64 of charsiu's tasks. */
#define DEV_WAIT_MS             2000u
/* How long to keep asking for a core another handle is using. */
#define DEV_BUSY_MS             5000u

struct charsiu_device {
	struct rkx *x;
	unsigned index;                 /* open order, for the core preference */
	pthread_mutex_t lock;

	int pending;                    /* a SUBMIT not yet waited for */
	unsigned long long seq;
	unsigned core;
	int last_err;                   /* the last job's failure, reported once */

	/* counters, for CHARSIU_RKNPU_STATS and the failure messages */
	unsigned long submits, chunks, tasks, waits, polls, poll_won, busy;
	unsigned long timeouts, dma_errors, refused;
	unsigned long bos, bo_bytes, bo_peak, bo_bytes_peak;
	unsigned long long hw_us, wait_us;

	/* M6 */
	int cached;                     /* the driver's buffers are cached: PREP/FINI do work */
	unsigned mem_mode;              /* RKNPU_MEM_*, from GET_INFO */
	unsigned long preps, finis;
	unsigned long long sync_us;

	/* M7 */
	int m7;                         /* the driver has RKNPU_SUBMIT_CLEAN and RKNPU_WAIT_PREP */
	int fuse;                       /* and they are used (CHARSIU_RKNPU_FUSE) */
	int ucache;                     /* PREP / FINI done here (CHARSIU_RKNPU_UCACHE) */
	unsigned defer[RKNPU_M7_MAX_CLEAN];     /* FINI_BOs for the next SUBMIT; lock */
	unsigned ndefer;
	unsigned long deferred, defer_full, cleaned_by_submit, prepped_by_wait, uc_ops;
	unsigned long long uc_us;
	/* the IOCTLs' wall time and what the driver says of it */
	unsigned long sub_n, wait_n, wait_slept, wait_timed;
	unsigned long long sub_wall_us, sub_drv_us, wait_wall_us, irq_dpc_us, dpc_wake_us, late_us;
};

static unsigned dev_count;              /* opened so far, under no lock: open is single-threaded */
static pthread_mutex_t open_lock = PTHREAD_MUTEX_INITIALIZER;

static long env_long(const char *name, long dflt)
{
	const char *e = getenv(name);

	return (e && *e) ? atol(e) : dflt;
}

/* ---------------------------------------------------- the crash note */

/*
 * device.c installs a signal handler that names the last stage. The same
 * note here; how it is printed is the port's business (compat_win.c on
 * Windows, nothing on the host test).
 */
const char *volatile charsiu_note_what = "nothing yet";
volatile unsigned long charsiu_note_a, charsiu_note_b;

void charsiu_note(const char *what, unsigned long a, unsigned long b)
{
	charsiu_note_what = what;
	charsiu_note_a = a;
	charsiu_note_b = b;
}

void charsiu_port_crash_handler_install(void);   /* compat_win.c, or the test's stub */

void charsiu_note_install(void)
{
	static int done;

	if (done || getenv("CHARSIU_NO_CRASH_NOTE"))
		return;
	done = 1;
	charsiu_port_crash_handler_install();
}

/* ----------------------------------------------------------- waiting */

unsigned long charsiu_spin_tries, charsiu_spin_won, charsiu_spin_lost;
unsigned long charsiu_spin_won_us, charsiu_spin_lost_us;

void charsiu_spin_report(void)
{
	if (!charsiu_spin_tries)
		return;
	fprintf(stderr,
		"charsiu spin: %lu polls, %lu won without blocking (%.0f%%, "
		"mean %.0f us), %lu fell back (mean %.0f us of polling before "
		"they did)\n",
		charsiu_spin_tries, charsiu_spin_won,
		100.0 * charsiu_spin_won / charsiu_spin_tries,
		charsiu_spin_won ? (double)charsiu_spin_won_us / charsiu_spin_won : 0.0,
		charsiu_spin_lost,
		charsiu_spin_lost ? (double)charsiu_spin_lost_us / charsiu_spin_lost : 0.0);
}

/* One WAIT. prep: M7, a buffer handle to PREP_BO in the same IOCTL, or 0. */
static int wait_once(struct charsiu_device *dev, unsigned ms, unsigned flags,
		     unsigned prep, rknpu_wait_out *o)
{
	rknpu_wait_m7_in in;
	unsigned long long t0 = rkx_now_us();
	unsigned got = 0;
	int r;

	memset(&in, 0, sizeof(in));
	in.base.seqno = dev->seq;
	in.base.timeout_ms = ms;
	in.base.flags = flags;
	if (dev->m7 && !(flags & RKNPU_WAIT_POLL))
		in.base.flags |= RKNPU_WAIT_LEAN;
	if (prep) {
		in.base.flags |= RKNPU_WAIT_PREP;
		in.prep_handle = prep;
	}
	r = rkx_ioctl(dev->x, IOCTL_RKNPU_WAIT, &in,
		      prep ? (unsigned)sizeof(in) : (unsigned)sizeof(in.base),
		      o, sizeof(*o), &got);
	if (!r && got < sizeof(*o))
		r = -EIO;
	if (!r && !(flags & RKNPU_WAIT_POLL)) {
		dev->wait_n++;
		dev->wait_wall_us += rkx_now_us() - t0;
		if (dev->m7) {
			dev->wait_timed++;
			dev->irq_dpc_us += o->irq_dpc_us;
			if (o->m7_flags & RKNPU_WAIT_OUT_SLEPT) {
				dev->wait_slept++;
				dev->dpc_wake_us += o->dpc_wake_us;
			} else {
				dev->late_us += o->dpc_wake_us;
			}
		}
	}
	return r;
}

/*
 * The device's last SUBMIT, finished. Lock held. 0, or the job's failure:
 * -ETIMEDOUT (the driver stopped it and reset the core), -EIO (a DMA error,
 * or the driver did not know the job).
 */
static int dev_wait_prep(struct charsiu_device *dev, unsigned ms, unsigned prep, int *prepped)
{
	static long spin_us = -1;
	rknpu_wait_out o;
	int r;

	if (prepped)
		*prepped = 0;
	if (!dev->pending)
		return 0;
	if (spin_us < 0)
		spin_us = env_long("CHARSIU_NPU_SPIN_US", 0);

	dev->waits++;
	if (spin_us > 0) {
		unsigned long long t0 = rkx_now_us(), el;

		charsiu_spin_tries++;
		for (;;) {
			dev->polls++;
			r = wait_once(dev, 0, RKNPU_WAIT_POLL, 0, &o);
			if (r || o.result != RKNPU_JOB_RUNNING) {
				el = rkx_now_us() - t0;
				charsiu_spin_won++;
				charsiu_spin_won_us += (unsigned long)el;
				dev->poll_won++;
				goto done;
			}
			el = rkx_now_us() - t0;
			if (el > (unsigned long long)spin_us) {
				charsiu_spin_lost++;
				charsiu_spin_lost_us += (unsigned long)el;
				break;
			}
		}
	}
	r = wait_once(dev, ms, 0, prep, &o);
	if (!r && prep && (o.m7_flags & RKNPU_WAIT_OUT_PREPPED)) {
		dev->prepped_by_wait++;
		if (prepped)
			*prepped = 1;
	}
done:
	dev->pending = 0;
	if (r) {
		fprintf(stderr, "charsiu: rknpu WAIT failed (%d) for job %llu on core %u\n",
			r, dev->seq, dev->core);
		return dev->last_err = r;
	}
	dev->hw_us += o.hw_us;
	dev->wait_us += o.wait_us;
	switch (o.result) {
	case RKNPU_JOB_DONE:
		return 0;
	case RKNPU_JOB_TIMEOUT:
		dev->timeouts++;
		fprintf(stderr, "charsiu: rknpu job %llu on core %u TIMED OUT after %u ms: "
			"%u of %u tasks, raw 0x%08x, task_status 0x%08x, reset 0x%08x\n",
			o.seqno, o.core, ms, o.tasks_done, o.task_count, o.raw_status,
			o.task_status, o.reset_status);
		return dev->last_err = -ETIMEDOUT;
	case RKNPU_JOB_MMU_FAULT:
		dev->dma_errors++;
		fprintf(stderr, "charsiu: rknpu job %llu on core %u IOMMU FAULT at NPU address 0x%08x "
			"(status 0x%08x): %u of %u tasks; the core was reset\n", o.seqno, o.core,
			o.fault_addr, o.fault_status, o.tasks_done, o.task_count);
		return dev->last_err = -EIO;
	case RKNPU_JOB_DMA_ERROR:
		dev->dma_errors++;
		fprintf(stderr, "charsiu: rknpu job %llu on core %u DMA ERROR: raw 0x%08x, "
			"%u of %u tasks\n", o.seqno, o.core, o.raw_status, o.tasks_done,
			o.task_count);
		return dev->last_err = -EIO;
	default:
		fprintf(stderr, "charsiu: rknpu job %llu: WAIT result %u\n", o.seqno, o.result);
		return dev->last_err = -EIO;
	}
}

static int dev_wait(struct charsiu_device *dev, unsigned ms)
{
	return dev_wait_prep(dev, ms, 0, NULL);
}

/* ------------------------------------------------------- open, close */

struct charsiu_device *charsiu_open(const char *path)
{
	struct charsiu_device *dev;
	struct rkx *x;
	rknpu_info info;
	unsigned got = 0;
	int err = 0;

	charsiu_note_install();
	/*
	 * device.c's path is a Linux node (/dev/accel/accelN); nothing that
	 * names one means anything here. CHARSIU_ACCEL still picks the device.
	 */
	if (path && path[0] == '/')
		path = NULL;
	if (!path)
		path = getenv("CHARSIU_ACCEL");
	x = rkx_open(path, &err);
	if (!x)
		return NULL;

	/* An M3 driver would refuse the copy flag at the first SUBMIT; say so now. */
	memset(&info, 0, sizeof(info));
	if (rkx_ioctl(x, IOCTL_RKNPU_GET_INFO, NULL, 0, &info, sizeof(info), &got) ||
	    got < sizeof(info) || info.interface_version != RKNPU_INTERFACE_VERSION ||
	    info.m3_version < RKNPU_M3_VERSION || info.m4_version < RKNPU_M4_VERSION) {
		fprintf(stderr, "charsiu: rknpu.sys answers but is not an M4 driver "
			"(interface %u, m3 %u, m4 %u); staying on the CPU\n",
			info.interface_version, info.m3_version, info.m4_version);
		rkx_close(x);
		return NULL;
	}
	if (!(info.flags & RKNPU_INFO_INTERRUPTS_CONNECTED)) {
		fprintf(stderr, "charsiu: rknpu.sys has no interrupts connected; staying on the CPU\n");
		rkx_close(x);
		return NULL;
	}

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		rkx_close(x);
		return NULL;
	}
	dev->x = x;
	/* M6: an M4 driver leaves these 0, which is M4's model */
	dev->mem_mode = info.m6_version ? info.mem_mode : RKNPU_MEM_BYPASS;
	dev->cached = info.m6_version && info.bo_cached;
	/* M7: an M6 driver leaves m7_version 0 and refuses the new flags */
	dev->m7 = info.m6_version && info.m7_version >= RKNPU_M7_VERSION;
	dev->fuse = dev->m7 && dev->cached && env_long("CHARSIU_RKNPU_FUSE", 1) != 0;
	dev->ucache = dev->cached && env_long("CHARSIU_RKNPU_UCACHE", 0) != 0 && rkx_dc_usable();
	pthread_mutex_init(&dev->lock, NULL);
	pthread_mutex_lock(&open_lock);
	dev->index = dev_count++;
	pthread_mutex_unlock(&open_lock);
	if (dev->index == 0 && env_long("CHARSIU_RKNPU_STATS", 0))
		fprintf(stderr, "charsiu rknpu: %s driver; cache maintenance %s\n",
			dev->m7 ? "M7" : info.m6_version ? "M6" : "M4",
			!dev->cached ? "not needed (write-combined)" :
			dev->ucache ? "in this process (DC by VA, CHARSIU_RKNPU_UCACHE)" :
			dev->fuse ? "fused into SUBMIT and WAIT (CHARSIU_RKNPU_FUSE)" :
			"one IOCTL a buffer (M6)");
	/*
	 * device.c burns the first buffer so that no stream sits at IOVA 0.
	 * Physical addresses here are never 0 (DRAM starts at 0x40000000), and
	 * the M6 driver hands out IOVAs from 1 MiB up, so there is nothing to
	 * burn.
	 */
	return dev;
}

void charsiu_close(struct charsiu_device *dev)
{
	if (!dev)
		return;
	pthread_mutex_lock(&dev->lock);
	(void)dev_wait(dev, DEV_WAIT_MS);
	pthread_mutex_unlock(&dev->lock);
	if (env_long("CHARSIU_RKNPU_STATS", 0))
		fprintf(stderr, "charsiu rknpu dev %u: %lu submits as %lu chunks of %lu tasks, "
			"%lu waits (%lu polls, %lu won), %lu busy retries, %lu timeouts, "
			"%lu dma errors, %lu refused; %lu BOs now, peak %lu BOs / %.1f MiB; "
			"NPU %.1f ms, waited %.1f ms; %s, %s, %lu preps + %lu finis in %.1f ms; %s\n",
			dev->index, dev->submits, dev->chunks, dev->tasks, dev->waits,
			dev->polls, dev->poll_won, dev->busy, dev->timeouts, dev->dma_errors,
			dev->refused, dev->bos, dev->bo_peak, dev->bo_bytes_peak / 1048576.0,
			dev->hw_us / 1e3, dev->wait_us / 1e3,
			dev->mem_mode == RKNPU_MEM_IOMMU ? "iommu" : "bypass",
			dev->cached ? "cached" : "write-combined", dev->preps, dev->finis,
			dev->sync_us / 1e3, rkx_mem_report());
	if (env_long("CHARSIU_RKNPU_STATS", 0))
		fprintf(stderr, "charsiu rknpu dev %u timing: %lu SUBMITs %.1f us avg (driver %.1f), "
			"%lu WAITs %.1f us avg, irq to dpc %.1f us, dpc to waiter %.1f us over %lu "
			"that slept, %lu came after the dpc by %.1f us avg; %lu FINIs deferred "
			"(%lu by SUBMIT, %lu list full), %lu PREPs by WAIT, %lu done here in %.1f ms\n",
			dev->index, dev->sub_n, dev->sub_n ? (double)dev->sub_wall_us / dev->sub_n : 0.0,
			dev->sub_n ? (double)dev->sub_drv_us / dev->sub_n : 0.0,
			dev->wait_n, dev->wait_n ? (double)dev->wait_wall_us / dev->wait_n : 0.0,
			dev->wait_timed ? (double)dev->irq_dpc_us / dev->wait_timed : 0.0,
			dev->wait_slept ? (double)dev->dpc_wake_us / dev->wait_slept : 0.0,
			dev->wait_slept, dev->wait_timed - dev->wait_slept,
			dev->wait_timed > dev->wait_slept ?
				(double)dev->late_us / (dev->wait_timed - dev->wait_slept) : 0.0,
			dev->deferred, dev->cleaned_by_submit, dev->defer_full, dev->prepped_by_wait,
			dev->uc_ops, dev->uc_us / 1e3);
	/* closing the handle frees every buffer it still holds (NpuM3FileCleanup) */
	rkx_close(dev->x);
	pthread_mutex_destroy(&dev->lock);
	free(dev);
}

/* ----------------------------------------------------------- buffers */

int charsiu_bo_alloc(struct charsiu_device *dev, size_t size, struct charsiu_bo *bo)
{
	rknpu_create_bo cb;
	rknpu_map_bo mb;
	rknpu_bo_handle fh;
	unsigned got = 0;
	int r;

	if (!dev || !bo || !size)
		return -EINVAL;
	if (size > RKNPU_BO_MAX_SIZE) {
		fprintf(stderr, "charsiu: a %.1f MiB buffer is over rknpu.sys's %u MiB limit\n",
			size / 1048576.0, RKNPU_BO_MAX_SIZE >> 20);
		return -E2BIG;
	}
	memset(&cb, 0, sizeof(cb));
	cb.size = (unsigned)size;
	pthread_mutex_lock(&dev->lock);
	r = rkx_ioctl(dev->x, IOCTL_RKNPU_CREATE_BO, &cb, sizeof(cb), &cb, sizeof(cb), &got);
	if (r)
		goto out;
	memset(&mb, 0, sizeof(mb));
	mb.handle = cb.handle;
	r = rkx_ioctl(dev->x, IOCTL_RKNPU_MAP_BO, &mb, sizeof(mb), &mb, sizeof(mb), &got);
	if (r || !mb.user_va) {
		memset(&fh, 0, sizeof(fh));
		fh.handle = cb.handle;
		(void)rkx_ioctl(dev->x, IOCTL_RKNPU_FREE_BO, &fh, sizeof(fh), NULL, 0, &got);
		r = r ? r : -ENOMEM;
		goto out;
	}
	bo->handle = cb.handle;
	bo->dma_address = cb.dma_address;
	bo->size = size;
	bo->map = (void *)(uintptr_t)mb.user_va;
	dev->bos++;
	dev->bo_bytes += cb.size;
	if (dev->bos > dev->bo_peak)
		dev->bo_peak = dev->bos;
	if (dev->bo_bytes > dev->bo_bytes_peak)
		dev->bo_bytes_peak = dev->bo_bytes;
out:
	pthread_mutex_unlock(&dev->lock);
	if (r)
		fprintf(stderr, "charsiu: rknpu buffer of %zu bytes failed (%d) with %lu buffers, "
			"%.1f MiB held%s\n", size, r, dev->bos, dev->bo_bytes / 1048576.0,
			r == -ENOMEM ? (dev->mem_mode == RKNPU_MEM_IOMMU ?
					": no memory or no NPU address space" :
					": no contiguous memory below 4 GiB") :
			r == -ENOSPC ? ": the driver's total is reached" : "");
	return r;
}

void charsiu_bo_free(struct charsiu_device *dev, struct charsiu_bo *bo)
{
	rknpu_bo_handle fh;
	unsigned got = 0, size;

	if (!bo || !bo->map)
		return;
	bo->map = NULL;
	if (!dev || !bo->handle)
		return;
	size = (unsigned)((bo->size + 4095) & ~(size_t)4095);
	pthread_mutex_lock(&dev->lock);
	/* M7: a deferred FINI_BO of a buffer about to go would fail the SUBMIT */
	for (unsigned i = 0; i < dev->ndefer; i++)
		if (dev->defer[i] == bo->handle) {
			dev->defer[i] = dev->defer[--dev->ndefer];
			break;
		}
	/* the device's last job may still be reading it */
	(void)dev_wait(dev, DEV_WAIT_MS);
	memset(&fh, 0, sizeof(fh));
	fh.handle = bo->handle;
	if (!rkx_ioctl(dev->x, IOCTL_RKNPU_FREE_BO, &fh, sizeof(fh), NULL, 0, &got)) {
		dev->bos--;
		dev->bo_bytes -= size;
	}
	pthread_mutex_unlock(&dev->lock);
	bo->handle = 0;
}

/* M6: PREP_BO or FINI_BO on the whole buffer. No lock: the driver looks the
 * buffer up under its own, and it takes no job state. */
static int bo_sync(struct charsiu_device *dev, unsigned code, const struct charsiu_bo *bo)
{
	rknpu_bo_sync bs;
	unsigned long long t0 = rkx_now_us();
	unsigned got = 0;
	int r;

	memset(&bs, 0, sizeof(bs));
	bs.handle = bo->handle;
	r = rkx_ioctl(dev->x, code, &bs, sizeof(bs), NULL, 0, &got);
	if (code == IOCTL_RKNPU_PREP_BO)
		dev->preps++;
	else
		dev->finis++;
	dev->sync_us += rkx_now_us() - t0;
	if (r)
		fprintf(stderr, "charsiu: rknpu %s of buffer %u failed (%d)\n",
			code == IOCTL_RKNPU_PREP_BO ? "PREP_BO" : "FINI_BO", bo->handle, r);
	return r;
}

/* M7, CHARSIU_RKNPU_UCACHE: the maintenance by VA in this process */
static void uc_sync(struct charsiu_device *dev, const struct charsiu_bo *bo, int prep)
{
	unsigned long long t0 = rkx_now_us();

	if (prep)
		rkx_dc_clean_inval(bo->map, bo->size);
	else
		rkx_dc_clean(bo->map, bo->size);
	dev->uc_ops++;
	dev->uc_us += rkx_now_us() - t0;
}

int charsiu_bo_prep(struct charsiu_device *dev, struct charsiu_bo *bo, int64_t timeout_ns)
{
	long long ms = timeout_ns / 1000000;
	int r, prepped = 0;
	int want = dev && dev->cached && bo && bo->handle;

	if (!dev)
		return -EINVAL;
	/* a WAIT that times out stops the job, so never hand it a short one */
	if (ms < (long long)DEV_WAIT_MS)
		ms = DEV_WAIT_MS;
	if (ms > RKNPU_WAIT_MAX_MS)
		ms = RKNPU_WAIT_MAX_MS;
	pthread_mutex_lock(&dev->lock);
	/* M7: the job's WAIT does this buffer's PREP_BO in the same IOCTL */
	r = dev_wait_prep(dev, (unsigned)ms, want && dev->fuse && !dev->ucache ? bo->handle : 0,
			  &prepped);
	pthread_mutex_unlock(&dev->lock);
	if (r || !want || prepped)
		return r;
	/* M6: what the NPU wrote, not what the CPU's caches still hold */
	if (dev->ucache && bo->map) {
		uc_sync(dev, bo, 1);
		return 0;
	}
	return bo_sync(dev, IOCTL_RKNPU_PREP_BO, bo);
}

int charsiu_bo_fini(struct charsiu_device *dev, struct charsiu_bo *bo)
{
	unsigned i;

	rkx_wmb();
	if (!dev || !dev->cached || !bo || !bo->handle)
		return 0;
	if (dev->ucache && bo->map) {
		uc_sync(dev, bo, 0);
		return 0;
	}
	/*
	 * M7: nothing but this device's next SUBMIT can make the NPU read this
	 * buffer (the driver's address check takes this handle's buffers
	 * only), so its clean can wait for that SUBMIT and ride on it.
	 */
	if (dev->fuse) {
		pthread_mutex_lock(&dev->lock);
		for (i = 0; i < dev->ndefer; i++)
			if (dev->defer[i] == bo->handle)
				break;
		if (i < dev->ndefer || dev->ndefer < RKNPU_M7_MAX_CLEAN) {
			if (i == dev->ndefer)
				dev->defer[dev->ndefer++] = bo->handle;
			dev->deferred++;
			pthread_mutex_unlock(&dev->lock);
			return 0;
		}
		dev->defer_full++;
		pthread_mutex_unlock(&dev->lock);
	}
	/* M6: what the CPU wrote reaches memory before the NPU reads it */
	return bo_sync(dev, IOCTL_RKNPU_FINI_BO, bo);
}

/* M7: the deferred FINI_BOs one IOCTL each, when a SUBMIT could not carry
 * them. Lock held. */
static void defer_flush(struct charsiu_device *dev)
{
	struct charsiu_bo b;
	unsigned i;

	memset(&b, 0, sizeof(b));
	for (i = 0; i < dev->ndefer; i++) {
		b.handle = dev->defer[i];
		(void)bo_sync(dev, IOCTL_RKNPU_FINI_BO, &b);
	}
	dev->ndefer = 0;
}

/* -------------------------------------------------------- submission */

static unsigned crumb_flags(struct charsiu_device *dev)
{
	static int mode = -1;   /* 0 default, 1 all, 2 off */
	const char *e;

	if (mode < 0) {
		e = getenv("CHARSIU_RKNPU_CRUMBS");
		mode = !e ? 0 : !strcmp(e, "all") ? 1 : !strcmp(e, "off") ? 2 : 0;
	}
	if (mode == 1)
		return 0;
	if (mode == 0 && (dev->chunks < 16 || dev->chunks % 4096 == 0))
		return 0;
	return RKNPU_SUBMIT_NO_CRUMBS;
}

/*
 * One SUBMIT of in->task_count tasks, on whichever core is free, the
 * device's preferred one first. Lock held, nothing pending.
 */
static int submit_chunk(struct charsiu_device *dev, rknpu_submit_m7_in *m7)
{
	rknpu_submit_in *in = &m7->base;
	unsigned long long t0 = 0, ts;
	unsigned try = 0, got = 0, inlen = sizeof(*in);
	rknpu_submit_out out;
	int r;

	in->flags = RKNPU_SUBMIT_COPY | crumb_flags(dev);
	/* M7: the FINI_BOs deferred to this SUBMIT */
	if (dev->ndefer) {
		in->flags |= RKNPU_SUBMIT_CLEAN;
		m7->clean_count = dev->ndefer;
		memcpy(m7->clean, dev->defer, dev->ndefer * sizeof(m7->clean[0]));
		inlen = sizeof(*m7);
	}
	for (;;) {
		in->core = (dev->index + try) % RKNPU_NUM_CORES;
		memset(&out, 0, sizeof(out));
		ts = rkx_now_us();
		r = rkx_ioctl(dev->x, IOCTL_RKNPU_SUBMIT, m7, inlen, &out, sizeof(out), &got);
		if (!r) {
			dev->sub_n++;
			dev->sub_wall_us += rkx_now_us() - ts;
			dev->sub_drv_us += out.drv_us;
		}
		if (r != -EBUSY)
			break;
		/* the other handle's job on that core; try the other core, then wait */
		dev->busy++;
		if (++try % RKNPU_NUM_CORES)
			continue;
		if (!t0)
			t0 = rkx_now_us();
		else if (rkx_now_us() - t0 > DEV_BUSY_MS * 1000ull)
			break;
		rkx_pause(try < 64 ? 0 : 50);
	}
	if (in->flags & RKNPU_SUBMIT_CLEAN) {
		if (!r)
			dev->cleaned_by_submit += dev->ndefer;
		else
			defer_flush(dev);       /* do not carry them into another failure */
		dev->ndefer = 0;
		in->flags &= ~RKNPU_SUBMIT_CLEAN;
		m7->clean_count = 0;
	}
	if (r) {
		if (r == -EACCES)
			dev->refused++;
		fprintf(stderr, "charsiu: rknpu SUBMIT of %u tasks (first stream 0x%08x, %u entries) "
			"failed: %d%s\n", in->task_count, in->tasks[0].regcmd, in->tasks[0].regcmd_count, r,
			r == -EACCES ? ", a stream wrote the PC block, a register charsiu never "
				       "writes, or an address outside this device's buffers (the "
				       "driver's debug log and its M3Value breadcrumbs name the entry)" :
			r == -EBUSY ? ", both cores stayed busy" : "");
		return r;
	}
	dev->pending = 1;
	dev->seq = out.seqno;
	dev->core = out.core;
	dev->chunks++;
	dev->tasks += in->task_count;
	return 0;
}

/* The bytes the PC fetches for a stream, as the driver's arena counts them. */
static unsigned stream_bytes(unsigned count)
{
	return (count + 1) / 2 * 2 * 8;
}

int charsiu_submit_jobs(struct charsiu_device *dev,
			const struct charsiu_joblist *jobs, unsigned job_count)
{
	rknpu_submit_m7_in m7;
	rknpu_submit_in *const inp = &m7.base;
	unsigned i, t, total = 0, bytes = 0;
	int r = 0;

	if (!dev || !jobs || !job_count)
		return -EINVAL;
	for (i = 0; i < job_count; i++)
		total += jobs[i].task_count;
	if (!total)
		return -EINVAL;

	pthread_mutex_lock(&dev->lock);
	dev->submits++;
	/* rocket would queue behind the entity's last job; here it runs after it */
	r = dev_wait(dev, DEV_WAIT_MS);
	if (r)
		goto out;
	memset(&m7, 0, sizeof(m7));
	for (i = 0; i < job_count; i++) {
		for (t = 0; t < jobs[i].task_count; t++) {
			const struct charsiu_task *k = &jobs[i].tasks[t];
			unsigned b = stream_bytes(k->regcmd_count);

			if (!k->regcmd_count || k->regcmd_count > RKNPU_MAX_REGCMD ||
			    b > RKNPU_COPY_ARENA_BYTES) {
				fprintf(stderr, "charsiu: a stream of %u entries cannot be submitted "
					"(limit %u entries, %u bytes of arena)\n", k->regcmd_count,
					RKNPU_MAX_REGCMD, RKNPU_COPY_ARENA_BYTES);
				r = -E2BIG;
				goto out;
			}
			if (inp->task_count == RKNPU_MAX_TASKS || bytes + b > RKNPU_COPY_ARENA_BYTES) {
				r = submit_chunk(dev, &m7);
				if (!r)
					r = dev_wait(dev, DEV_WAIT_MS);
				if (r)
					goto out;
				memset(&m7, 0, sizeof(m7));
				bytes = 0;
			}
			inp->tasks[inp->task_count].regcmd = k->regcmd;
			inp->tasks[inp->task_count].regcmd_count = k->regcmd_count;
			inp->task_count++;
			bytes += b;
		}
	}
	r = submit_chunk(dev, &m7);
out:
	pthread_mutex_unlock(&dev->lock);
	return r;
}

int charsiu_submit(struct charsiu_device *dev, const struct charsiu_bo *regcmd,
		   unsigned regcmd_count, const uint32_t *in_handles,
		   unsigned in_count, const uint32_t *out_handles,
		   unsigned out_count)
{
	struct charsiu_task task;
	struct charsiu_joblist job;

	if (regcmd->dma_address >> 32)
		return -ERANGE;
	task.regcmd = (uint32_t)regcmd->dma_address;
	task.regcmd_count = regcmd_count;
	job.tasks = &task;
	job.task_count = 1;
	job.in_handles = in_handles;
	job.in_count = in_count;
	job.out_handles = out_handles;
	job.out_count = out_count;
	return charsiu_submit_jobs(dev, &job, 1);
}
