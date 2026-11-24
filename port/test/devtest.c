/*
 * devtest.c - device_rknpu.c against the in-memory driver (xport_fake.c)
 *
 * Runs on a Linux host in CI. Real charsiu streams (charsiu_emit_job, the
 * int8 matmul and the w4a16 one) in real charsiu buffers, through the same
 * device layer the Windows build uses, and checks what the driver's rules
 * say about them:
 *
 *   - a job of 100 tasks goes as two SUBMITs (64 + 36), each after the last;
 *   - streams too long for one arena together are split by bytes as well;
 *   - a third device, whose preferred core is busy, takes the other core;
 *   - a job whose core was taken by another device still answers its WAIT;
 *   - a stream with an address outside the device's buffers, one that
 *     writes the PC block and one that writes a register charsiu never
 *     writes are each refused, and the device keeps working afterwards;
 *   - a buffer of another device is outside this one's buffers;
 *   - M7: FINI_BO rides on the next SUBMIT and PREP_BO on the WAIT (the
 *     model refuses a stream whose clean never arrived), or, with
 *     CHARSIU_RKNPU_UCACHE=1 and RKNPU_FAKE_UCACHE=1, both happen in the
 *     caller; RKNPU_FAKE_M6=1 is the M6 driver, one IOCTL each;
 *   - M7, CHARSIU_RKNPU_ASYNC=1: the same work through the SUBMIT threads,
 *     and a refused stream fails the next PREP and every later SUBMIT
 *     (the busy-core, refusal and shared-CBUF steps above need a
 *     synchronous SUBMIT and are skipped).
 *
 * Nothing computes; see xport_fake.c.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "charsiu.h"
#include "rknpu_ioctl.h"
#include "rknpu_regcmd.h"

/* xport_fake.c */
void rkx_fake_counts(unsigned long long *submits, unsigned long long *busy,
		     unsigned long long *refused, unsigned long long *max_tasks);
void rkx_fake_sync_counts(unsigned long long *preps, unsigned long long *finis,
			  unsigned long long *unfinished);
void rkx_fake_cbuf_counts(unsigned long long *overlaps, unsigned long long *waits);
void rkx_fake_m7_counts(unsigned long long *cleans, unsigned long long *wait_preps,
			unsigned long long *ufinis, unsigned long long *upreps);

static int failures;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

struct mm {
	struct charsiu_bo in, wt, out, coef, reg;
	struct charsiu_job job;
	unsigned nreg;
};

/* one matmul's buffers and stream, on dev; streams[] gets `copies` of it */
static int mm_init(struct charsiu_device *dev, struct mm *m, enum charsiu_dtype w, enum charsiu_dtype a,
		   unsigned copies)
{
	size_t room = 4096u * copies;
	unsigned c;

	memset(m, 0, sizeof(*m));
	m->job.mm.m = 1;
	m->job.mm.k = 256;
	m->job.mm.n = 64;
	m->job.mm.wdtype = w;
	m->job.mm.adtype = a;
	m->job.input_zero_point = a == CHARSIU_INT8 ? 128 : 0;
	m->job.weight_zero_point = 128;
	m->job.input_scale = m->job.weight_scale = m->job.output_scale = 1.0f;
	m->job.acc_out = 1;
	if (charsiu_bo_alloc(dev, 65536, &m->in) || charsiu_bo_alloc(dev, charsiu_weight_bytes(&m->job.mm) + 4096, &m->wt) ||
	    charsiu_bo_alloc(dev, 65536, &m->out) || charsiu_bo_alloc(dev, charsiu_coef_bytes(&m->job.mm) + 4096, &m->coef) ||
	    charsiu_bo_alloc(dev, room, &m->reg))
		return -1;
	m->job.input_addr = (uint32_t)m->in.dma_address;
	m->job.weight_addr = (uint32_t)m->wt.dma_address;
	m->job.output_addr = (uint32_t)m->out.dma_address;
	m->job.coef_addr = (uint32_t)m->coef.dma_address;
	for (c = 0; c < copies; c++) {
		size_t n = charsiu_emit_job(&m->job, (uint64_t *)((char *)m->reg.map + 4096u * c), 4096 / 8);

		if (!n)
			return -1;
		m->nreg = (unsigned)n;
	}
	return charsiu_bo_fini(dev, &m->reg);
}

static void mm_free(struct charsiu_device *dev, struct mm *m)
{
	charsiu_bo_free(dev, &m->in);
	charsiu_bo_free(dev, &m->wt);
	charsiu_bo_free(dev, &m->out);
	charsiu_bo_free(dev, &m->coef);
	charsiu_bo_free(dev, &m->reg);
}

static int submit_n(struct charsiu_device *dev, struct mm *m, unsigned ntasks, unsigned stride, unsigned count)
{
	struct charsiu_task *t = calloc(ntasks, sizeof(*t));
	struct charsiu_joblist jl;
	unsigned i;
	int r;

	for (i = 0; i < ntasks; i++) {
		t[i].regcmd = (uint32_t)m->reg.dma_address + (i % (unsigned)(m->reg.size / stride)) * stride;
		t[i].regcmd_count = count ? count : m->nreg;
	}
	memset(&jl, 0, sizeof(jl));
	jl.tasks = t;
	jl.task_count = ntasks;
	r = charsiu_submit_jobs(dev, &jl, 1);
	free(t);
	return r;
}

/* entry i of the first stream, rewritten, and (M6) flushed as charsiu does */
static void poke(struct charsiu_device *dev, struct mm *m, unsigned i, uint64_t e)
{
	((uint64_t *)m->reg.map)[i] = e;
	charsiu_bo_fini(dev, &m->reg);
}

static unsigned find_reg(struct mm *m, unsigned reg)
{
	unsigned i;

	for (i = 0; i < m->nreg; i++)
		if (RKNPU_RC_REG(((uint64_t *)m->reg.map)[i]) == reg)
			return i;
	return 0;
}

int main(void)
{
	struct charsiu_device *d0, *d1, *d2;
	struct mm a, b, c, big;
	unsigned long long s0, s1, busy0, busy1, ref0, ref1, maxt;
	uint64_t saved;
	unsigned i;
	int r;
	int async = getenv("CHARSIU_RKNPU_ASYNC") && atoi(getenv("CHARSIU_RKNPU_ASYNC"));

	d0 = charsiu_open(NULL);
	d1 = charsiu_open(NULL);
	d2 = charsiu_open(NULL);
	CHECK(d0 && d1 && d2, "three devices open");
	if (!d0 || !d1 || !d2)
		return 1;
	CHECK(!mm_init(d0, &a, CHARSIU_INT8, CHARSIU_INT8, 4), "int8 matmul on device 0");
	CHECK(!mm_init(d1, &b, CHARSIU_INT4, CHARSIU_FP16, 4), "w4a16 matmul on device 1");
	CHECK(!mm_init(d2, &c, CHARSIU_FP16, CHARSIU_FP16, 4), "fp16 matmul on device 2");

	/* 100 tasks: 64 + 36 */
	rkx_fake_counts(&s0, &busy0, &ref0, &maxt);
	r = submit_n(d0, &a, 100, 4096, 0);
	CHECK(r == 0, "100 tasks submit: %d", r);
	CHECK(charsiu_bo_prep(d0, &a.out, 1000000000) == 0, "wait for them");
	rkx_fake_counts(&s1, &busy1, &ref1, &maxt);
	CHECK(s1 - s0 == 2 && maxt == RKNPU_MAX_TASKS, "100 tasks went as %llu SUBMITs, max %llu tasks", s1 - s0, maxt);

	/* streams of 4096 entries (32 KiB): eight fill an arena, so 20 go as 3 */
	/* 64 copies of one stream, 4 KiB apart: a 4096-entry task is eight of
	 * them and their zero padding, which passes the check */
	CHECK(!mm_init(d0, &big, CHARSIU_INT8, CHARSIU_INT8, 64), "a 256 KiB stream buffer");
	rkx_fake_counts(&s0, &busy0, &ref0, &maxt);
	r = submit_n(d0, &big, 20, 32768, 4096);
	CHECK(r == 0, "20 long streams: %d", r);
	charsiu_bo_prep(d0, &big.out, 1000000000);
	rkx_fake_counts(&s1, &busy1, &ref1, &maxt);
	CHECK(s1 - s0 == 3, "20 streams of 32 KiB went as %llu SUBMITs (want 3)", s1 - s0);
	mm_free(d0, &big);

	/* device 2 prefers core 0, which device 0 holds: it takes core 1 */
	if (!async) {
	rkx_fake_counts(&s0, &busy0, &ref0, &maxt);
	CHECK(submit_n(d0, &a, 1, 4096, 0) == 0, "device 0 on core 0");
	CHECK(submit_n(d2, &c, 1, 4096, 0) == 0, "device 2 while core 0 is busy");
	rkx_fake_counts(&s1, &busy1, &ref1, &maxt);
	CHECK(busy1 > busy0, "device 2 met the busy core (%llu)", busy1 - busy0);
	CHECK(charsiu_bo_prep(d2, &c.out, 1000000000) == 0, "device 2's job");
	/* device 2 again, on core 0 once device 0's job is done: device 0's
	 * WAIT then finds its job in the history */
	CHECK(submit_n(d2, &c, 1, 4096, 0) == 0, "device 2 again");
	CHECK(charsiu_bo_prep(d0, &a.out, 1000000000) == 0, "device 0's WAIT after its core was retaken");
	CHECK(charsiu_bo_prep(d2, &c.out, 1000000000) == 0, "device 2's second job");
	}

	/* M6, the shared CBUF: device 0 on window 0 and device 1 on window 1
	 * overlap without a wait; the same window waits (counted at SUBMIT,
	 * so not with CHARSIU_RKNPU_ASYNC) */
	if (!async) {
		struct mm w1;
		unsigned long long ov0, wt0, ov1, wt1;

		CHECK(!mm_init(d1, &w1, CHARSIU_INT8, CHARSIU_INT8, 1), "a window 1 job");
		w1.job.cbuf_window = 1;
		charsiu_emit_job(&w1.job, (uint64_t *)w1.reg.map, 4096 / 8);
		charsiu_bo_fini(d1, &w1.reg);
		rkx_fake_cbuf_counts(&ov0, &wt0);
		CHECK(submit_n(d0, &a, 1, 4096, 0) == 0, "window 0 on device 0");
		CHECK(submit_n(d1, &w1, 1, 4096, 0) == 0, "window 1 on device 1, at once");
		rkx_fake_cbuf_counts(&ov1, &wt1);
		CHECK(ov1 == ov0 + 1 && wt1 == wt0, "two windows overlap with no wait (%llu, %llu)",
		      ov1 - ov0, wt1 - wt0);
		charsiu_bo_prep(d0, &a.out, 1000000000);
		charsiu_bo_prep(d1, &w1.out, 1000000000);
		CHECK(submit_n(d0, &a, 1, 4096, 0) == 0, "window 0 on device 0 again");
		/* device 1's w4a16 job is window 0 too (charsiu_emit_job's default) */
		CHECK(submit_n(d1, &b, 1, 4096, 0) == 0, "window 0 on device 1, at once");
		rkx_fake_cbuf_counts(&ov0, &wt0);
		CHECK(wt0 == wt1 + 1, "the same window waited (%llu)", wt0 - wt1);
		charsiu_bo_prep(d0, &a.out, 1000000000);
		charsiu_bo_prep(d1, &b.out, 1000000000);
		mm_free(d1, &w1);
	}

	/* device 1's w4a16 stream, the default for Llama */
	CHECK(submit_n(d1, &b, 16, 4096, 0) == 0, "w4a16 tasks");
	CHECK(charsiu_bo_prep(d1, &b.out, 1000000000) == 0, "w4a16 wait");

	/* refusals: an address outside the device's buffers ... */
	i = find_reg(&a, RKNPU_RC_DPU_DST_ADDR);
	saved = ((uint64_t *)a.reg.map)[i];
	CHECK(i, "the stream has a DPU destination");
	if (!async) {
	poke(d0, &a, i, RKNPU_RC_ENTRY(RKNPU_RC_DPU, RKNPU_RC_DPU_DST_ADDR, 0xF0000000u));
	rkx_fake_counts(&s0, &busy0, &ref0, &maxt);
	CHECK(submit_n(d0, &a, 1, 4096, 0) == -EACCES, "output outside every buffer is refused");
	/* ... another device's buffer ... */
	poke(d0, &a, i, RKNPU_RC_ENTRY(RKNPU_RC_DPU, RKNPU_RC_DPU_DST_ADDR, (uint32_t)b.out.dma_address));
	CHECK(submit_n(d0, &a, 1, 4096, 0) == -EACCES, "output in device 1's buffer is refused");
	/* ... the PC block ... */
	poke(d0, &a, i, RKNPU_RC_ENTRY(0x0101u, 0x0010u, (uint32_t)a.reg.dma_address));
	CHECK(submit_n(d0, &a, 1, 4096, 0) == -EACCES, "a PC_BASE_ADDRESS write is refused");
	/* ... and the IOMMU, a register charsiu never writes */
	poke(d0, &a, i, RKNPU_RC_ENTRY(0x0201u, 0x2000u, 0));
	CHECK(submit_n(d0, &a, 1, 4096, 0) == -EACCES, "an IOMMU register write is refused");
	rkx_fake_counts(&s1, &busy1, &ref1, &maxt);
	CHECK(ref1 - ref0 == 4 && s1 == s0, "4 refused, none started (%llu, %llu)", ref1 - ref0, s1 - s0);
	/* and the device still works */
	poke(d0, &a, i, saved);
	CHECK(submit_n(d0, &a, 2, 4096, 0) == 0, "after the refusals");
	CHECK(charsiu_bo_prep(d0, &a.out, 1000000000) == 0, "after the refusals, wait");
	}

	/* M6: cached buffers. charsiu's own prep/fini reached the driver ... */
	{
		unsigned long long p0, f0, u0, p1, f1, u1;

		unsigned long long cl, wp, uf, up;
		int m6 = getenv("RKNPU_FAKE_M6") || getenv("RKNPU_FAKE_M4");
		int uc = getenv("RKNPU_FAKE_UCACHE") && getenv("CHARSIU_RKNPU_UCACHE") &&
			 atoi(getenv("CHARSIU_RKNPU_UCACHE"));
		int fuse = !m6 && !uc && !(getenv("CHARSIU_RKNPU_FUSE") &&
					   !atoi(getenv("CHARSIU_RKNPU_FUSE")));

		rkx_fake_sync_counts(&p0, &f0, &u0);
		rkx_fake_m7_counts(&cl, &wp, &uf, &up);
		CHECK(getenv("RKNPU_FAKE_M4") || (p0 + up > 0 && f0 + cl + uf > 0 && u0 == 0),
		      "PREP_BO %llu, FINI_BO %llu, unflushed jobs %llu", p0, f0, u0);
		/* M7: which path did the work */
		if (fuse)
			CHECK(cl > 0 && wp > 0 && wp <= p0 && uf == 0 && up == 0,
			      "fused: %llu FINIs by SUBMIT, %llu PREPs by WAIT, %llu FINI_BO IOCTLs",
			      cl, wp, f0);
		else if (uc && !getenv("RKNPU_FAKE_M4"))
			CHECK(uf > 0 && up > 0 && cl == 0 && wp == 0 && f0 == 0,
			      "in the caller: %llu cleans, %llu invalidates (%llu, %llu by the driver)",
			      uf, up, cl, wp);
		else if (!getenv("RKNPU_FAKE_M4"))
			CHECK(cl == 0 && wp == 0 && f0 > 0, "M6: %llu FINI_BO, %llu cleans by SUBMIT",
			      f0, cl);
		/* ... and a stream written after its FINI is caught by the model */
		((uint64_t *)a.reg.map)[0] = ((uint64_t *)a.reg.map)[0];
		charsiu_bo_prep(d0, &a.reg, 1000000000);
		r = submit_n(d0, &a, 1, 4096, 0);
		if (async && !r)
			r = charsiu_bo_prep(d0, &a.out, 1000000000);
		rkx_fake_sync_counts(&p1, &f1, &u1);
		CHECK(getenv("RKNPU_FAKE_M4") ? r == 0 : (r == -EIO && u1 == u0 + 1),
		      "a stream not flushed since the CPU touched it: %d", r);
		if (!async)
			charsiu_bo_prep(d0, &a.out, 1000000000);
	}

	/* M7 async: a refused stream fails the next PREP, then every SUBMIT
	 * (device 2, which nothing else uses from here) */
	if (async) {
		unsigned j = find_reg(&c, RKNPU_RC_DPU_DST_ADDR);

		poke(d2, &c, j, RKNPU_RC_ENTRY(RKNPU_RC_DPU, RKNPU_RC_DPU_DST_ADDR, 0xF0000000u));
		CHECK(submit_n(d2, &c, 1, 4096, 0) == 0, "async: the refused SUBMIT is accepted for now");
		r = charsiu_bo_prep(d2, &c.out, 1000000000);
		CHECK(r == -EACCES, "async: the PREP after it fails (%d)", r);
		r = submit_n(d2, &c, 1, 4096, 0);
		CHECK(r == -EACCES, "async: and so does the next SUBMIT (%d)", r);
	}

	mm_free(d0, &a);
	mm_free(d1, &b);
	mm_free(d2, &c);
	charsiu_close(d2);
	charsiu_close(d1);
	charsiu_close(d0);
	printf("%s\n", failures ? "DEVTEST FAILED" : "DEVTEST OK");
	return failures != 0;
}
