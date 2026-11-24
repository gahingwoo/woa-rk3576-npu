/*
 * xport_fake.c - rknpu.sys in memory, for running charsiu on a Linux host
 *
 * The transport of device_rknpu.c, with the driver's rules and none of its
 * hardware: buffers are page-aligned heap memory at made-up 32-bit
 * "physical" addresses, MAP_BO hands back the heap pointer, and SUBMIT does
 * what m3.c does with RKNPU_SUBMIT_COPY up to the point where it would start
 * the core: the task and arena limits, the copy, and rknpu_rc_check_stream()
 * from include/rknpu_regcmd.h, the same function the driver calls. Nothing
 * computes, so the NPU's outputs stay as charsiu left them and the text is
 * garbage; what this checks is the plumbing and the streams.
 *
 * A job stays "on its core" until the next IOCTL from any handle, so a second
 * device that submits at once meets STATUS_DEVICE_BUSY as it can on the
 * board. WAIT with RKNPU_WAIT_POLL says RUNNING once per job.
 *
 * Environment:
 *   RKNPU_FAKE_BASE      the first buffer's address (default 0x40100000)
 *   RKNPU_FAKE_TOTAL_MB  the driver's total (default RKNPU_BO_MAX_TOTAL)
 *   RKNPU_FAKE_SCAN      a file: every (block, register) whose value fell
 *                        inside a buffer, with counts. Two runs at two bases
 *                        separate address registers from coincidences.
 *   RKNPU_FAKE_SCRIBBLE  1: zero each task's output, from the DPU destination
 *                        to the end of its buffer, so that charsiu's "did the
 *                        hardware write it" checks pass and what follows runs
 *   RKNPU_FAKE_REGS      a file: every (block, register) any stream wrote
 *   RKNPU_FAKE_REPORT    1: the totals at exit, on stderr
 *   RKNPU_FAKE_M4        1: answer as the M4 driver (bypass, write-combined,
 *                        no PREP_BO / FINI_BO); default the M7 driver in
 *                        IOMMU mode with cached buffers
 *   RKNPU_FAKE_M6        1: answer as the M6 driver (no RKNPU_SUBMIT_CLEAN,
 *                        RKNPU_WAIT_PREP or RKNPU_WAIT_LEAN)
 *
 * M6: a cached buffer counts the PREP_BO and FINI_BO it gets, and SUBMIT
 * refuses (-EIO, logged) a job whose stream buffer was written through its
 * mapping and never FINIed, which is the bug a cached buffer turns into
 * stale data on the board. "Written" is approximated: a buffer is dirty from
 * MAP_BO and from every PREP_BO until its next FINI_BO.
 *
 * M6, the shared CBUF: a SUBMIT while the other core's job is still running
 * counts as an overlap; if the two jobs' CBUF keys (rknpu_regcmd.h) collide
 * it is counted as a CBUF wait and the other job is finished first, as the
 * driver does. charsiu gives its two devices two windows, so a real run is
 * expected to overlap with no CBUF waits.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rknpu_ioctl.h"
#include "rknpu_regcmd.h"
#include "../rknpu_xport.h"

struct fbo {
	unsigned handle, addr, size;
	void *mem;
	const struct rkx *owner;
	int mapped;
	int dirty;              /* M6: CPU access since the last FINI_BO */
};

struct rkx {
	int open;
};

struct fjob {
	unsigned long long seq;
	const struct rkx *owner;
	int active, polled, age;
	unsigned tasks;
	unsigned key;           /* M6: rknpu_rc_cbuf_key() over its streams */
};

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static struct fbo *bos;
static unsigned nbo, capbo, next_handle;
static unsigned long long next_addr, total, total_peak, total_max, nbo_peak, bo_max_seen;
static unsigned long long seqno;
static struct fjob core_job[RKNPU_NUM_CORES];
static struct fjob hist[RKNPU_JOB_HISTORY];
static unsigned hist_at;
static unsigned char arena[RKNPU_COPY_ARENA_BYTES];

/* statistics */
static unsigned long long n_submit, n_tasks, n_entries, max_tasks, max_entries, max_arena;
static unsigned long long n_busy, n_refused, n_wait, n_poll;
static unsigned long long n_prep, n_fini, n_unfini, n_overlap, n_cbuf_wait;
static unsigned long long n_clean, n_wprep;     /* M7: FINIs a SUBMIT did, PREPs a WAIT did */
static unsigned long long n_ufini, n_uprep;     /* M7: the same done by the caller (UCACHE) */
static int m4, m6;

/* the scan: (target << 16 | reg) -> count, for values inside a buffer */
#define SCAN_SLOTS 4096
struct slot { unsigned key; unsigned long long n; };
static struct slot scan[SCAN_SLOTS], regs[SCAN_SLOTS];
static const char *scan_path, *regs_path;
static int scribble;
static int inited;

static void report(void);

static void init(void)
{
	const char *e;

	if (inited)
		return;
	inited = 1;
	e = getenv("RKNPU_FAKE_BASE");
	next_addr = e ? strtoull(e, NULL, 0) : 0x40100000ull;
	e = getenv("RKNPU_FAKE_TOTAL_MB");
	total_max = e ? strtoull(e, NULL, 0) << 20 : RKNPU_BO_MAX_TOTAL;
	scan_path = getenv("RKNPU_FAKE_SCAN");
	scribble = getenv("RKNPU_FAKE_SCRIBBLE") != NULL;
	regs_path = getenv("RKNPU_FAKE_REGS");
	m4 = getenv("RKNPU_FAKE_M4") != NULL;
	m6 = m4 || getenv("RKNPU_FAKE_M6") != NULL;
	atexit(report);
}

static struct fbo *find(const struct rkx *x, unsigned handle)
{
	unsigned i;

	for (i = 0; i < nbo; i++)
		if (bos[i].handle == handle && bos[i].owner == x)
			return &bos[i];
	return NULL;
}

static struct fbo *find_range(const struct rkx *x, unsigned addr, unsigned len)
{
	unsigned i;

	for (i = 0; i < nbo; i++)
		if (bos[i].owner == x && addr >= bos[i].addr &&
		    (unsigned long long)addr + len <= (unsigned long long)bos[i].addr + bos[i].size)
			return &bos[i];
	return NULL;
}

static int any_range(unsigned addr)
{
	unsigned i;

	for (i = 0; i < nbo; i++)
		if (addr >= bos[i].addr && addr < bos[i].addr + bos[i].size)
			return 1;
	return 0;
}

static int in_bo(void *ctx, unsigned int addr)
{
	return find_range((const struct rkx *)ctx, addr, 1) != NULL;
}

static void count_in(struct slot *t, unsigned key)
{
	unsigned h = (key * 2654435761u) % SCAN_SLOTS, i;

	for (i = 0; i < SCAN_SLOTS; i++, h = (h + 1) % SCAN_SLOTS) {
		if (t[h].n && t[h].key != key)
			continue;
		t[h].key = key;
		t[h].n++;
		return;
	}
}

static void scan_count(unsigned key)
{
	count_in(scan, key);
}

/* the hardware "finishes" every job one IOCTL after it started */
static void tick(void)
{
	unsigned c;

	for (c = 0; c < RKNPU_NUM_CORES; c++)
		if (core_job[c].active && ++core_job[c].age >= 2)
			core_job[c].active = 0;
}

static void retire(unsigned c)
{
	if (!core_job[c].seq)
		return;
	hist[hist_at++ % RKNPU_JOB_HISTORY] = core_job[c];
}

static int do_submit(struct rkx *x, const rknpu_submit_in *in, unsigned inlen, rknpu_submit_out *out)
{
	unsigned i, at = 0, key = RKNPU_RC_CBUF_ANY, other = in->core ^ 1u;
	struct fjob *j;
	unsigned allowed = RKNPU_SUBMIT_NO_CRUMBS | RKNPU_SUBMIT_COPY | (m6 ? 0 : RKNPU_SUBMIT_CLEAN);

	if (in->core >= RKNPU_NUM_CORES || !in->task_count || in->task_count > RKNPU_MAX_TASKS ||
	    (in->flags & ~allowed))
		return -EINVAL;
	if ((in->flags & RKNPU_SUBMIT_CLEAN) &&
	    (inlen < sizeof(rknpu_submit_m7_in) ||
	     ((const rknpu_submit_m7_in *)in)->clean_count > RKNPU_M7_MAX_CLEAN))
		return -EINVAL;
	if (!(in->flags & RKNPU_SUBMIT_COPY))
		return -EINVAL;    /* charsiu always copies; the M3 path is rknputest's */
	for (i = 0; i < in->task_count; i++)
		if ((in->tasks[i].regcmd & 0xF) || !in->tasks[i].regcmd_count ||
		    in->tasks[i].regcmd_count > RKNPU_MAX_REGCMD)
			return -EINVAL;
	j = &core_job[in->core];
	if (j->active) {
		n_busy++;
		return -EBUSY;
	}
	/* M7: the deferred FINI_BOs, all of them before any stream is read */
	if (in->flags & RKNPU_SUBMIT_CLEAN) {
		const rknpu_submit_m7_in *m = (const rknpu_submit_m7_in *)in;

		for (i = 0; i < m->clean_count; i++)
			if (!find(x, m->clean[i]))
				return -EINVAL;     /* STATUS_INVALID_HANDLE */
		for (i = 0; i < m->clean_count; i++) {
			find(x, m->clean[i])->dirty = 0;
			n_clean++;
		}
	}
	/* the copy and the check, as NpuCopyStreamsLocked */
	for (i = 0; i < in->task_count; i++) {
		unsigned fetched = (in->tasks[i].regcmd_count + 1) / 2 * 2, bad = 0, k;
		struct fbo *b = find_range(x, in->tasks[i].regcmd, fetched * 8);
		const uint64_t *src;
		uint64_t *dst;
		int why;

		if (!b)
			return -EINVAL;
		if (!m4 && b->dirty) {
			n_unfini++;
			fprintf(stderr, "rknpu-fake: stream 0x%08x is in a cached buffer written "
				"since its last FINI_BO\n", in->tasks[i].regcmd);
			return -EIO;
		}
		if (at + fetched * 8 > sizeof(arena))
			return -E2BIG;
		src = (const uint64_t *)((unsigned char *)b->mem + (in->tasks[i].regcmd - b->addr));
		dst = (uint64_t *)(arena + at);
		memcpy(dst, src, fetched * 8);
		why = rknpu_rc_check_stream((const unsigned long long *)dst, fetched, 0, in_bo, x, &bad);
		key = rknpu_rc_cbuf_merge(key, rknpu_rc_cbuf_key((const unsigned long long *)dst, fetched), i == 0);
		if (why) {
			n_refused++;
			fprintf(stderr, "rknpu-fake: SUBMIT refused, stream 0x%08x entry %u "
				"(0x%016llx): %s\n", in->tasks[i].regcmd, bad,
				(unsigned long long)dst[bad], rknpu_rc_why(why));
			return -EACCES;
		}
		if (regs_path)
			for (k = 0; k < fetched; k++)
				if (dst[k])
					count_in(regs, RKNPU_RC_TARGET(dst[k]) << 16 | RKNPU_RC_REG(dst[k]));
		if (scan_path)
			for (k = 0; k < fetched; k++) {
				unsigned v = RKNPU_RC_VALUE(dst[k]);

				if (dst[k] && v && any_range(v))
					scan_count(RKNPU_RC_TARGET(dst[k]) << 16 | RKNPU_RC_REG(dst[k]));
			}
		/* RKNPU_FAKE_SCRIBBLE: "write" the output, zeros from the DPU's
		 * destination to the end of its buffer, so charsiu's checks
		 * that the hardware wrote its output pass and the paths behind
		 * them run too. The numbers stay garbage. */
		if (scribble)
			for (k = 0; k < fetched; k++)
				if (dst[k] && RKNPU_RC_REG(dst[k]) == RKNPU_RC_DPU_DST_ADDR) {
					struct fbo *o = find_range(x, RKNPU_RC_VALUE(dst[k]), 1);

					if (o)
						memset((unsigned char *)o->mem + (RKNPU_RC_VALUE(dst[k]) - o->addr), 0,
						       o->addr + o->size - RKNPU_RC_VALUE(dst[k]));
				}
		at += fetched * 8;
		n_entries += in->tasks[i].regcmd_count;
		if (in->tasks[i].regcmd_count > max_entries)
			max_entries = in->tasks[i].regcmd_count;
	}
	if (core_job[other].active) {
		n_overlap++;
		if (rknpu_rc_cbuf_collide(key, core_job[other].key)) {
			n_cbuf_wait++;
			core_job[other].active = 0;     /* the driver waits for it */
		}
	}
	retire(in->core);
	memset(j, 0, sizeof(*j));
	j->seq = ++seqno;
	j->owner = x;
	j->active = 1;
	j->tasks = in->task_count;
	j->key = key;
	n_submit++;
	n_tasks += in->task_count;
	if (in->task_count > max_tasks)
		max_tasks = in->task_count;
	if (at > max_arena)
		max_arena = at;
	memset(out, 0, sizeof(*out));
	out->seqno = j->seq;
	out->core = in->core;
	return 0;
}

static int do_wait1(struct rkx *x, const rknpu_wait_in *in, rknpu_wait_out *o);

static int do_wait(struct rkx *x, const rknpu_wait_in *in, unsigned inlen, rknpu_wait_out *o)
{
	unsigned allowed = RKNPU_WAIT_POLL | (m6 ? 0 : RKNPU_WAIT_PREP | RKNPU_WAIT_LEAN);
	unsigned prep = 0;
	struct fbo *b;
	int r;

	if (in->flags & ~allowed)
		return -EINVAL;
	if (in->flags & RKNPU_WAIT_PREP) {
		if (inlen < sizeof(rknpu_wait_m7_in) || (in->flags & RKNPU_WAIT_POLL))
			return -EINVAL;
		prep = ((const rknpu_wait_m7_in *)in)->prep_handle;
	}
	r = do_wait1(x, in, o);
	if (r || !(in->flags & RKNPU_WAIT_PREP) || o->result == RKNPU_JOB_RUNNING)
		return r;
	b = find(x, prep);
	if (!b)
		return -EINVAL;
	b->dirty = 1;           /* as PREP_BO: the CPU may write it from here */
	n_prep++;
	n_wprep++;
	o->m7_flags |= RKNPU_WAIT_OUT_PREPPED;
	return 0;
}

static int do_wait1(struct rkx *x, const rknpu_wait_in *in, rknpu_wait_out *o)
{
	unsigned c, h;

	memset(o, 0, sizeof(*o));
	o->seqno = in->seqno;
	n_wait++;
	for (c = 0; c < RKNPU_NUM_CORES; c++) {
		struct fjob *j = &core_job[c];

		if (j->seq != in->seqno || j->owner != x)
			continue;
		if (j->active && (in->flags & RKNPU_WAIT_POLL) && !j->polled) {
			j->polled = 1;
			n_poll++;
			o->result = RKNPU_JOB_RUNNING;
			return 0;
		}
		j->active = 0;
		o->result = RKNPU_JOB_DONE;
		o->core = c;
		o->tasks_done = o->task_count = j->tasks;
		return 0;
	}
	for (h = 0; h < RKNPU_JOB_HISTORY; h++)
		if (hist[h].seq == in->seqno && hist[h].owner == x) {
			o->result = RKNPU_JOB_DONE;
			o->tasks_done = o->task_count = hist[h].tasks;
			return 0;
		}
	o->result = RKNPU_JOB_UNKNOWN;
	return 0;
}

struct rkx *rkx_open(const char *path, int *err)
{
	struct rkx *x;

	(void)path;
	pthread_mutex_lock(&lk);
	init();
	pthread_mutex_unlock(&lk);
	if (getenv("RKNPU_FAKE_ABSENT")) {
		*err = -ENOENT;
		return NULL;
	}
	x = calloc(1, sizeof(*x));
	if (!x) {
		*err = -ENOMEM;
		return NULL;
	}
	x->open = 1;
	return x;
}

void rkx_close(struct rkx *x)
{
	unsigned i, c;

	pthread_mutex_lock(&lk);
	for (c = 0; c < RKNPU_NUM_CORES; c++)
		if (core_job[c].owner == x) {
			core_job[c].active = 0;
			core_job[c].owner = NULL;
		}
	for (i = 0; i < nbo;) {
		if (bos[i].owner == x) {
			total -= bos[i].size;
			free(bos[i].mem);
			bos[i] = bos[--nbo];
		} else {
			i++;
		}
	}
	pthread_mutex_unlock(&lk);
	free(x);
}

int rkx_ioctl(struct rkx *x, unsigned int code, const void *in, unsigned int inlen,
	      void *out, unsigned int outlen, unsigned int *got)
{
	int r = 0;

	*got = 0;
	pthread_mutex_lock(&lk);
	tick();
	switch (code) {
	case IOCTL_RKNPU_GET_INFO: {
		rknpu_info *i = out;

		if (outlen < sizeof(*i)) { r = -EINVAL; break; }
		memset(i, 0, sizeof(*i));
		i->interface_version = RKNPU_INTERFACE_VERSION;
		i->struct_size = sizeof(*i);
		i->flags = RKNPU_INFO_INTERRUPTS_CONNECTED | RKNPU_INFO_DOMAINS_ON;
		i->core_count = RKNPU_NUM_CORES;
		i->m3_version = RKNPU_M3_VERSION;
		i->m4_version = RKNPU_M4_VERSION;
		i->copy_arena_bytes = RKNPU_COPY_ARENA_BYTES;
		i->bo_max_kib = RKNPU_BO_MAX_SIZE >> 10;
		i->bo_max_total_kib = (m4 ? RKNPU_BO_MAX_TOTAL : RKNPU_BO_MAX_TOTAL_IOMMU) >> 10;
		if (!m4) {
			i->m6_version = RKNPU_M6_VERSION;
			i->mem_mode = RKNPU_MEM_IOMMU;
			i->bo_cached = 1;
		}
		if (!m6)
			i->m7_version = RKNPU_M7_VERSION;
		*got = sizeof(*i);
		break;
	}
	case IOCTL_RKNPU_CREATE_BO: {
		rknpu_create_bo req;
		unsigned size;
		void *mem;

		if (inlen < sizeof(req) || outlen < sizeof(req)) { r = -EINVAL; break; }
		memcpy(&req, in, sizeof(req));
		if (req.flags || !req.size || req.size > RKNPU_BO_MAX_SIZE) { r = -EINVAL; break; }
		size = (req.size + 4095u) & ~4095u;
		if (total + size > total_max) { r = -ENOSPC; break; }
		/* a guard page between buffers, so an off-by-one is outside both */
		if (next_addr + size + 4096 > 0x100000000ull) { r = -ENOMEM; break; }
		if (posix_memalign(&mem, 4096, size)) { r = -ENOMEM; break; }
		memset(mem, 0, size);
		if (nbo == capbo) {
			struct fbo *nb = realloc(bos, (capbo ? capbo * 2 : 256) * sizeof(*bos));

			if (!nb) { free(mem); r = -ENOMEM; break; }
			bos = nb;
			capbo = capbo ? capbo * 2 : 256;
		}
		bos[nbo].handle = ++next_handle;
		bos[nbo].addr = (unsigned)next_addr;
		bos[nbo].size = size;
		bos[nbo].mem = mem;
		bos[nbo].owner = x;
		bos[nbo].mapped = 0;
		next_addr += size + 4096;
		total += size;
		if (total > total_peak)
			total_peak = total;
		if (++nbo > nbo_peak)
			nbo_peak = nbo;
		if (size > bo_max_seen)
			bo_max_seen = size;
		req.size = size;
		req.flags = m4 ? 0 : RKNPU_BO_CACHED;
		req.handle = bos[nbo - 1].handle;
		req.dma_address = bos[nbo - 1].addr;
		memcpy(out, &req, sizeof(req));
		*got = sizeof(req);
		break;
	}
	case IOCTL_RKNPU_MAP_BO: {
		rknpu_map_bo m;
		struct fbo *b;

		if (inlen < sizeof(m) || outlen < sizeof(m)) { r = -EINVAL; break; }
		memcpy(&m, in, sizeof(m));
		b = find(x, m.handle);
		if (!b) { r = -EINVAL; break; }
		if (b->mapped) { r = -EIO; break; }
		b->mapped = 1;
		b->dirty = !m4;
		m.user_va = (unsigned long long)(uintptr_t)b->mem;
		memcpy(out, &m, sizeof(m));
		*got = sizeof(m);
		break;
	}
	case IOCTL_RKNPU_FREE_BO: {
		rknpu_bo_handle h;
		struct fbo *b;
		unsigned c;

		if (inlen < sizeof(h)) { r = -EINVAL; break; }
		memcpy(&h, in, sizeof(h));
		b = find(x, h.handle);
		if (!b) { r = -EINVAL; break; }
		for (c = 0; c < RKNPU_NUM_CORES; c++)
			if (core_job[c].active && core_job[c].owner == x)
				r = -EBUSY;
		if (r)
			break;
		total -= b->size;
		free(b->mem);
		*b = bos[--nbo];
		break;
	}
	case IOCTL_RKNPU_PREP_BO:
	case IOCTL_RKNPU_FINI_BO: {
		rknpu_bo_sync bs;
		struct fbo *b;

		if (m4 || inlen < sizeof(bs)) { r = -EINVAL; break; }
		memcpy(&bs, in, sizeof(bs));
		b = find(x, bs.handle);
		if (!b || bs.flags || (unsigned long long)bs.offset + bs.length > b->size) { r = -EINVAL; break; }
		if (code == IOCTL_RKNPU_PREP_BO) {
			n_prep++;
			b->dirty = 1;
		} else {
			n_fini++;
			b->dirty = 0;
		}
		break;
	}
	case IOCTL_RKNPU_SUBMIT:
		if (inlen < sizeof(rknpu_submit_in) || outlen < sizeof(rknpu_submit_out)) {
			r = -EINVAL;
			break;
		}
		r = do_submit(x, in, inlen, out);
		if (!r)
			*got = sizeof(rknpu_submit_out);
		break;
	case IOCTL_RKNPU_WAIT:
		if (inlen < sizeof(rknpu_wait_in) || outlen < sizeof(rknpu_wait_out)) {
			r = -EINVAL;
			break;
		}
		r = do_wait(x, in, inlen, out);
		if (!r)
			*got = sizeof(rknpu_wait_out);
		break;
	default:
		r = -EINVAL;
	}
	pthread_mutex_unlock(&lk);
	return r;
}

void rkx_pause(unsigned int us)
{
	struct timespec ts = { 0, (long)us * 1000 };

	if (us)
		nanosleep(&ts, NULL);
	else
		sched_yield();
}

void rkx_relax(void)
{
	sched_yield();
}

unsigned long long rkx_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000;
}

void rkx_wmb(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

const char *rkx_mem_report(void)
{
	return "no memory counters on the host";
}

void charsiu_port_crash_handler_install(void)
{
}

/* M7: the host has no NPU to be coherent with; RKNPU_FAKE_UCACHE=1 says
 * user mode may do the maintenance, which is then a fence */
int rkx_dc_usable(void)
{
	return getenv("RKNPU_FAKE_UCACHE") != NULL;
}

/* the buffer whose mapping p is, marked as its FINI_BO (clean) or PREP_BO
 * (dirty from here) would mark it */
static void fake_dc(const void *p, size_t n, int dirty)
{
	unsigned i;

	(void)n;
	pthread_mutex_lock(&lk);
	for (i = 0; i < nbo; i++)
		if (bos[i].mem == p) {
			bos[i].dirty = dirty;
			if (dirty)
				n_uprep++;
			else
				n_ufini++;
			break;
		}
	pthread_mutex_unlock(&lk);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void rkx_dc_clean(const void *p, size_t n)
{
	fake_dc(p, n, 0);
}

void rkx_dc_clean_inval(const void *p, size_t n)
{
	fake_dc(p, n, 1);
}

/* for port/test/devtest.c (M7) */
void rkx_fake_m7_counts(unsigned long long *cleans, unsigned long long *wait_preps,
			unsigned long long *ufinis, unsigned long long *upreps)
{
	pthread_mutex_lock(&lk);
	*cleans = n_clean;
	*wait_preps = n_wprep;
	*ufinis = n_ufini;
	*upreps = n_uprep;
	pthread_mutex_unlock(&lk);
}

/* for port/test/devtest.c (M6) */
void rkx_fake_cbuf_counts(unsigned long long *overlaps, unsigned long long *waits)
{
	pthread_mutex_lock(&lk);
	*overlaps = n_overlap;
	*waits = n_cbuf_wait;
	pthread_mutex_unlock(&lk);
}

void rkx_fake_sync_counts(unsigned long long *preps, unsigned long long *finis,
			  unsigned long long *unfinished)
{
	pthread_mutex_lock(&lk);
	*preps = n_prep;
	*finis = n_fini;
	*unfinished = n_unfini;
	pthread_mutex_unlock(&lk);
}

/* for port/test/devtest.c */
void rkx_fake_counts(unsigned long long *submits, unsigned long long *busy,
		     unsigned long long *refused, unsigned long long *maxt)
{
	pthread_mutex_lock(&lk);
	*submits = n_submit;
	*busy = n_busy;
	*refused = n_refused;
	*maxt = max_tasks;
	pthread_mutex_unlock(&lk);
}

static int scan_cmp(const void *a, const void *b)
{
	unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;

	return x < y ? -1 : x > y;
}

/* "0xTTTT 0xRRRR count" per (block, register), sorted */
static void dump(const struct slot *t, const char *path)
{
	static unsigned keys[SCAN_SLOTS];
	unsigned i, n = 0;
	FILE *f = fopen(path, "w");

	if (!f)
		return;
	for (i = 0; i < SCAN_SLOTS; i++)
		if (t[i].n)
			keys[n++] = t[i].key;
	qsort(keys, n, sizeof(keys[0]), scan_cmp);
	for (i = 0; i < n; i++) {
		unsigned h = (keys[i] * 2654435761u) % SCAN_SLOTS;

		while (t[h].key != keys[i] || !t[h].n)
			h = (h + 1) % SCAN_SLOTS;
		fprintf(f, "0x%04x 0x%04x %llu\n", keys[i] >> 16, keys[i] & 0xFFFF, t[h].n);
	}
	fclose(f);
}

static void report(void)
{
	if (scan_path)
		dump(scan, scan_path);
	if (regs_path)
		dump(regs, regs_path);
	if (getenv("RKNPU_FAKE_REPORT"))
		fprintf(stderr, "rknpu-fake: %llu SUBMITs, %llu tasks (max %llu a SUBMIT), "
			"%llu entries (max %llu a stream), arena peak %llu bytes, %llu busy, "
			"%llu refused, %llu waits (%llu polled RUNNING); buffers peak %llu "
			"(%.1f MiB), largest %.1f MiB; %llu PREP_BO, %llu FINI_BO, %llu jobs "
			"from unflushed streams; %llu SUBMITs overlapped the other core's job, "
			"%llu of them on the same CBUF window (waited); M7: %llu FINIs by SUBMIT, "
			"%llu PREPs by WAIT\n",
			n_submit, n_tasks, max_tasks, n_entries, max_entries, max_arena, n_busy,
			n_refused, n_wait, n_poll, nbo_peak, total_peak / 1048576.0,
			bo_max_seen / 1048576.0, n_prep, n_fini, n_unfini, n_overlap, n_cbuf_wait,
			n_clean, n_wprep);
}
