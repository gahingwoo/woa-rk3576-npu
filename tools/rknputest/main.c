/*
 * rknputest - talks to rknpu.sys (\\.\RKNPU). Run as administrator.
 *
 *   rknputest info              driver and hardware state, with checks
 *   rknputest regs <off> <n>    read n words at byte offset off (hex or
 *                               decimal) of the NPU register window
 *   rknputest selftest          interface layout and range checks; needs no
 *                               device and runs on any host (CI runs it on
 *                               Linux)
 *
 * Exit status: 0 ok, 1 usage or device error, 2 a check in `info` failed.
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

#include "rknpu_ioctl.h"

#define EXPECT_DSU0_HZ  594000000u

static const unsigned int expect_gsiv[RKNPU_NUM_CORES] = { 279, 280 };

/* ------------------------------------------------------------ device */

#ifdef _WIN32
static HANDLE g_dev = INVALID_HANDLE_VALUE;

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
    }

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
        snprintf(line, sizeof(line), "core %u IOMMUs have paging off", n);
        check(!(c->mmu_status[0] & 1) && !(c->mmu_status[1] & 1), line);
        snprintf(line, sizeof(line), "core %u interrupt is GSIV %u", n, expect_gsiv[n]);
        check(c->gsiv == expect_gsiv[n], line);
        snprintf(line, sizeof(line), "core %u no interrupt seen (no job was started)", n);
        check(c->isr_count == 0 && c->isr_unclaimed == 0, line);
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

/* ---------------------------------------------------------- selftest */

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
    printf("%s\n", bad ? "SELFTEST FAILED" : "SELFTEST OK");
    return bad;
}

/* -------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
            "usage: rknputest info\n"
            "       rknputest regs <offset> <count>\n"
            "       rknputest selftest\n");
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
