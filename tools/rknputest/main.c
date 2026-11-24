/*
 * rknputest - talks to rknpu.sys (\\.\RKNPU). Run as administrator.
 *
 *   rknputest info              driver and hardware state, with checks
 *   rknputest regs <off> <n>    read n words at byte offset off (hex or
 *                               decimal) of the NPU register window
 *   rknputest selftest          interface layout and range checks; needs no
 *                               device and runs on any host (CI runs it on
 *                               Linux)
 *   rknputest m3 [options]      M3: int8 matmuls on the NPU, compared bit for
 *                               bit with the CPU (see cmd_m3 and docs/history/2026-11-22-m3.md)
 *   rknputest m6 [options]      M6: the driver's IOMMU: scattered buffers far
 *                               past M4's contiguous limit, the M3 ladder
 *                               through the page table, per-job latency, both
 *                               cores at once, and optionally a deliberate
 *                               page fault (see cmd_m6 and docs/history/2026-11-24-m6.md)
 *
 * Exit status: 0 ok, 1 usage or device error, 2 a check in `info` failed,
 * 3 an m3 result was wrong, 4 an m3 job did not complete, 5 an m6 check
 * failed.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <time.h>
#endif

#include "rknpu_ioctl.h"
#include "m3_gemm.h"

#define EXPECT_DSU0_HZ  594000000u

static const unsigned int expect_gsiv[RKNPU_NUM_CORES] = { 279, 280 };

/* ------------------------------------------------------------ device */

#ifdef _WIN32
/* per thread: `m6` drives both cores from two threads, each with its own
 * handle (an M6 driver lets more than one be open) */
static __declspec(thread) HANDLE g_dev = INVALID_HANDLE_VALUE;

static int dev_open(void)
{
    g_dev = CreateFileW(RKNPU_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_dev == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot open \\\\.\\RKNPU: error %lu (driver not installed or not started, "
                "not run as administrator, or another program has it open)\n", GetLastError());
        return -1;
    }
    return 0;
}

static int dev_ioctl(DWORD code, const void *in, DWORD in_len, void *out, DWORD out_len, DWORD *got)
{
    DWORD n = 0;

    if (!DeviceIoControl(g_dev, code, (void *)in, in_len, out, out_len, &n, NULL)) {
        fprintf(stderr, "DeviceIoControl 0x%08lx failed: error %lu\n", code, GetLastError());
        return -1;
    }
    if (got)
        *got = n;
    return 0;
}
#elif defined(RKNPU_FAKE_DEVICE)
#include "fakedev.h"
#include <pthread.h>
static int dev_open(void)
{
    printf("(fakedev: no NPU, the matmul is done in C; this checks the tool, not the hardware)\n");
    fake_open();
    return 0;
}
#else
static int dev_open(void)
{
    fprintf(stderr, "no device on this platform (only `selftest` runs here)\n");
    return -1;
}
#endif

static int dev_info(rknpu_info *o)
{
    memset(o, 0, sizeof(*o));
#ifdef _WIN32
    {
        DWORD got = 0;
        if (dev_ioctl(IOCTL_RKNPU_GET_INFO, NULL, 0, o, sizeof(*o), &got))
            return -1;
        if (got != sizeof(*o)) {
            fprintf(stderr, "GET_INFO returned %lu bytes, expected %u\n", got, (unsigned)sizeof(*o));
            return -1;
        }
    }
    return 0;
#elif defined(RKNPU_FAKE_DEVICE)
    return fake_ioctl(IOCTL_RKNPU_GET_INFO, NULL, 0, o, sizeof(*o));
#else
    return -1;
#endif
}

static int dev_read_regs(unsigned int offset, unsigned int count, unsigned int *out)
{
    rknpu_read_regs_in in;

    in.offset = offset;
    in.count = count;
#ifdef _WIN32
    return dev_ioctl(IOCTL_RKNPU_READ_REGS, &in, sizeof(in), out, count * 4, NULL);
#else
    (void)in;
    (void)out;
    return -1;
#endif
}

/* -------------------------------------------------------------- info */

static int g_fail;

static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        g_fail = 1;
}

static int cmd_info(void)
{
    rknpu_info in;
    unsigned int n, i;
    char line[160];

    if (dev_open() || dev_info(&in))
        return 1;

    printf("interface %u (this tool %u), struct %u bytes, flags 0x%08x%s%s%s\n",
           in.interface_version, RKNPU_INTERFACE_VERSION, in.struct_size, in.flags,
           (in.flags & RKNPU_INFO_INTERRUPTS_CONNECTED) ? " interrupts" : "",
           (in.flags & RKNPU_INFO_CLOCKS_VALID) ? " clocks" : "",
           (in.flags & RKNPU_INFO_DOMAINS_ON) ? " domains-on" : "");
    printf("registers 0x%llx + 0x%x, %u cores\n", in.regs_pa, in.regs_len, in.core_count);

    for (n = 0; n < RKNPU_NUM_CORES; n++) {
        const rknpu_core_info *c = &in.core[n];

        printf("core %u: version=0x%08x version_num=0x%08x\n", n, c->pc_version, c->pc_version_num);
        printf("  pc: operation_enable 0x%08x int mask 0x%08x status 0x%08x raw 0x%08x "
               "task_con 0x%08x task_status 0x%08x\n",
               c->pc_operation_enable, c->pc_interrupt_mask, c->pc_interrupt_status,
               c->pc_interrupt_raw, c->pc_task_con, c->pc_task_status);
        for (i = 0; i < RKNPU_MMUS_PER_CORE; i++)
            printf("  mmu %u: status 0x%08x (paging %s) dte 0x%08x int_rawstat 0x%08x int_mask 0x%08x\n",
                   i, c->mmu_status[i], (c->mmu_status[i] & 1) ? "ON" : "off", c->mmu_dte_addr[i],
                   c->mmu_int_rawstat[i], c->mmu_int_mask[i]);
        printf("  irq: gsiv %u, isr %u (unclaimed %u), dpc %u, last pc 0x%08x, last mmu 0x%08x/0x%08x "
               "fault 0x%08x/0x%08x\n",
               c->gsiv, c->isr_count, c->isr_unclaimed, c->dpc_count, c->last_pc_status,
               c->last_mmu_int_status[0], c->last_mmu_int_status[1],
               c->last_mmu_fault_addr[0], c->last_mmu_fault_addr[1]);
        if (in.m3_version)
            printf("  m3: jobs %u submitted, %u completed, %u timed out, %u resets\n",
                   c->jobs_submitted, c->jobs_completed, c->jobs_timed_out, c->resets);
        if (in.m6_version)
            printf("  m6: iommu enabled %u times, %u faults\n", c->mmu_enables, c->mmu_faults);
    }
    if (in.m3_version)
        printf("m3: version %u, %u buffers, %u KiB\n", in.m3_version, in.bo_count, in.bo_kib);
    else
        printf("m3: not in this driver (an M2 build)\n");
    if (in.m4_version)
        printf("m4: version %u, copy arena %u KiB a core, buffers up to %u KiB each and %u MiB in all\n",
               in.m4_version, in.copy_arena_bytes / 1024, in.bo_max_kib, in.bo_max_total_kib / 1024);
    else if (in.m3_version)
        printf("m4: not in this driver (an M3 build)\n");
    if (in.m6_version)
        printf("m6: version %u, memory %s, buffers %s, NPU address space %u KiB in use, %u page-table pages\n",
               in.m6_version, in.mem_mode == RKNPU_MEM_IOMMU ? "IOMMU (driver page table)" : "bypass (M4)",
               in.bo_cached ? "cached (PREP_BO/FINI_BO)" : "write-combined", in.iova_kib, in.pt_pages);
    else if (in.m4_version)
        printf("m6: not in this driver (an M4 build)\n");

    if (in.flags & RKNPU_INFO_CLOCKS_VALID) {
        const rknpu_clock_info *k = &in.clocks;
        printf("pmu: STATUS0 0x%08x REPAIR0 0x%08x\n", k->pmu_status0, k->pmu_repair0);
        printf("cru: CLKSEL86 0x%08x GATE28 0x%08x GATE29 0x%08x GATE31 0x%08x GATE32 0x%08x\n",
               k->cru_clksel86, k->cru_gate28, k->cru_gate29, k->cru_gate31, k->cru_gate32);
        printf("clk: gpll %u Hz, clk_rknn_dsu0 %u Hz\n", k->gpll_hz, k->dsu0_hz);
    } else {
        printf("clocks: not read (the driver could not map the PMU and CRU pages)\n");
    }

    printf("checks:\n");
    check(in.interface_version == RKNPU_INTERFACE_VERSION && in.struct_size == sizeof(in),
          "driver and tool agree on the interface");
    check(in.regs_len >= RKNPU_REGS_WINDOW, "register window is 64 KiB");
    check((in.flags & RKNPU_INFO_INTERRUPTS_CONNECTED) != 0, "interrupts connected");
    check((in.flags & RKNPU_INFO_DOMAINS_ON) != 0, "NPU domains on and clocks ungated");
    for (n = 0; n < RKNPU_NUM_CORES; n++) {
        const rknpu_core_info *c = &in.core[n];
        snprintf(line, sizeof(line), "core %u version register reads a value (not 0, not all ones)", n);
        check(c->pc_version != 0 && c->pc_version != 0xFFFFFFFFu, line);
        if (in.m6_version && in.mem_mode == RKNPU_MEM_IOMMU) {
            /* paging is on once a job has run, off before the first and after a reset */
            snprintf(line, sizeof(line), "core %u IOMMUs agree (paging %s), no fault", n,
                     (c->mmu_status[0] & 1) ? "on" : "off");
            check((c->mmu_status[0] & 1) == (c->mmu_status[1] & 1) && c->mmu_faults == 0, line);
        } else {
            snprintf(line, sizeof(line), "core %u IOMMUs have paging off", n);
            check(!(c->mmu_status[0] & 1) && !(c->mmu_status[1] & 1), line);
        }
        snprintf(line, sizeof(line), "core %u interrupt is GSIV %u", n, expect_gsiv[n]);
        check(c->gsiv == expect_gsiv[n], line);
        if (c->jobs_submitted == 0) {
            snprintf(line, sizeof(line), "core %u no interrupt seen (no job was started)", n);
            check(c->isr_count == 0 && c->isr_unclaimed == 0, line);
        } else {
            snprintf(line, sizeof(line), "core %u no unclaimed interrupt (%u jobs, %u isr)", n,
                     c->jobs_submitted, c->isr_count);
            check(c->isr_unclaimed == 0, line);
        }
    }
    if (in.flags & RKNPU_INFO_CLOCKS_VALID)
        check(in.clocks.dsu0_hz == EXPECT_DSU0_HZ, "clk_rknn_dsu0 is 594 MHz (GPLL / 2)");
    check(in.core[0].pc_version == in.core[1].pc_version, "both cores report the same version");
    printf("compare by hand with the firmware serial log:\n");
    for (n = 0; n < RKNPU_NUM_CORES; n++)
        printf("  RK3576Dxe: NPU: core %u version=0x%08x version_num=0x%08x mmu status=0x%08x/0x%08x\n",
               n, in.core[n].pc_version, in.core[n].pc_version_num,
               in.core[n].mmu_status[0], in.core[n].mmu_status[1]);
    printf("%s\n", g_fail ? "RKNPU INFO: CHECKS FAILED" : "RKNPU INFO: OK");
    return g_fail ? 2 : 0;
}

/* -------------------------------------------------------------- regs */

static int parse_u32(const char *s, unsigned int *v)
{
    char *end;
    unsigned long x = strtoul(s, &end, 0);

    if (*s == '\0' || *end != '\0' || x > 0xFFFFFFFFul)
        return -1;
    *v = (unsigned int)x;
    return 0;
}

static int cmd_regs(const char *off_s, const char *n_s)
{
    unsigned int off, n, i, *buf;

    if (parse_u32(off_s, &off) || parse_u32(n_s, &n)) {
        fprintf(stderr, "regs: offset and count are numbers (0x.. for hex)\n");
        return 1;
    }
    if (!rknpu_read_range_ok(off, n)) {
        fprintf(stderr, "regs: 0x%x + %u words is not inside one described block of a core\n"
                "  (per core, core 1 at +0x8000: pc 0x0000-0x0fff, cna 0x1000-0x1fff,\n"
                "   mmu 0x2000-0x20ff and 0x2100-0x21ff, core 0x3000-0x3fff; at most %u words)\n",
                off, n, RKNPU_MAX_READ_WORDS);
        return 1;
    }
    buf = (unsigned int *)calloc(n, 4);
    if (!buf)
        return 1;
    if (dev_open() || dev_read_regs(off, n, buf)) {
        free(buf);
        return 1;
    }
    for (i = 0; i < n; i++) {
        if (i % 4 == 0)
            printf("%s%05x:", i ? "\n" : "", off + i * 4);
        printf(" %08x", buf[i]);
    }
    printf("\n");
    free(buf);
    return 0;
}

/* ---------------------------------------------------------------- m3 */

/*
 * rknputest m3 [--core 0|1|both] [--iters N] [--shape M K N] [--map] [--no-reset]
 *
 * Runs a ladder of int8 matmuls C = A * B^T on the NPU and compares every
 * output word with the CPU: M = 1, K = 64, N = 64 first (the shape charsiu's
 * lab notebook records exact on the board), then up to M = 16, K = 1024,
 * N = 1024. A job of M rows is M tasks, one per row, which also exercises the
 * driver's task chaining from the interrupt. m3_gemm.h has the format and
 * where each part of it comes from. The ladder stops at the first failure.
 *
 * Every buffer is written and read through WRITE_BO and READ_BO, so no NPU
 * page is mapped into this process unless --map is given (then the output is
 * read through a MAP_BO mapping as well, and unmapped before it is freed).
 * Each output row has 4 KiB of canary after it; a canary word the NPU
 * overwrote is reported as an overrun, since with the IOMMU off an overrun
 * lands in whatever memory follows.
 *
 * After the ladder, unless --no-reset, each core is reset through RESET (the
 * SSDT's _DSM) and the first shape is run again on it: the path a timed-out
 * job takes, tried while nothing is wrong.
 */

#define M3_CANARY       0xA5A5A5A5u
#define M3_TIMEOUT_MS   2000u

static double now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER t;

    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e6 / (double)f.QuadPart;
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
#endif
}

struct m3_bo {
    unsigned int handle, addr, size, flags;     /* flags: RKNPU_BO_* from CREATE_BO (M6) */
};

static int m3_call(unsigned int code, const void *in, unsigned int in_len, void *out, unsigned int out_len)
{
#ifdef _WIN32
    return dev_ioctl(code, in, in_len, out, out_len, NULL);
#elif defined(RKNPU_FAKE_DEVICE)
    if (fake_ioctl(code, in, in_len, out, out_len)) {
        fprintf(stderr, "fakedev: ioctl 0x%08x failed\n", code);
        return -1;
    }
    return 0;
#else
    (void)code; (void)in; (void)in_len; (void)out; (void)out_len;
    return -1;
#endif
}

static int bo_create(unsigned int size, struct m3_bo *b)
{
    rknpu_create_bo c;

    memset(&c, 0, sizeof(c));
    c.size = size;
    memset(b, 0, sizeof(*b));
    if (m3_call(IOCTL_RKNPU_CREATE_BO, &c, sizeof(c), &c, sizeof(c)))
        return -1;
    b->handle = c.handle;
    b->addr = c.dma_address;
    b->size = c.size;
    b->flags = c.flags;
    return 0;
}

/* M6: PREP_BO / FINI_BO on a cached buffer; nothing to do on a write-combined one */
static int bo_sync(unsigned int code, const struct m3_bo *b)
{
    rknpu_bo_sync s;

    if (!(b->flags & RKNPU_BO_CACHED))
        return 0;
    memset(&s, 0, sizeof(s));
    s.handle = b->handle;
    return m3_call(code, &s, sizeof(s), NULL, 0);
}

static void bo_free(struct m3_bo *b)
{
    rknpu_bo_handle h;

    if (!b->handle)
        return;
    memset(&h, 0, sizeof(h));
    h.handle = b->handle;
    m3_call(IOCTL_RKNPU_FREE_BO, &h, sizeof(h), NULL, 0);
    b->handle = 0;
}

static int bo_io(unsigned int code, const struct m3_bo *b, unsigned int off, void *p, size_t len)
{
    rknpu_bo_io io;
    size_t done = 0;

    while (done < len) {
        unsigned int n = (unsigned int)(len - done > RKNPU_BO_IO_MAX ? RKNPU_BO_IO_MAX : len - done);

        memset(&io, 0, sizeof(io));
        io.handle = b->handle;
        io.offset = off + (unsigned int)done;
        io.length = n;
        if (m3_call(code, &io, sizeof(io), (unsigned char *)p + done, n))
            return -1;
        done += n;
    }
    return 0;
}

/* The rules SUBMIT enforces (rknpu_regcmd.h), checked here first for a clearer message. */
static int m3_stream_ok(const unsigned long long *e, unsigned int count, const struct m3_bo *bos, unsigned int nbo)
{
    unsigned int i, j, fetched = (count + 1) / 2 * 2;

    for (i = 0; i < fetched; i++) {
        unsigned long long x = i < count ? e[i] : 0;
        unsigned int reg = RKNPU_RC_REG(x), v = RKNPU_RC_VALUE(x);
        int inside = 0;

        if (x == 0)
            continue;
        if (rknpu_rc_is_pc_reg(reg))
            return 0;
        if (!rknpu_rc_is_addr_reg(reg) || v == 0)
            continue;
        for (j = 0; j < nbo; j++)
            if (v >= bos[j].addr && (unsigned long long)v < (unsigned long long)bos[j].addr + bos[j].size)
                inside = 1;
        if (!inside)
            return 0;
    }
    return 1;
}

static void m3_crumb_hint(void)
{
    printf("  the driver's last stage is in the registry (it survives a hang):\n"
           "    reg query \"HKLM\\SYSTEM\\CurrentControlSet\\Enum\\ACPI\\RKCP3576\\0\\Device Parameters\"\n"
           "  M3Stage: 3 submitted, 4 interrupt seen, 5 returned, 6 timed out, 7 reset\n");
}

static void m3_print_wait(const rknpu_wait_out *w)
{
    printf("    wait: result %u, raw 0x%08x task_status 0x%08x, tasks %u of %u, isr %u, hw %u us, "
           "reset 0x%08x, version after 0x%08x, mmu 0x%08x/0x%08x\n",
           w->result, w->raw_status, w->task_status, w->tasks_done, w->task_count, w->isr_count, w->hw_us,
           w->reset_status, w->version_after, w->mmu_status[0], w->mmu_status[1]);
}

/* 0 exact, 1 setup error, 3 wrong, 4 the job did not complete */
/*
 * fault (M6, IOMMU mode, m = 1): the output buffer is one page and the DPU
 * destination is 128 bytes before its end, so the NPU's write runs into the
 * unmapped guard page after it. SUBMIT accepts it (the address is inside the
 * buffer); the IOMMU must stop it: 0 if WAIT says RKNPU_JOB_MMU_FAULT.
 */
static int m3_run_ex(unsigned int core, unsigned int m, unsigned int k, unsigned int n, unsigned int iters, int map,
                     int fault, double *rt_min_out, unsigned int win)
{
    const struct m3_shape *s = m3_find_shape_w(k, n, win);
    unsigned int rc_slot, in_slot, out_slot, r, i, it;
    size_t wsz, csz, in_total, out_total, rc_total;
    unsigned char *A = NULL, *B = NULL, *wbuf = NULL, *cbuf = NULL, *ibuf = NULL, *obuf = NULL;
    unsigned long long *rcbuf = NULL;
    int *want = NULL;
    struct m3_bo bo[5];     /* regcmd, input, weights, coefs, output */
    double cpu_us, t0, rt_sum = 0, hw_sum = 0, rt_min = 1e30;
    unsigned int hw_min = 0xFFFFFFFFu;
    int ret = 1;

    memset(bo, 0, sizeof(bo));
    if (!s || m == 0 || m > RKNPU_MAX_TASKS) {
        fprintf(stderr, "m3: no captured stream for K=%u N=%u, or M=%u is not 1..%u (shapes:", k, n, m,
                RKNPU_MAX_TASKS);
        for (i = 0; i < sizeof(m3_shapes) / sizeof(m3_shapes[0]); i++)
            fprintf(stderr, " %ux%u", m3_shapes[i].k, m3_shapes[i].n);
        fprintf(stderr, ")\n");
        return 1;
    }
    printf("core %u: M=%u K=%u N=%u, %u task%s of %u entries, CBUF window %u\n", core, m, k, n, m,
           m > 1 ? "s" : "", s->count, win);
    fflush(stdout);

    rc_slot = M3_ALIGN((s->count + 1) * 8u, 256u);
    in_slot = (unsigned int)M3_ALIGN(m3_input_row_bytes(k) + 4096u, 4096u);
    out_slot = M3_ALIGN(n * 4u, 4096u) + 4096u;
    if (fault) {
        if (m != 1)
            return 1;
        out_slot = 4096u;
    }
    wsz = m3_weight_bytes(k, n) + 4096u;
    csz = m3_coef_bytes(n) + 4096u;
    rc_total = (size_t)rc_slot * m;
    in_total = (size_t)in_slot * m;
    out_total = (size_t)out_slot * m;

    A = (unsigned char *)malloc((size_t)m * k);
    B = (unsigned char *)malloc((size_t)n * k);
    wbuf = (unsigned char *)malloc(wsz);
    cbuf = (unsigned char *)malloc(csz);
    ibuf = (unsigned char *)malloc(in_total);
    obuf = (unsigned char *)malloc(out_total);
    rcbuf = (unsigned long long *)calloc(rc_total / 8, 8);
    want = (int *)malloc((size_t)m * n * sizeof(int));
    if (!A || !B || !wbuf || !cbuf || !ibuf || !obuf || !rcbuf || !want) {
        fprintf(stderr, "m3: out of memory\n");
        goto out;
    }

    /* npu_gemm_test.c main(): the same data */
    for (r = 0; r < m; r++)
        for (i = 0; i < k; i++)
            A[(size_t)r * k + i] = m3_mix(r * 2u + 1u, i, 15);
    for (r = 0; r < n; r++)
        for (i = 0; i < k; i++)
            B[(size_t)r * k + i] = m3_mix(r, i, 15);
    t0 = now_us();
    for (r = 0; r < m; r++)
        m3_reference_row(k, n, A + (size_t)r * k, B, want + (size_t)r * n);
    cpu_us = now_us() - t0;

    if (bo_create((unsigned int)rc_total, &bo[0]) || bo_create((unsigned int)in_total, &bo[1]) ||
        bo_create((unsigned int)wsz, &bo[2]) || bo_create((unsigned int)csz, &bo[3]) ||
        bo_create((unsigned int)out_total, &bo[4])) {
        fprintf(stderr, "m3: CREATE_BO failed\n");
        goto out;
    }
    printf("  buffers (NPU addresses%s): regcmd 0x%08x input 0x%08x weights 0x%08x coefs 0x%08x output 0x%08x\n",
           (bo[4].flags & RKNPU_BO_CACHED) ? ", cached" : "",
           bo[0].addr, bo[1].addr, bo[2].addr, bo[3].addr, bo[4].addr);

    memset(wbuf, 0, wsz);
    m3_pack_weights(k, n, B, wbuf);
    memset(cbuf, 0, csz);
    m3_build_coefs(n, cbuf);
    for (r = 0; r < m; r++)
        m3_pack_input(k, A + (size_t)r * k, ibuf + (size_t)r * in_slot, in_slot);
    for (r = 0; r < m; r++) {
        unsigned int base[M3_NBUF];

        base[M3_BUF_INPUT] = bo[1].addr + r * in_slot;
        base[M3_BUF_WEIGHT] = bo[2].addr;
        base[M3_BUF_OUTPUT] = fault ? bo[4].addr + 4096u - 128u : bo[4].addr + r * out_slot;
        base[M3_BUF_COEF] = bo[3].addr;
        m3_build_stream(s, base, rcbuf + (size_t)r * rc_slot / 8);
        if (!m3_stream_ok(rcbuf + (size_t)r * rc_slot / 8, s->count, bo, 5)) {
            fprintf(stderr, "m3: the stream for row %u breaks the SUBMIT rules (a bug in this tool)\n", r);
            goto out;
        }
    }
    if (bo_io(IOCTL_RKNPU_WRITE_BO, &bo[0], 0, rcbuf, rc_total) || bo_io(IOCTL_RKNPU_WRITE_BO, &bo[1], 0, ibuf, in_total) ||
        bo_io(IOCTL_RKNPU_WRITE_BO, &bo[2], 0, wbuf, wsz) || bo_io(IOCTL_RKNPU_WRITE_BO, &bo[3], 0, cbuf, csz)) {
        fprintf(stderr, "m3: WRITE_BO failed\n");
        goto out;
    }

    for (it = 0; it < iters; it++) {
        rknpu_submit_in sub;
        rknpu_submit_out so;
        rknpu_wait_in wi;
        rknpu_wait_out wo;
        unsigned int bad = 0, untouched = 0, overrun = 0;
        long long worst = 0, first_delta = 0;
        int same_delta = 1;
        double rt;

        for (i = 0; i < out_total / 4; i++) {
            const unsigned int canary = M3_CANARY;
            memcpy(obuf + (size_t)i * 4, &canary, 4);
        }
        if (bo_io(IOCTL_RKNPU_WRITE_BO, &bo[4], 0, obuf, out_total)) {
            fprintf(stderr, "m3: WRITE_BO (output canary) failed\n");
            goto out;
        }

        memset(&sub, 0, sizeof(sub));
        sub.core = core;
        sub.task_count = m;
        if (it > 0)
            sub.flags = RKNPU_SUBMIT_NO_CRUMBS;     /* the first run leaves the breadcrumbs */
        for (r = 0; r < m; r++) {
            sub.tasks[r].regcmd = bo[0].addr + r * rc_slot;
            sub.tasks[r].regcmd_count = s->count;
        }
        t0 = now_us();
        if (m3_call(IOCTL_RKNPU_SUBMIT, &sub, sizeof(sub), &so, sizeof(so))) {
            fprintf(stderr, "m3: SUBMIT refused\n");
            goto out;
        }
        memset(&wi, 0, sizeof(wi));
        wi.seqno = so.seqno;
        wi.timeout_ms = M3_TIMEOUT_MS;
        if (m3_call(IOCTL_RKNPU_WAIT, &wi, sizeof(wi), &wo, sizeof(wo))) {
            fprintf(stderr, "m3: WAIT failed\n");
            goto out;
        }
        rt = now_us() - t0;
        if (fault) {
            if (wo.result == RKNPU_JOB_MMU_FAULT) {
                printf("  IOMMU FAULT as intended: NPU address 0x%08x (the guard page after 0x%08x + 4 KiB), "
                       "status 0x%08x, core reset 0x%08x\n", wo.fault_addr, bo[4].addr, wo.fault_status,
                       wo.reset_status);
                ret = 0;
            } else {
                printf("  no fault: result %u (the NPU did not write past the buffer, or the IOMMU let it)\n",
                       wo.result);
                m3_print_wait(&wo);
                ret = 3;
            }
            goto out;
        }
        if (wo.result != RKNPU_JOB_DONE) {
            printf("  job %llu did not complete (%s)\n", so.seqno,
                   wo.result == RKNPU_JOB_TIMEOUT ? "timed out, the core was reset" :
                   wo.result == RKNPU_JOB_DMA_ERROR ? "DMA error" :
                   wo.result == RKNPU_JOB_MMU_FAULT ? "IOMMU fault, the core was reset" : "unknown job");
            if (wo.result == RKNPU_JOB_MMU_FAULT)
                printf("    fault at NPU address 0x%08x, status 0x%08x\n", wo.fault_addr, wo.fault_status);
            m3_print_wait(&wo);
            m3_crumb_hint();
            ret = 4;
            goto out;
        }
        if (it == 0)
            m3_print_wait(&wo);

        if (map && it == 0) {
            rknpu_map_bo mb;
            rknpu_bo_handle h;

            memset(&mb, 0, sizeof(mb));
            mb.handle = bo[4].handle;
            if (m3_call(IOCTL_RKNPU_MAP_BO, &mb, sizeof(mb), &mb, sizeof(mb))) {
                fprintf(stderr, "m3: MAP_BO failed\n");
                goto out;
            }
            /* M6: a cached mapping shows what the NPU wrote only after PREP_BO */
            if (bo_sync(IOCTL_RKNPU_PREP_BO, &bo[4])) {
                fprintf(stderr, "m3: PREP_BO failed\n");
                goto out;
            }
            memcpy(obuf, (const void *)(size_t)mb.user_va, out_total);
            memset(&h, 0, sizeof(h));
            h.handle = bo[4].handle;
            if (m3_call(IOCTL_RKNPU_UNMAP_BO, &h, sizeof(h), NULL, 0)) {
                fprintf(stderr, "m3: UNMAP_BO failed\n");
                goto out;
            }
            printf("  output read through MAP_BO at %p, then unmapped\n", (void *)(size_t)mb.user_va);
        } else if (bo_io(IOCTL_RKNPU_READ_BO, &bo[4], 0, obuf, out_total)) {
            fprintf(stderr, "m3: READ_BO failed\n");
            goto out;
        }

        for (r = 0; r < m; r++) {
            const unsigned char *row = obuf + (size_t)r * out_slot;

            for (i = 0; i < n; i++) {
                int got;
                long long d;

                memcpy(&got, row + (size_t)i * 4, 4);
                if ((unsigned int)got == M3_CANARY)
                    untouched++;
                d = (long long)got - want[(size_t)r * n + i];
                if (d) {
                    if (!bad)
                        first_delta = d;
                    else if (d != first_delta)
                        same_delta = 0;
                    bad++;
                    if ((d < 0 ? -d : d) > (worst < 0 ? -worst : worst))
                        worst = d;
                }
            }
            for (i = n * 4u; i + 4 <= out_slot; i += 4) {
                unsigned int w;

                memcpy(&w, row + i, 4);
                if (w != M3_CANARY)
                    overrun++;
            }
        }
        if (bad || overrun) {
            printf("  run %u: %u of %u words wrong (worst delta %lld), %u never written, %u canary words "
                   "after the rows overwritten\n", it, bad, m * n, worst, untouched, overrun);
            if (bad && same_delta)
                printf("  every wrong word is off by the same %lld: an offset in the output stage, "
                       "not the arithmetic\n", first_delta);
            printf("    got: ");
            for (i = 0; i < 8 && i < n; i++) {
                int g;

                memcpy(&g, obuf + (size_t)i * 4, 4);
                printf(" %d", g);
            }
            printf("\n    want:");
            for (i = 0; i < 8 && i < n; i++)
                printf(" %d", want[i]);
            printf("\n");
            m3_print_wait(&wo);
            ret = 3;
            goto out;
        }
        rt_sum += rt;
        hw_sum += wo.hw_us;
        if (rt < rt_min)
            rt_min = rt;
        if (wo.hw_us < hw_min)
            hw_min = wo.hw_us;
    }
    {
        double macs = (double)m * k * n;

        printf("  EXACT: %u x %u words, %u run%s. hw %u us min / %.0f avg (%.1f us a task), "
               "submit to wait return %.0f us min / %.0f avg, CPU reference %.0f us; %.2f GMAC/s on the NPU\n",
               m, n, iters, iters > 1 ? "s" : "", hw_min, hw_sum / iters, (double)hw_min / m,
               rt_min, rt_sum / iters, cpu_us, hw_min ? macs / hw_min / 1e3 : 0.0);
    }
    if (rt_min_out)
        *rt_min_out = rt_sum / iters;
    ret = 0;
out:
    for (i = 0; i < 5; i++)
        bo_free(&bo[i]);
    free(A); free(B); free(wbuf); free(cbuf); free(ibuf); free(obuf); free(rcbuf); free(want);
    fflush(stdout);
    return ret;
}

static int m3_run(unsigned int core, unsigned int m, unsigned int k, unsigned int n, unsigned int iters, int map)
{
    /* core c uses CBUF window c, as charsiu's device d uses window d */
    return m3_run_ex(core, m, k, n, iters, map, 0, NULL, core);
}

static int m3_reset(unsigned int core)
{
    rknpu_reset_in in;
    rknpu_reset_out o;

    memset(&in, 0, sizeof(in));
    memset(&o, 0, sizeof(o));
    in.core = core;
    printf("core %u: RESET (_DSM function 1)\n", core);
    fflush(stdout);
    if (m3_call(IOCTL_RKNPU_RESET, &in, sizeof(in), &o, sizeof(o)))
        return 1;
    printf("  _DSM status 0x%08x returned %u, version after 0x%08x, mmu 0x%08x/0x%08x\n", o.dsm_status,
           o.dsm_return, o.version_after, o.mmu_status[0], o.mmu_status[1]);
    return (o.dsm_status == 0 && o.dsm_return == 0 && o.version_after != 0 && o.version_after != 0xFFFFFFFFu &&
            !(o.mmu_status[0] & 1) && !(o.mmu_status[1] & 1)) ? 0 : 1;
}

static const struct { unsigned int m, k, n; } m3_ladder[] = {
    { 1, 64, 64 },          /* charsiu lab notebook: exact on the board under Linux */
    { 4, 256, 64 },         /* four tasks in one job: the ISR starts tasks 2..4 */
    { 8, 256, 256 },
    { 16, 1024, 1024 },     /* medium: 1 MiB of weights read by each of 16 tasks */
};

static int cmd_m3(int argc, char **argv)
{
    unsigned int cores[2] = { 0, 1 }, ncores = 2, iters = 5, c, i, sm = 0, sk = 0, sn = 0;
    int map = 0, reset = 1, a, rc;
    rknpu_info info;

    for (a = 0; a < argc; a++) {
        if (!strcmp(argv[a], "--core") && a + 1 < argc) {
            a++;
            if (!strcmp(argv[a], "both")) {
                ncores = 2;
            } else if (!parse_u32(argv[a], &cores[0]) && cores[0] < RKNPU_NUM_CORES) {
                ncores = 1;
            } else {
                fprintf(stderr, "m3: --core 0, 1 or both\n");
                return 1;
            }
        } else if (!strcmp(argv[a], "--iters") && a + 1 < argc) {
            if (parse_u32(argv[++a], &iters) || iters == 0 || iters > 1000) {
                fprintf(stderr, "m3: --iters 1..1000\n");
                return 1;
            }
        } else if (!strcmp(argv[a], "--shape") && a + 3 < argc) {
            if (parse_u32(argv[a + 1], &sm) || parse_u32(argv[a + 2], &sk) || parse_u32(argv[a + 3], &sn)) {
                fprintf(stderr, "m3: --shape M K N\n");
                return 1;
            }
            a += 3;
        } else if (!strcmp(argv[a], "--map")) {
            map = 1;
        } else if (!strcmp(argv[a], "--no-reset")) {
            reset = 0;
        } else {
            fprintf(stderr, "m3: unknown option %s\n", argv[a]);
            return 1;
        }
    }

    if (dev_open() || dev_info(&info))
        return 1;
    printf("rknputest m3: driver m3 version %u, flags 0x%08x\n", info.m3_version, info.flags);
    if (info.m3_version != RKNPU_M3_VERSION) {
        fprintf(stderr, "m3: the driver is not an M3 build (m3_version %u); install the M3 package\n",
                info.m3_version);
        return 1;
    }
    if (!(info.flags & RKNPU_INFO_INTERRUPTS_CONNECTED) || !(info.flags & RKNPU_INFO_DOMAINS_ON)) {
        fprintf(stderr, "m3: interrupts not connected or NPU domains off; run `rknputest info`\n");
        return 1;
    }
    for (c = 0; c < ncores && !(info.m6_version && info.mem_mode == RKNPU_MEM_IOMMU); c++)
        if ((info.core[cores[c]].mmu_status[0] | info.core[cores[c]].mmu_status[1]) & 1) {
            fprintf(stderr, "m3: core %u has IOMMU paging on; bypass mode needs it off\n", cores[c]);
            return 1;
        }
    if (info.m6_version)
        printf("rknputest m3: memory %s, buffers %s\n", info.mem_mode == RKNPU_MEM_IOMMU ? "IOMMU" : "bypass",
               info.bo_cached ? "cached" : "write-combined");

    for (c = 0; c < ncores; c++) {
        if (sm) {
            rc = m3_run(cores[c], sm, sk, sn, iters, map);
            if (rc)
                goto done;
            continue;
        }
        for (i = 0; i < sizeof(m3_ladder) / sizeof(m3_ladder[0]); i++) {
            rc = m3_run(cores[c], m3_ladder[i].m, m3_ladder[i].k, m3_ladder[i].n, iters, map && i == 0);
            if (rc)
                goto done;
        }
    }
    rc = 0;
    if (reset) {
        for (c = 0; c < ncores && !rc; c++) {
            if (m3_reset(cores[c])) {
                printf("  the reset did not come back clean\n");
                rc = 4;
                break;
            }
            rc = m3_run(cores[c], m3_ladder[0].m, m3_ladder[0].k, m3_ladder[0].n, 1, 0);
        }
    }
done:
    if (dev_info(&info) == 0)
        for (c = 0; c < RKNPU_NUM_CORES; c++)
            printf("core %u totals: %u submitted, %u completed, %u timed out, %u resets, isr %u unclaimed %u\n", c,
                   info.core[c].jobs_submitted, info.core[c].jobs_completed, info.core[c].jobs_timed_out,
                   info.core[c].resets, info.core[c].isr_count, info.core[c].isr_unclaimed);
    printf("%s\n", rc ? "RKNPU M3: FAILED" : "RKNPU M3: PASS");
    return rc;
}

/* ---------------------------------------------------------------- m6 */

/*
 * rknputest m6 [--mb N] [--iters N] [--fault] [--no-parallel]
 *
 * M6 on the board, in the order a failure would want to be found:
 *
 *   1. scatter: CREATE_BO 4 MiB buffers until --mb MiB (default 768) are
 *      held, each written at both ends and read back through the driver.
 *      M4 failed its 4 MiB buffers from 80 to 370 MiB held ("no contiguous
 *      memory below 4 GiB"); in IOMMU mode every one must succeed.
 *   2. the M3 ladder on both cores, exact, through the page table.
 *   3. latency: 1x64x64 (one task, the cheapest job) --iters times per core,
 *      reported as SUBMIT to WAIT return: the per-job cost charsiu pays.
 *   4. both cores at once: 16x1024x1024 on core 0 and core 1 from two
 *      threads with a handle each, against the same work one core after the
 *      other. A ratio near 2 says the driver no longer serialises the cores.
 *      The cores share the CBUF, so core c's streams use CBUF window c
 *      (charsiu's device d uses window d); with --collide both use window 0,
 *      which corrupts both on the hardware unless the driver makes the two
 *      jobs wait for each other (it must: exact, ratio near 1, CBUF waits).
 *   5. --fault: a job whose output runs into a buffer's guard page must end
 *      in RKNPU_JOB_MMU_FAULT, and the core must then run the 1x64x64 job
 *      exactly again (the recovery path).
 */

struct m6_par {
    unsigned int core, iters, win;
    int rc;
    double avg_us;
};

#ifdef _WIN32
static DWORD WINAPI m6_thread(LPVOID p)
{
    struct m6_par *a = (struct m6_par *)p;

    a->rc = 1;
    if (dev_open())
        return 0;
    a->rc = m3_run_ex(a->core, 16, 1024, 1024, a->iters, 0, 0, &a->avg_us, a->win);
    CloseHandle(g_dev);
    g_dev = INVALID_HANDLE_VALUE;
    return 0;
}
#elif defined(RKNPU_FAKE_DEVICE)
/* the same two threads against fakedev, whose jobs overlap as the board's do */
static void *m6_thread(void *p)
{
    struct m6_par *a = (struct m6_par *)p;

    a->rc = 1;
    if (dev_open())
        return NULL;
    a->rc = m3_run_ex(a->core, 16, 1024, 1024, a->iters, 0, 0, &a->avg_us, a->win);
    return NULL;
}
#endif

static int m6_scatter(unsigned int mb)
{
    unsigned int want = mb / 4, got = 0, i, fails = 0;
    struct m3_bo *bos = (struct m3_bo *)calloc(want ? want : 1, sizeof(*bos));
    rknpu_info info;
    double t0, t;
    int rc = 0;

    if (!bos)
        return 1;
    printf("scatter: %u buffers of 4 MiB (%u MiB)\n", want, want * 4);
    fflush(stdout);
    t0 = now_us();
    for (i = 0; i < want; i++) {
        unsigned int head[4], tail[4], back[4], j;

        if (bo_create(4u << 20, &bos[i])) {
            printf("  buffer %u (%u MiB held) FAILED\n", i, i * 4);
            rc = 5;
            break;
        }
        got++;
        for (j = 0; j < 4; j++) {
            head[j] = 0x6D360000u + i * 16 + j;
            tail[j] = ~head[j];
        }
        if (bo_io(IOCTL_RKNPU_WRITE_BO, &bos[i], 0, head, sizeof(head)) ||
            bo_io(IOCTL_RKNPU_WRITE_BO, &bos[i], (4u << 20) - sizeof(tail), tail, sizeof(tail)) ||
            bo_io(IOCTL_RKNPU_READ_BO, &bos[i], 0, back, sizeof(back)) || memcmp(back, head, sizeof(back)) ||
            bo_io(IOCTL_RKNPU_READ_BO, &bos[i], (4u << 20) - sizeof(back), back, sizeof(back)) ||
            memcmp(back, tail, sizeof(back)))
            fails++;
    }
    t = now_us() - t0;
    if (dev_info(&info) == 0)
        printf("  %u of %u allocated in %.0f ms (%.0f us each), %u read-back mismatches; driver: %u buffers, "
               "%u KiB, NPU address space %u KiB, %u page-table pages\n", got, want, t / 1e3,
               got ? t / got : 0.0, fails, info.bo_count, info.bo_kib, info.iova_kib, info.pt_pages);
    for (i = 0; i < got; i++) {
        if (i && bos[i].addr < bos[i - 1].addr + bos[i - 1].size + 4096u && bos[i].addr > bos[i - 1].addr) {
            printf("  buffers %u and %u have no guard page between them\n", i - 1, i);
            rc = 5;
        }
    }
    t0 = now_us();
    for (i = 0; i < got; i++)
        bo_free(&bos[i]);
    printf("  freed in %.0f ms\n", (now_us() - t0) / 1e3);
    free(bos);
    if (fails)
        rc = 5;
    printf("  %s\n", rc ? "SCATTER FAILED" : "SCATTER OK");
    return rc;
}

static int cmd_m6(int argc, char **argv)
{
    unsigned int mb = 768, iters = 200, c, i;
    int fault = 0, parallel = 1, collide = 0, a, rc = 0, r;
    double lat[RKNPU_NUM_CORES] = { 0, 0 };
    rknpu_info info;

    for (a = 0; a < argc; a++) {
        if (!strcmp(argv[a], "--mb") && a + 1 < argc) {
            if (parse_u32(argv[++a], &mb) || mb > 1536) {
                fprintf(stderr, "m6: --mb 0..1536\n");
                return 1;
            }
        } else if (!strcmp(argv[a], "--iters") && a + 1 < argc) {
            if (parse_u32(argv[++a], &iters) || iters == 0 || iters > 10000) {
                fprintf(stderr, "m6: --iters 1..10000\n");
                return 1;
            }
        } else if (!strcmp(argv[a], "--fault")) {
            fault = 1;
        } else if (!strcmp(argv[a], "--no-parallel")) {
            parallel = 0;
        } else if (!strcmp(argv[a], "--collide")) {
            collide = 1;
        } else {
            fprintf(stderr, "m6: unknown option %s\n", argv[a]);
            return 1;
        }
    }
    if (dev_open() || dev_info(&info))
        return 1;
    printf("rknputest m6: driver m6 version %u, memory %s, buffers %s\n", info.m6_version,
           info.mem_mode == RKNPU_MEM_IOMMU ? "IOMMU" : "bypass", info.bo_cached ? "cached" : "write-combined");
    if (!info.m6_version || info.mem_mode != RKNPU_MEM_IOMMU) {
        fprintf(stderr, "m6: the driver is not in IOMMU mode (an M4 build, or Device Parameters\\M6Iommu = 0)\n");
        return 1;
    }

    if (mb && (r = m6_scatter(mb)) != 0)
        rc = r;

    for (c = 0; c < RKNPU_NUM_CORES && !rc; c++)
        for (i = 0; i < sizeof(m3_ladder) / sizeof(m3_ladder[0]) && !rc; i++)
            rc = m3_run(c, m3_ladder[i].m, m3_ladder[i].k, m3_ladder[i].n, 3, i == 0);

    for (c = 0; c < RKNPU_NUM_CORES && !rc; c++) {
        printf("latency, core %u:\n", c);
        rc = m3_run_ex(c, 1, 64, 64, iters, 0, 0, &lat[c], c);
    }

#if defined(_WIN32) || defined(RKNPU_FAKE_DEVICE)
    if (!rc && parallel) {
        struct m6_par p[RKNPU_NUM_CORES];
#ifdef _WIN32
        HANDLE th[RKNPU_NUM_CORES];
#else
        pthread_t th[RKNPU_NUM_CORES];
#endif
        unsigned int piters = iters / 4 ? iters / 4 : 1, started = 0;
        double t0, seq, par;

        printf("one core after the other, 16x1024x1024 x %u each:\n", piters);
        t0 = now_us();
        for (c = 0; c < RKNPU_NUM_CORES && !rc; c++)
            rc = m3_run_ex(c, 16, 1024, 1024, piters, 0, 0, NULL, c);
        seq = now_us() - t0;
        printf("both cores at once, two handles, CBUF windows %s:\n", collide ? "0 and 0 (--collide)" : "0 and 1");
        fflush(stdout);
        t0 = now_us();
        for (c = 0; c < RKNPU_NUM_CORES && !rc; c++) {
            p[c].core = c;
            p[c].iters = piters;
            /* --collide: both on window 0, the arrangement that corrupts on
             * the hardware; the driver must make the two wait for each other */
            p[c].win = collide ? 0 : c;
            p[c].rc = 1;
#ifdef _WIN32
            th[c] = CreateThread(NULL, 0, m6_thread, &p[c], 0, NULL);
            if (!th[c])
                rc = 1;
#else
            if (pthread_create(&th[c], NULL, m6_thread, &p[c]))
                rc = 1;
#endif
            if (!rc)
                started++;
        }
#ifdef _WIN32
        if (started)
            WaitForMultipleObjects(started, th, TRUE, INFINITE);
        for (c = 0; c < started; c++)
            CloseHandle(th[c]);
#else
        for (c = 0; c < started; c++)
            pthread_join(th[c], NULL);
#endif
        for (c = 0; c < started; c++)
            if (p[c].rc) {
                printf("  core %u's thread FAILED (%d)\n", c, p[c].rc);
                rc = 5;
            }
        par = now_us() - t0;
        if (!rc)
            printf("PARALLEL%s: sequential %.0f ms, both cores %.0f ms, %.2fx\n", collide ? " (same window)" : "",
                   seq / 1e3, par / 1e3, par > 0 ? seq / par : 0.0);
    }
#else
    (void)parallel;
    (void)collide;
#endif

    if (!rc && fault) {
        printf("fault: a job that writes into a guard page, on core 0\n");
        rc = m3_run_ex(0, 1, 64, 64, 1, 0, 1, NULL, 0);
        if (!rc) {
            printf("  and the same core again, normally:\n");
            rc = m3_run(0, 1, 64, 64, 3, 0);
        }
    }

    if (dev_info(&info) == 0) {
        for (c = 0; c < RKNPU_NUM_CORES; c++)
            printf("core %u totals: %u submitted, %u completed, %u timed out, %u resets, %u IOMMU enables, "
                   "%u faults, %u CBUF waits, isr %u unclaimed %u\n", c, info.core[c].jobs_submitted,
                   info.core[c].jobs_completed, info.core[c].jobs_timed_out, info.core[c].resets,
                   info.core[c].mmu_enables, info.core[c].mmu_faults, info.core[c].cbuf_waits,
                   info.core[c].isr_count, info.core[c].isr_unclaimed);
        if (!rc && !fault && (info.core[0].mmu_faults || info.core[1].mmu_faults)) {
            printf("an IOMMU fault was counted with no --fault run\n");
            rc = 5;
        }
    }
    if (!rc)
        printf("LATENCY: SUBMIT to WAIT return, 1x64x64, avg %.0f us core 0, %.0f us core 1\n", lat[0], lat[1]);
    printf("%s\n", rc ? "RKNPU M6: FAILED" : "RKNPU M6: PASS");
    return rc;
}

/* ---------------------------------------------------------- selftest */

/*
 * The m3 pieces that need no device: every captured stream, relocated into
 * made-up buffers, passes the SUBMIT rules and carries the addresses it was
 * given; a stream pointed outside its buffers, or one that writes the PC
 * block, fails them. (That the streams and packing equal charsiu's is
 * tools/m3gen's check, which links charsiu.)
 */
static int m3_selftest(void)
{
    static unsigned long long e[4096];
    struct m3_bo bos[4] = {
        { 1, 0x50000000u, 0x00100000u, 0 }, { 2, 0x50200000u, 0x00200000u, 0 },
        { 3, 0x50400000u, 0x00010000u, 0 }, { 4, 0x50800000u, 0x00080000u, 0 },
    };
    unsigned int base[M3_NBUF], i, j;
    int bad = 0;

    base[M3_BUF_INPUT] = bos[0].addr + 0x1000;
    base[M3_BUF_WEIGHT] = bos[1].addr;
    base[M3_BUF_OUTPUT] = bos[2].addr + 0x2000;
    base[M3_BUF_COEF] = bos[3].addr;
    for (i = 0; i < 2 * sizeof(m3_shapes) / sizeof(m3_shapes[0]); i++) {
        const unsigned int ns = sizeof(m3_shapes) / sizeof(m3_shapes[0]);
        const struct m3_shape *s = i < ns ? &m3_shapes[i] : &m3_shapes_w1[i - ns];
        unsigned int seen = 0;

        if (s->count + 1 > sizeof(e) / sizeof(e[0]) || s->count > RKNPU_MAX_REGCMD) {
            printf("  m3 shape %ux%u: %u entries is too many\n", s->k, s->n, s->count);
            bad = 1;
            continue;
        }
        m3_build_stream(s, base, e);
        for (j = 0; j < s->nrel; j++) {
            unsigned int v = RKNPU_RC_VALUE(e[s->rel[j].index]);
            if (v != base[s->rel[j].buf] + s->rel[j].offset || !rknpu_rc_is_addr_reg(RKNPU_RC_REG(e[s->rel[j].index])))
                bad = 1;
            seen |= 1u << s->rel[j].buf;
        }
        if (seen != (1u << M3_NBUF) - 1 || !m3_stream_ok(e, s->count, bos, 4)) {
            printf("  m3 shape %ux%u: relocated stream fails the SUBMIT rules\n", s->k, s->n);
            bad = 1;
        }
        /* the output moved past every buffer must be refused */
        e[s->rel[2].index] = RKNPU_RC_ENTRY(RKNPU_RC_DPU, RKNPU_RC_DPU_DST_ADDR, 0x60000000u);
        if (m3_stream_ok(e, s->count, bos, 4)) {
            printf("  m3 shape %ux%u: an address outside the buffers passed\n", s->k, s->n);
            bad = 1;
        }
        m3_build_stream(s, base, e);
        e[s->count - 1] = RKNPU_RC_ENTRY(RKNPU_RC_CNA, 0x0010, 0x50000000u);  /* PC_BASE_ADDRESS */
        if (m3_stream_ok(e, s->count, bos, 4)) {
            printf("  m3 shape %ux%u: a PC block write passed\n", s->k, s->n);
            bad = 1;
        }
    }
    /* M6: the two windows' streams carry different CBUF keys, so the driver
     * lets them run together, and the same window collides */
    for (i = 0; i < sizeof(m3_shapes) / sizeof(m3_shapes[0]); i++) {
        unsigned int k0 = rknpu_rc_cbuf_key(m3_shapes[i].entries, m3_shapes[i].count);
        unsigned int k1 = rknpu_rc_cbuf_key(m3_shapes_w1[i].entries, m3_shapes_w1[i].count);

        if (k0 == RKNPU_RC_CBUF_ANY || k1 == RKNPU_RC_CBUF_ANY || rknpu_rc_cbuf_collide(k0, k1) ||
            !rknpu_rc_cbuf_collide(k0, k0)) {
            printf("  m3 shape %ux%u: CBUF keys 0x%08x / 0x%08x do not separate the windows\n",
                   m3_shapes[i].k, m3_shapes[i].n, k0, k1);
            bad = 1;
        }
    }
    printf("m3: %u captured shapes checked, in both CBUF windows\n",
           (unsigned)(sizeof(m3_shapes) / sizeof(m3_shapes[0])));
    return bad;
}

static int cmd_selftest(void)
{
    static const struct { unsigned int off, n; int ok; } cases[] = {
        { 0x0000, 1, 1 },    { 0x0000, 1024, 1 }, { 0x0000, 1025, 0 }, { 0x0002, 1, 0 },
        { 0x0ffc, 1, 1 },    { 0x0ffc, 2, 0 },    { 0x1000, 1024, 1 }, { 0x2000, 64, 1 },
        { 0x2000, 65, 0 },   { 0x20fc, 1, 1 },    { 0x2100, 64, 1 },   { 0x21fc, 2, 0 },
        { 0x2200, 1, 0 },    { 0x2ffc, 1, 0 },    { 0x3000, 1024, 1 }, { 0x4000, 1, 0 },
        { 0x7ffc, 1, 0 },    { 0x8000, 1, 1 },    { 0xa100, 64, 1 },   { 0xb000, 1024, 1 },
        { 0xc000, 1, 0 },    { 0xf008, 1, 0 },    { 0x10000, 1, 0 },   { 0x0000, 0, 0 },
        { 0xfffffffc, 1, 0 }, { 0x3ffc, 0x40000000, 0 },
    };
    unsigned int i;
    int bad = 0;

    printf("sizeof: rknpu_info %u, rknpu_core_info %u, rknpu_clock_info %u, rknpu_read_regs_in %u\n",
           (unsigned)sizeof(rknpu_info), (unsigned)sizeof(rknpu_core_info),
           (unsigned)sizeof(rknpu_clock_info), (unsigned)sizeof(rknpu_read_regs_in));
    printf("IOCTL_RKNPU_GET_INFO 0x%08x, IOCTL_RKNPU_READ_REGS 0x%08x\n",
           (unsigned)IOCTL_RKNPU_GET_INFO, (unsigned)IOCTL_RKNPU_READ_REGS);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = rknpu_read_range_ok(cases[i].off, cases[i].n);
        if (got != cases[i].ok) {
            printf("  range 0x%x + %u: got %d, want %d\n", cases[i].off, cases[i].n, got, cases[i].ok);
            bad = 1;
        }
    }
    bad |= m3_selftest();
    printf("%s\n", bad ? "SELFTEST FAILED" : "SELFTEST OK");
    return bad;
}

/* -------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
            "usage: rknputest info\n"
            "       rknputest regs <offset> <count>\n"
            "       rknputest selftest\n"
            "       rknputest m3 [--core 0|1|both] [--iters N] [--shape M K N] [--map] [--no-reset]\n"
            "       rknputest m6 [--mb N] [--iters N] [--fault] [--no-parallel] [--collide]\n");
}

int main(int argc, char **argv)
{
    int rc;

    if (argc == 2 && !strcmp(argv[1], "info"))
        rc = cmd_info();
    else if (argc == 4 && !strcmp(argv[1], "regs"))
        rc = cmd_regs(argv[2], argv[3]);
    else if (argc == 2 && !strcmp(argv[1], "selftest"))
        rc = cmd_selftest();
    else if (argc >= 2 && !strcmp(argv[1], "m3"))
        rc = cmd_m3(argc - 2, argv + 2);
    else if (argc >= 2 && !strcmp(argv[1], "m6"))
        rc = cmd_m6(argc - 2, argv + 2);
    else {
        usage();
        return 1;
    }
#ifdef _WIN32
    if (g_dev != INVALID_HANDLE_VALUE)
        CloseHandle(g_dev);
#endif
    return rc;
}
