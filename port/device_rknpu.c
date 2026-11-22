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
 *   CREATE_BO + mmap, IOVA per fd        CREATE_BO + MAP_BO; the address is
 *                                        physical (IOMMU bypass), below 4 GiB
 *   PREP_BO waits for the BO's fences,   PREP waits for the device's last job
 *   then invalidates the CPU cache       (a superset of the BO's fences); the
 *                                        mapping is write-combined, so there
 *                                        is no cache to invalidate
 *   FINI_BO cleans the CPU cache         FINI drains the write-combine buffer
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
 *   CHARSIU_NPU_SPIN_US   poll this long before blocking in PREP (default
 *                         200, as device.c); 0 blocks at once
 *   CHARSIU_RKNPU_CRUMBS  the driver's registry breadcrumbs: "all" on every
 *                         SUBMIT, "off" never; by default the first 16
 *                         SUBMITs of a device and every 4096th (each one is
 *                         a registry flush, which costs milliseconds)
 *   CHARSIU_RKNPU_STATS   1: one line of counters per device at close
 *   CHARSIU_ACCEL         the device path (default \\.\RKNPU)
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

static int wait_once(struct charsiu_device *dev, unsigned ms, unsigned flags,
		     rknpu_wait_out *o)
{
	rknpu_wait_in in;
	unsigned got = 0;
	int r;

	memset(&in, 0, sizeof(in));
	in.seqno = dev->seq;
	in.timeout_ms = ms;
	in.flags = flags;
	r = rkx_ioctl(dev->x, IOCTL_RKNPU_WAIT, &in, sizeof(in), o, sizeof(*o), &got);
	if (!r && got < sizeof(*o))
		r = -EIO;
	return r;
}

/*
 * The device's last SUBMIT, finished. Lock held. 0, or the job's failure:
 * -ETIMEDOUT (the driver stopped it and reset the core), -EIO (a DMA error,
 * or the driver did not know the job).
 */
static int dev_wait(struct charsiu_device *dev, unsigned ms)
{
	static long spin_us = -1;
	rknpu_wait_out o;
	int r;

	if (!dev->pending)
		return 0;
	if (spin_us < 0)
		spin_us = env_long("CHARSIU_NPU_SPIN_US", 200);

	dev->waits++;
	if (spin_us > 0) {
		unsigned long long t0 = rkx_now_us(), el;

		charsiu_spin_tries++;
		for (;;) {
			dev->polls++;
			r = wait_once(dev, 0, RKNPU_WAIT_POLL, &o);
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
	r = wait_once(dev, ms, 0, &o);
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
	pthread_mutex_init(&dev->lock, NULL);
	pthread_mutex_lock(&open_lock);
	dev->index = dev_count++;
	pthread_mutex_unlock(&open_lock);
	/*
	 * device.c burns the first buffer so that no stream sits at IOVA 0.
	 * Physical addresses here are never 0 (DRAM starts at 0x40000000), so
	 * there is nothing to burn.
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
			"NPU %.1f ms, waited %.1f ms\n",
			dev->index, dev->submits, dev->chunks, dev->tasks, dev->waits,
			dev->polls, dev->poll_won, dev->busy, dev->timeouts, dev->dma_errors,
			dev->refused, dev->bos, dev->bo_peak, dev->bo_bytes_peak / 1048576.0,
			dev->hw_us / 1e3, dev->wait_us / 1e3);
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
			r == -ENOMEM ? ": no contiguous memory below 4 GiB" :
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

int charsiu_bo_prep(struct charsiu_device *dev, struct charsiu_bo *bo, int64_t timeout_ns)
{
	long long ms = timeout_ns / 1000000;
	int r;

	(void)bo;
	if (!dev)
		return -EINVAL;
	/* a WAIT that times out stops the job, so never hand it a short one */
	if (ms < (long long)DEV_WAIT_MS)
		ms = DEV_WAIT_MS;
	if (ms > RKNPU_WAIT_MAX_MS)
		ms = RKNPU_WAIT_MAX_MS;
	pthread_mutex_lock(&dev->lock);
	r = dev_wait(dev, (unsigned)ms);
	pthread_mutex_unlock(&dev->lock);
	return r;
}

int charsiu_bo_fini(struct charsiu_device *dev, struct charsiu_bo *bo)
{
	(void)dev;
	(void)bo;
	rkx_wmb();
	return 0;
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
static int submit_chunk(struct charsiu_device *dev, rknpu_submit_in *in)
{
	unsigned long long t0 = 0;
	unsigned try = 0, got = 0;
	rknpu_submit_out out;
	int r;

	in->flags = RKNPU_SUBMIT_COPY | crumb_flags(dev);
	for (;;) {
		in->core = (dev->index + try) % RKNPU_NUM_CORES;
		memset(&out, 0, sizeof(out));
		r = rkx_ioctl(dev->x, IOCTL_RKNPU_SUBMIT, in, sizeof(*in), &out, sizeof(out), &got);
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
	rknpu_submit_in in;
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
	memset(&in, 0, sizeof(in));
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
			if (in.task_count == RKNPU_MAX_TASKS || bytes + b > RKNPU_COPY_ARENA_BYTES) {
				r = submit_chunk(dev, &in);
				if (!r)
					r = dev_wait(dev, DEV_WAIT_MS);
				if (r)
					goto out;
				memset(&in, 0, sizeof(in));
				bytes = 0;
			}
			in.tasks[in.task_count].regcmd = k->regcmd;
			in.tasks[in.task_count].regcmd_count = k->regcmd_count;
			in.task_count++;
			bytes += b;
		}
	}
	r = submit_chunk(dev, &in);
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
