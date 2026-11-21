// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Jiaxing Hu <gahing@gahingwoo.com>
/*
 * M0 part 2: one GEMM on NPU core 0 with its IOMMU NOT translating, compared
 * against the CPU to the last bit.
 *
 * The Windows driver's first phases (M1-M5) assume the NPU's DMA reaches
 * physical memory untranslated when its IOMMU has paging off. Mainline Linux
 * always runs it with paging on, so nobody has measured that. This does, from
 * Linux, before any Windows code depends on it.
 *
 * HOW. rocket stays bound and loaded; it is only kept idle. m0_snapshot.sh's
 * pinning (power/control = on) keeps core 0's domains and clocks up. This
 * program then
 *   1. refuses unless: root, core 0 runtime-active with power/control = on,
 *      npu/nputop/npu0 on in genpd, no process holding /dev/accel/accel*
 *      open, and core 0 idle (OPERATION_ENABLE = 0);
 *   2. takes buffers from mlocked anonymous memory (a 2 MiB huge page when
 *      there is one) and reads their physical addresses from
 *      /proc/self/pagemap; every buffer must be physically contiguous and
 *      below 4 GiB, because a register stream carries 32-bit addresses;
 *   3. builds the job with charsiu's own emitter, packers and coefficient
 *      builder (the same calls as charsiu's tools/npu_gemm_test.c, m = 1),
 *      with physical addresses in place of IOVAs;
 *   4. cleans the buffers out of the CPU caches (DC CIVAC: the NPU is not
 *      coherent), submits with rocket_job_hw_submit()'s register sequence
 *      and polls for completion with the interrupt masked;
 *   5. invalidates, compares against the CPU, and checks that nothing in the
 *      buffers outside the expected output changed (a canary pattern).
 *
 * WHAT IT WRITES, all of it on NPU core 0, all through /dev/mem:
 *   PC  0x27700000 + 0x10 BASE_ADDRESS, 0x14 REGISTER_AMOUNTS,
 *                    0x20 INTERRUPT_MASK (0: no interrupt is raised),
 *                    0x24 INTERRUPT_CLEAR, 0x30 TASK_CON,
 *                    0x34 TASK_DMA_BASE_ADDR, 0x08 OPERATION_ENABLE
 *   CNA 0x27701004 S_POINTER, CORE 0x27703004 S_POINTER
 * These are exactly the registers rocket writes on every submit
 * (rocket_job_hw_submit(), drivers/accel/rocket/rocket_job.c) and on every
 * completion (OPERATION_ENABLE = 0, INTERRUPT_CLEAR = 0x1ffff), so rocket's
 * next job overwrites every one of them: nothing is left for rocket to
 * restore. The interrupt stays masked, so rocket's IRQ handler never sees
 * this job.
 * Only with --allow-mmu-writes, and only if paging is ON when it starts:
 *   MMU 0x27702000 and 0x27702100 + 0x08 COMMAND: stall, disable paging,
 *   unstall; afterwards DTE_ADDR and INT_MASK written back with the values
 *   read before, zap, enable paging, unstall, and both read back.
 * It never writes the PMU, the CRU, a GRF, a QoS register, the PMIC, core 1,
 * or GLOBAL_OPERATION_ENABLE (0xf008, which reaches core 1 from core 0's
 * window). No clock rate, no voltage, no reset changes.
 *
 * WHY IT IS SAFE. The NPU writes only where the register stream says, and
 * the output address is one of our own pinned pages. The DMA cannot reach
 * anything else unless the stream is wrong, which is what the canary is for.
 * The one real hazard is a register read with core 0's domain or clocks off
 * (an SError), and step 1 refuses that state.
 *
 * WHEN TO REBOOT: if it prints REBOOT. That is a job that did not finish
 * (core 0 may be wedged until a reset), an IOMMU that did not take its old
 * state back, or a canary that changed (memory outside our buffers may have
 * been written).
 *
 * Usage: m0_bypass [--k K] [--n N] [--allow-mmu-writes] [--dry-run]
 *   --dry-run: everything except the register writes and the submit.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "charsiu.h"

/* rk3576.dtsi as the NPU series adds it (rfc-send-v17/npu v17-0011) */
#define NPU0_BASE        0x27700000UL
#define NPU0_SPAN        0x10000UL
#define CNA_OFF          0x1000
#define CORE_OFF         0x3000
#define MMU0_OFF         0x2000
#define MMU1_OFF         0x2100

/* rocket_registers.h */
#define PC_VERSION                 0x0000
#define PC_VERSION_NUM             0x0004
#define PC_OPERATION_ENABLE        0x0008
#define PC_BASE_ADDRESS            0x0010
#define PC_REGISTER_AMOUNTS        0x0014
#define PC_INTERRUPT_MASK          0x0020
#define PC_INTERRUPT_CLEAR         0x0024
#define PC_INTERRUPT_RAW_STATUS    0x002c
#define PC_TASK_CON                0x0030
#define PC_TASK_DMA_BASE_ADDR      0x0034
#define PC_TASK_STATUS             0x003c
#define RAW_DPU_0                  0x00000100
#define RAW_DPU_1                  0x00000200
#define RAW_DMA_READ_ERROR         0x00001000
#define RAW_DMA_WRITE_ERROR        0x00002000
#define S_POINTER                  0x0004       /* in the CNA and CORE windows */
#define S_POINTER_PP_EN            (1u << 1)
#define S_POINTER_EXECUTER_PP_EN   (1u << 2)
#define S_POINTER_PP_MODE          (1u << 3)
/* v17-0009: the RK3576 PC_TASK_CON layout */
#define RK3576_TASK_NUMBER(n)      ((n) & 0xffff)
#define RK3576_TASK_PP_EN          (1u << 16)
#define RK3576_TASK_COUNT_CLEAR    (1u << 17)
#define RK3576_TASK_LAST_LAYER_CLR (1u << 18)

/* drivers/iommu/rockchip-iommu.c */
#define RK_MMU_DTE_ADDR            0x00
#define RK_MMU_STATUS              0x04
#define RK_MMU_COMMAND             0x08
#define RK_MMU_INT_MASK            0x1c
#define RK_MMU_STATUS_PAGING       (1u << 0)
#define RK_MMU_STATUS_STALL_ACTIVE (1u << 2)
#define RK_MMU_CMD_ENABLE_PAGING   0
#define RK_MMU_CMD_DISABLE_PAGING  1
#define RK_MMU_CMD_ENABLE_STALL    2
#define RK_MMU_CMD_DISABLE_STALL   3
#define RK_MMU_CMD_ZAP_CACHE       4

#define SYS_CORE0 "/sys/bus/platform/devices/27700000.npu"
#define GENPD     "/sys/kernel/debug/pm_genpd/pm_genpd_summary"

#define POOL_HUGE  (2u << 20)
#define POOL_SMALL (4u << 20)
#define CANARY     0xA5u

static volatile uint32_t *npu;
static int dry_run;

static uint32_t rd(unsigned off) { return npu[off / 4]; }

static void wr(unsigned off, uint32_t v)
{
	printf("  write 0x%08lx = 0x%08x\n", NPU0_BASE + off, v);
	if (!dry_run)
		npu[off / 4] = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static int read_line(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	if (!fgets(buf, (int)len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

static int genpd_on(const char *dom)
{
	FILE *f = fopen(GENPD, "r");
	char line[256], name[64], state[64];
	int on = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%63s %63s", name, state) == 2 &&
		    !strcmp(name, dom) && !strncmp(state, "on", 2))
			on = 1;
	fclose(f);
	return on;
}

/* Is any process holding the NPU's accel node open? Then rocket may run a
 * job on core 0 while we do, and both would be wrong. */
static int accel_in_use(void)
{
	DIR *proc = opendir("/proc");
	struct dirent *p;
	int found = 0;

	if (!proc)
		return -1;
	while ((p = readdir(proc))) {
		char dir[300], link[600], target[300];
		DIR *fds;
		struct dirent *fd;

		if (p->d_name[0] < '0' || p->d_name[0] > '9')
			continue;
		snprintf(dir, sizeof(dir), "/proc/%s/fd", p->d_name);
		fds = opendir(dir);
		if (!fds)
			continue;
		while ((fd = readdir(fds))) {
			ssize_t n;

			snprintf(link, sizeof(link), "%s/%s", dir, fd->d_name);
			n = readlink(link, target, sizeof(target) - 1);
			if (n <= 0)
				continue;
			target[n] = '\0';
			if (!strncmp(target, "/dev/accel/accel", 16)) {
				printf("  pid %s holds %s\n", p->d_name, target);
				found = 1;
			}
		}
		closedir(fds);
	}
	closedir(proc);
	return found;
}

/* ------------------------------------------------------------ memory -- */

struct pool {
	uint8_t *va;
	size_t size;
	uint64_t *pa;        /* per 4 KiB page */
	size_t used;
};

static uint64_t pagemap_pa(int pm, const void *va)
{
	uint64_t e;
	off_t off = (off_t)((uintptr_t)va / 4096 * 8);

	if (pread(pm, &e, 8, off) != 8 || !(e & (1ull << 63)))
		return 0;
	return (e & ((1ull << 55) - 1)) * 4096;
}

/*
 * The buffers for K = 256, N = 64 come to about 310 KiB, the coefficient
 * table alone 257 KiB, so in practice this needs a huge page: anonymous 4 KiB
 * pages are almost never physically contiguous over 66 pages. A huge page can
 * still sit above 4 GiB; up to 16 are tried and the first one below is kept.
 */
static int pool_init(struct pool *p)
{
	uint8_t *tried[16];
	int pm, huge = 0, ntried = 0;
	size_t pages, i;

	pm = open("/proc/self/pagemap", O_RDONLY);
	if (pm < 0) {
		perror("/proc/self/pagemap");
		return -1;
	}
	p->size = POOL_HUGE;
	while (ntried < 16) {
		uint8_t *va = mmap(NULL, POOL_HUGE, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
		uint64_t pa;

		if (va == MAP_FAILED)
			break;
		va[0] = CANARY;
		pa = pagemap_pa(pm, va);
		printf("  huge page at PA 0x%" PRIx64 "%s\n", pa,
		       pa && pa + POOL_HUGE <= 0x100000000ull ? "" : " (not below 4 GiB)");
		if (pa && pa + POOL_HUGE <= 0x100000000ull) {
			p->va = va;
			huge = 1;
			break;
		}
		tried[ntried++] = va;
	}
	for (i = 0; i < (size_t)ntried; i++)
		munmap(tried[i], POOL_HUGE);
	if (!huge) {
		printf("  no huge page below 4 GiB (%d tried; nr_hugepages set? mem=2G?), trying 4 KiB pages\n",
		       ntried);
		p->size = POOL_SMALL;
		p->va = mmap(NULL, p->size, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
		if (p->va == MAP_FAILED) {
			perror("mmap");
			return -1;
		}
	}
	if (mlock(p->va, p->size)) {
		perror("mlock");
		return -1;
	}
	memset(p->va, CANARY, p->size);

	pages = p->size / 4096;
	p->pa = calloc(pages, sizeof(*p->pa));
	if (!p->pa)
		return -1;
	for (i = 0; i < pages; i++) {
		p->pa[i] = pagemap_pa(pm, p->va + i * 4096);
		if (!p->pa[i]) {
			fprintf(stderr, "pagemap: no PFN for page %zu (not root?)\n", i);
			return -1;
		}
	}
	close(pm);
	printf("  memory: %s, %zu KiB, first page at PA 0x%" PRIx64 "\n",
	       huge ? "one 2 MiB huge page" : "4 KiB pages", p->size / 1024, p->pa[0]);
	return 0;
}

/* A 4 KiB aligned, physically contiguous run below 4 GiB, from the pool. */
static int pool_take(struct pool *p, size_t size, uint8_t **va, uint32_t *pa)
{
	size_t need = (size + 4095) / 4096, start, i;

	for (start = p->used / 4096; start + need <= p->size / 4096; start++) {
		for (i = 0; i < need; i++)
			if (p->pa[start + i] != p->pa[start] + i * 4096 ||
			    p->pa[start + i] + 4096 > 0x100000000ull)
				break;
		if (i == need) {
			*va = p->va + start * 4096;
			*pa = (uint32_t)p->pa[start];
			p->used = (start + need) * 4096;
			return 0;
		}
	}
	fprintf(stderr, "no physically contiguous run of %zu pages below 4 GiB in the pool\n"
		"  (reserve huge pages: echo 16 > /proc/sys/vm/nr_hugepages; on a board with RAM\n"
		"  above 4 GiB also boot with mem=2G for this session)\n",
		need);
	return -1;
}

/* DC CIVAC over a range: clean and invalidate to the point of coherency.
 * Linux sets SCTLR_EL1.UCI, so EL0 may issue it. */
static void civac(const void *va, size_t len)
{
	uint64_t ctr;
	uintptr_t line, a, end = (uintptr_t)va + len;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	line = 4u << ((ctr >> 16) & 0xf);
	for (a = (uintptr_t)va & ~(line - 1); a < end; a += line)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* --------------------------------------------------------------- MMU -- */

static int mmu_wait(unsigned base, uint32_t mask, uint32_t want)
{
	for (int i = 0; i < 1000; i++) {
		if ((rd(base + RK_MMU_STATUS) & mask) == want)
			return 0;
		usleep(1);
	}
	return -1;
}

static int mmu_cmd(unsigned base, uint32_t cmd, uint32_t mask, uint32_t want)
{
	wr(base + RK_MMU_COMMAND, cmd);
	if (dry_run)
		return 0;
	if (mmu_wait(base, mask, want)) {
		fprintf(stderr, "  IOMMU 0x%08lx: command %u, STATUS 0x%08x\n",
			NPU0_BASE + base, cmd, rd(base + RK_MMU_STATUS));
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------ oracle -- */

static uint8_t mix(unsigned a, unsigned b, unsigned span)
{
	uint32_t h = a * 2654435761u ^ (b + 0x9e3779b9u) * 40503u;

	h ^= h >> 13;
	return (uint8_t)(128 + (int)(h % span) - (int)(span / 2));
}

int main(int argc, char **argv)
{
	unsigned k = 256, n = 64, m = 1;
	int allow_mmu = 0, mmu_was_on = 0, rc = 1, need_reboot = 0;
	const unsigned mmus[2] = { MMU0_OFF, MMU1_OFF };
	uint32_t dte[2] = { 0 }, imask[2] = { 0 };
	struct charsiu_job job = { 0 };
	struct pool pool = { 0 };
	uint8_t *A, *B, *wt, *in, *ob, *coef, *reg;
	uint32_t pwt, pin, pob, pcoef, preg;
	size_t wsz, insz, osz, csz, nreg, i;
	int32_t *want, *zero;
	char buf[128];
	int memfd;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--k") && i + 1 < argc)
			k = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--n") && i + 1 < argc)
			n = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--allow-mmu-writes"))
			allow_mmu = 1;
		else if (!strcmp(argv[i], "--dry-run"))
			dry_run = 1;
		else {
			fprintf(stderr, "usage: %s [--k K] [--n N] [--allow-mmu-writes] [--dry-run]\n", argv[0]);
			return 2;
		}
	}
	printf("m0_bypass: M=%u K=%u N=%u, int8, core 0%s\n", m, k, n, dry_run ? ", DRY RUN" : "");

	/* --- 1. refuse unless the state is the one this was written for --- */
	if (geteuid()) {
		fprintf(stderr, "run as root\n");
		return 1;
	}
	if (read_line(SYS_CORE0 "/power/control", buf, sizeof(buf)) || strcmp(buf, "on")) {
		fprintf(stderr, "%s/power/control is not 'on'. Pin it first:\n"
			"  echo on > %s/power/control\n", SYS_CORE0, SYS_CORE0);
		return 1;
	}
	if (read_line(SYS_CORE0 "/power/runtime_status", buf, sizeof(buf)) || strcmp(buf, "active")) {
		fprintf(stderr, "core 0 runtime_status is '%s', not active\n", buf);
		return 1;
	}
	if (!genpd_on("npu") || !genpd_on("nputop") || !genpd_on("npu0")) {
		fprintf(stderr, "npu / nputop / npu0 not all on in %s\n", GENPD);
		return 1;
	}
	if (accel_in_use()) {
		fprintf(stderr, "something holds the NPU open; stop it first\n");
		return 1;
	}

	memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd < 0) {
		perror("/dev/mem");
		return 1;
	}
	npu = mmap(NULL, NPU0_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, NPU0_BASE);
	if (npu == MAP_FAILED) {
		fprintf(stderr, "mmap of 0x%08lx: %s\n"
			"  rocket owns this range; boot with iomem=relaxed for this session\n",
			NPU0_BASE, strerror(errno));
		return 1;
	}
	printf("  core 0: version 0x%08x num 0x%08x op_en %u raw 0x%08x task_status 0x%08x\n",
	       rd(PC_VERSION), rd(PC_VERSION_NUM), rd(PC_OPERATION_ENABLE),
	       rd(PC_INTERRUPT_RAW_STATUS), rd(PC_TASK_STATUS));
	if (rd(PC_OPERATION_ENABLE)) {
		fprintf(stderr, "core 0 OPERATION_ENABLE is set: a job is running\n");
		return 1;
	}
	for (i = 0; i < 2; i++) {
		uint32_t st = rd(mmus[i] + RK_MMU_STATUS);

		dte[i] = rd(mmus[i] + RK_MMU_DTE_ADDR);
		imask[i] = rd(mmus[i] + RK_MMU_INT_MASK);
		printf("  IOMMU 0x%08lx: STATUS 0x%08x DTE_ADDR 0x%08x INT_MASK 0x%08x, paging %s\n",
		       NPU0_BASE + mmus[i], st, dte[i], imask[i],
		       (st & RK_MMU_STATUS_PAGING) ? "ON" : "off");
		if (st & RK_MMU_STATUS_PAGING)
			mmu_was_on = 1;
	}
	if (mmu_was_on && !allow_mmu) {
		fprintf(stderr,
			"paging is on (the NPU group's default domain is a DMA domain).\n"
			"Either reboot with iommu.passthrough=1 for this session, so the IOMMU\n"
			"sits in identity between rocket jobs and nothing here touches it, or\n"
			"pass --allow-mmu-writes to switch paging off for the job and back on.\n");
		return 1;
	}

	/* --- 2. buffers, 3. the job --- */
	if (pool_init(&pool))
		return 1;
	job.cbuf_window = 0;
	job.mm.m = m; job.mm.k = k; job.mm.n = n;
	job.mm.wdtype = CHARSIU_INT8;
	job.mm.adtype = CHARSIU_INT8;
	job.input_zero_point = 128;
	job.weight_zero_point = 128;
	job.output_zero_point = 0;
	job.input_scale = 1.0f;
	job.weight_scale = 1.0f;
	job.output_scale = 1.0f;
	job.acc_out = 1;

	wsz = charsiu_weight_bytes(&job.mm) + 4096;
	insz = (size_t)charsiu_entries_per_row(&job.mm) * 64 * m + 4096;
	osz = (size_t)m * n * 4 + 4096;
	csz = charsiu_coef_bytes(&job.mm) + 4096;
	if (pool_take(&pool, wsz, &wt, &pwt) || pool_take(&pool, insz, &in, &pin) ||
	    pool_take(&pool, osz, &ob, &pob) || pool_take(&pool, csz, &coef, &pcoef) ||
	    pool_take(&pool, 4096, &reg, &preg))
		return 1;
	printf("  PA: weights 0x%08x input 0x%08x output 0x%08x coef 0x%08x regcmd 0x%08x\n",
	       pwt, pin, pob, pcoef, preg);

	A = malloc((size_t)m * k);
	B = malloc((size_t)n * k);
	want = calloc((size_t)m * n, sizeof(*want));
	zero = calloc(n, sizeof(*zero));
	if (!A || !B || !want || !zero)
		return 1;
	for (unsigned r = 0; r < m; r++)
		for (unsigned c = 0; c < k; c++)
			A[(size_t)r * k + c] = mix(r, c, 61);
	for (unsigned r = 0; r < n; r++)
		for (unsigned c = 0; c < k; c++)
			B[(size_t)r * k + c] = mix(r + 7919, c, 59);
	for (unsigned r = 0; r < m; r++)
		for (unsigned c = 0; c < n; c++) {
			int32_t acc = 0;

			for (unsigned j = 0; j < k; j++)
				acc += ((int)A[(size_t)r * k + j] - 128) * ((int)B[(size_t)c * k + j] - 128);
			want[(size_t)r * n + c] = acc;
		}

	memset(wt, 0, charsiu_weight_bytes(&job.mm));
	charsiu_pack_weights(&job.mm, B, wt);
	charsiu_pack_input(&job.mm, A, in, insz, (uint8_t)job.input_zero_point);
	charsiu_build_coefs(&job, zero, zero, coef);
	memset(ob, 0, (size_t)m * n * 4);
	job.input_addr = pin;
	job.output_addr = pob;
	job.weight_addr = pwt;
	job.coef_addr = pcoef;
	nreg = charsiu_emit_job(&job, (uint64_t *)reg, 4096 / 8);
	if (!nreg) {
		fprintf(stderr, "the emitter returned an empty stream\n");
		return 1;
	}
	printf("  regcmd: %zu entries\n", nreg);

	/* 4. the NPU is not coherent: everything it reads or writes out of the caches */
	civac(pool.va, pool.used);

	if (mmu_was_on) {
		printf("  IOMMU: switching paging off for the job\n");
		for (i = 0; i < 2; i++)
			if (mmu_cmd(mmus[i], RK_MMU_CMD_ENABLE_STALL, RK_MMU_STATUS_STALL_ACTIVE, RK_MMU_STATUS_STALL_ACTIVE) ||
			    mmu_cmd(mmus[i], RK_MMU_CMD_DISABLE_PAGING, RK_MMU_STATUS_PAGING, 0) ||
			    mmu_cmd(mmus[i], RK_MMU_CMD_DISABLE_STALL, RK_MMU_STATUS_STALL_ACTIVE, 0)) {
				need_reboot = 1;
				goto restore_mmu;
			}
	}

	/* rocket_job_hw_submit(), core 0, with the interrupt masked */
	printf("  submit:\n");
	wr(PC_BASE_ADDRESS, 0x1);
	wr(CNA_OFF + S_POINTER, S_POINTER_PP_EN | S_POINTER_EXECUTER_PP_EN | S_POINTER_PP_MODE);
	wr(CORE_OFF + S_POINTER, S_POINTER_PP_EN | S_POINTER_EXECUTER_PP_EN | S_POINTER_PP_MODE);
	wr(PC_BASE_ADDRESS, preg);
	wr(PC_REGISTER_AMOUNTS, (uint32_t)((nreg + 1) / 2 - 1) & 0xffff);
	wr(PC_INTERRUPT_MASK, 0);
	wr(PC_INTERRUPT_CLEAR, 0x1ffff);
	wr(PC_TASK_CON, RK3576_TASK_LAST_LAYER_CLR | RK3576_TASK_COUNT_CLEAR |
			RK3576_TASK_PP_EN | RK3576_TASK_NUMBER(1));
	wr(PC_TASK_DMA_BASE_ADDR, 0);
	wr(PC_OPERATION_ENABLE, 1);

	if (!dry_run) {
		struct timespec t0, t1;
		uint32_t raw = 0;
		int done = 0;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int t = 0; t < 10000 && !done; t++) {    /* 1 s */
			raw = rd(PC_INTERRUPT_RAW_STATUS);
			done = (raw & (RAW_DPU_0 | RAW_DPU_1)) != 0;
			if (!done)
				usleep(100);
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("  %s after %.0f us: RAW_STATUS 0x%08x TASK_STATUS 0x%08x\n",
		       done ? "done" : "NOT DONE", (t1.tv_sec - t0.tv_sec) * 1e6 +
		       (t1.tv_nsec - t0.tv_nsec) / 1e3, raw, rd(PC_TASK_STATUS));
		/* rocket's completion writes */
		wr(PC_OPERATION_ENABLE, 0);
		wr(PC_INTERRUPT_CLEAR, 0x1ffff);
		if (!done)
			need_reboot = 1;
		if (raw & (RAW_DMA_READ_ERROR | RAW_DMA_WRITE_ERROR))
			printf("  DMA %s error flagged\n",
			       (raw & RAW_DMA_READ_ERROR) ? "read" : "write");
	}

restore_mmu:
	if (mmu_was_on) {
		printf("  IOMMU: paging back on\n");
		/*
		 * rk_iommu_enable()'s order. Its stall is skipped while paging
		 * is off (rk_iommu_enable_stall() returns early), so there is
		 * none here either; the unstall runs only if a stall is active.
		 */
		for (i = 0; i < 2; i++) {
			wr(mmus[i] + RK_MMU_DTE_ADDR, dte[i]);
			wr(mmus[i] + RK_MMU_COMMAND, RK_MMU_CMD_ZAP_CACHE);
			wr(mmus[i] + RK_MMU_INT_MASK, imask[i]);
			if (mmu_cmd(mmus[i], RK_MMU_CMD_ENABLE_PAGING, RK_MMU_STATUS_PAGING, RK_MMU_STATUS_PAGING))
				need_reboot = 1;
			if (!dry_run && (rd(mmus[i] + RK_MMU_STATUS) & RK_MMU_STATUS_STALL_ACTIVE) &&
			    mmu_cmd(mmus[i], RK_MMU_CMD_DISABLE_STALL, RK_MMU_STATUS_STALL_ACTIVE, 0))
				need_reboot = 1;
			if (!dry_run && rd(mmus[i] + RK_MMU_DTE_ADDR) != dte[i]) {
				printf("  IOMMU 0x%08lx: DTE_ADDR reads 0x%08x, was 0x%08x\n",
				       NPU0_BASE + mmus[i], rd(mmus[i] + RK_MMU_DTE_ADDR), dte[i]);
				need_reboot = 1;
			}
		}
	}
	if (need_reboot || dry_run)
		goto out;

	/* 5. read back and compare */
	civac(pool.va, pool.used);
	{
		const int32_t *got = (const int32_t *)ob;
		unsigned bad = 0;
		long worst = 0;
		size_t touched = 0;

		for (i = 0; i < (size_t)m * n; i++) {
			long d = (long)got[i] - (long)want[i];

			if (d) {
				bad++;
				if (labs(d) > labs(worst))
					worst = d;
			}
		}
		printf("  result: %u of %u wrong, worst delta %ld\n", bad, m * n, worst);
		if (bad) {
			printf("    got:");
			for (i = 0; i < 6 && i < (size_t)m * n; i++)
				printf(" %d", got[i]);
			printf("\n    want:");
			for (i = 0; i < 6 && i < (size_t)m * n; i++)
				printf(" %d", want[i]);
			printf("\n");
		}
		/* the canary: the pool beyond the buffers, and each buffer's slack */
		for (i = pool.used; i < pool.size; i++)
			if (pool.va[i] != CANARY)
				touched++;
		for (i = (size_t)m * n * 4; i < osz; i++)
			if (ob[i] != CANARY)
				touched++;
		printf("  canary: %zu bytes outside the output changed\n", touched);
		if (touched)
			need_reboot = 1;
		rc = bad ? 3 : 0;
		printf("M0 BYPASS %s\n", bad ? "FAIL" : "PASS");
	}

out:
	if (need_reboot) {
		printf("REBOOT: core 0 or its IOMMU may be in a state rocket cannot recover from\n");
		rc = 4;
	}
	return rc;
}
