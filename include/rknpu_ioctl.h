/*
 * rknpu_ioctl.h - the interface between rknpu.sys and user mode
 *
 * Shared by the driver (drivers/rknpu), the test tool (tools/rknputest) and,
 * from M4, charsiu's device_win.c. Include after <windows.h> or <ntddk.h>;
 * on any other host it defines what it needs itself, so the tool and the
 * layout checks also build with gcc on Linux.
 *
 * M2 implements GET_INFO and READ_REGS, both read only. The function codes
 * from 0x810 on are reserved for M3 (buffers, submission, completion); the
 * driver answers them with STATUS_INVALID_DEVICE_REQUEST until then. Their
 * structures will copy include/uapi/drm/rocket_accel.h (MIT) field for
 * field, as docs/FEASIBILITY.md section 2.1 says; nothing for them is
 * defined here yet, so nothing can come to depend on a guess.
 *
 * Every structure is fixed size, uses 32- and 64-bit fields only, and is
 * checked by the size assertions at the end of this file. A change to any
 * of them bumps RKNPU_INTERFACE_VERSION.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RKNPU_IOCTL_H
#define RKNPU_IOCTL_H

/* \\.\RKNPU, administrators and SYSTEM only */
#define RKNPU_NT_NAME           L"\\Device\\RKNPU"
#define RKNPU_DOS_NAME          L"\\DosDevices\\RKNPU"
#define RKNPU_USER_PATH         L"\\\\.\\RKNPU"

#define RKNPU_INTERFACE_VERSION 1

#ifndef CTL_CODE
#define CTL_CODE(t, f, m, a) (((t) << 16) | ((a) << 14) | ((f) << 2) | (m))
#define METHOD_BUFFERED   0
#define FILE_ANY_ACCESS   0
#define FILE_READ_ACCESS  1
#define FILE_WRITE_ACCESS 2
#endif

#define RKNPU_DEVICE_TYPE       0x8576u     /* unsigned: << 16 does not fit an int */

/* ---- M2: implemented ---------------------------------------------------- */

#define RKNPU_FN_GET_INFO       0x800
#define RKNPU_FN_READ_REGS      0x801

#define IOCTL_RKNPU_GET_INFO    CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_GET_INFO, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RKNPU_READ_REGS   CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_READ_REGS, METHOD_BUFFERED, FILE_READ_ACCESS)

/* ---- M3: RESERVED, not implemented -------------------------------------- */
/*
 * Function codes only, so that nothing else takes them. No IOCTL_ code and
 * no structure exists for these yet; M3 defines both. The rocket call each
 * one follows is named beside it.
 */
#define RKNPU_FN_CREATE_BO      0x810   /* RESERVED (M3): DRM_ROCKET_CREATE_BO + mmap */
#define RKNPU_FN_FREE_BO        0x811   /* RESERVED (M3): DRM_IOCTL_GEM_CLOSE */
#define RKNPU_FN_PREP_BO        0x812   /* RESERVED (M3): DRM_ROCKET_PREP_BO, by range */
#define RKNPU_FN_FINI_BO        0x813   /* RESERVED (M3): DRM_ROCKET_FINI_BO, by range */
#define RKNPU_FN_SUBMIT         0x814   /* RESERVED (M3): DRM_ROCKET_SUBMIT */
#define RKNPU_FN_WAIT           0x815   /* RESERVED (M3): wait for a job sequence number */
#define RKNPU_FN_RESET          0x816   /* RESERVED (M3): core soft reset through _DSM function 1 */
#define RKNPU_FN_RESERVED_LAST  0x81F   /* 0x817..0x81F: RESERVED for M3 and M5 */

#pragma pack(push, 8)

/* ---- GET_INFO ----------------------------------------------------------- */

#define RKNPU_NUM_CORES         2
#define RKNPU_MMUS_PER_CORE     2

/* rknpu_info.flags */
#define RKNPU_INFO_INTERRUPTS_CONNECTED 0x00000001u  /* D0, interrupts enabled */
#define RKNPU_INFO_CLOCKS_VALID         0x00000002u  /* the clocks block below was read */
#define RKNPU_INFO_DOMAINS_ON           0x00000004u  /* the same test as the SSDT's _STA */

/* One core, read at the time of the call (all reads, no writes). */
typedef struct rknpu_core_info {
    unsigned int pc_version;            /* PC 0x0000 */
    unsigned int pc_version_num;        /* PC 0x0004 */
    unsigned int pc_operation_enable;   /* PC 0x0008 */
    unsigned int pc_interrupt_mask;     /* PC 0x0020 */
    unsigned int pc_interrupt_status;   /* PC 0x0028 */
    unsigned int pc_interrupt_raw;      /* PC 0x002C */
    unsigned int pc_task_con;           /* PC 0x0030 */
    unsigned int pc_task_status;        /* PC 0x003C */

    /* the core's two IOMMU instances, +0x2000 and +0x2100 */
    unsigned int mmu_status[RKNPU_MMUS_PER_CORE];      /* 0x04, bit 0 = paging */
    unsigned int mmu_dte_addr[RKNPU_MMUS_PER_CORE];    /* 0x00 */
    unsigned int mmu_int_rawstat[RKNPU_MMUS_PER_CORE]; /* 0x14 */
    unsigned int mmu_int_mask[RKNPU_MMUS_PER_CORE];    /* 0x1C */

    /* the interrupt, as the driver saw it */
    unsigned int gsiv;                  /* raw resource vector (expected 279 / 280) */
    unsigned int isr_count;             /* ISR calls that claimed the interrupt */
    unsigned int isr_unclaimed;         /* ISR calls that found nothing to ack */
    unsigned int dpc_count;
    unsigned int last_pc_status;        /* PC INTERRUPT_STATUS at the last claim */
    unsigned int last_mmu_int_status[RKNPU_MMUS_PER_CORE];
    unsigned int last_mmu_fault_addr[RKNPU_MMUS_PER_CORE];

    unsigned int reserved[8];
} rknpu_core_info;

/*
 * Power and clock state, read from the PMU and the CRU. Neither is in the
 * device's _CRS; the driver maps one page of each read only, and reads them
 * only for GET_INFO. RKNPU_INFO_CLOCKS_VALID is clear if it could not.
 */
typedef struct rknpu_clock_info {
    unsigned int pmu_status0;           /* 0x27380230, bit 0 = PD_NPU off */
    unsigned int pmu_repair0;           /* 0x27380570, bits 22..24 = NPUTOP, NPU0, NPU1 on */
    unsigned int cru_clksel86;          /* 0x27200458, clk_rknn_dsu0 mux [8:7], div [6:2] */
    unsigned int cru_gate28;            /* 0x27200870, bit 9 aclk_rknn0 (1 = gated) */
    unsigned int cru_gate29;            /* 0x27200874, bit 0 aclk_rknn1 */
    unsigned int cru_gate31;            /* 0x2720087C, bits 4, 5, 8 */
    unsigned int cru_gate32;            /* 0x27200880, bits 0, 5, 12 */
    unsigned int gpll_hz;               /* from GPLL_CON0..2 and MODE_CON00 */
    unsigned int dsu0_hz;               /* clk_rknn_dsu0; 0 unless its parent is GPLL */
    unsigned int reserved[7];
} rknpu_clock_info;

typedef struct rknpu_info {
    unsigned int interface_version;     /* RKNPU_INTERFACE_VERSION */
    unsigned int struct_size;           /* sizeof(rknpu_info) */
    unsigned int flags;                 /* RKNPU_INFO_* */
    unsigned int core_count;            /* RKNPU_NUM_CORES */
    unsigned long long regs_pa;         /* the _CRS memory resource */
    unsigned int regs_len;
    unsigned int reserved0;
    rknpu_core_info core[RKNPU_NUM_CORES];
    rknpu_clock_info clocks;
    unsigned int reserved[16];          /* M3: queue depth, seqno page, ... */
} rknpu_info;

/* ---- READ_REGS ---------------------------------------------------------- */

/*
 * Read count 32-bit words starting at offset, a byte offset into the
 * 64 KiB register window. The whole range must lie inside one of the blocks
 * the device tree describes, for one core (core 1 is the same at +0x8000):
 *
 *   0x0000..0x0FFF  pc       0x2000..0x20FF  mmu instance 0
 *   0x1000..0x1FFF  cna      0x2100..0x21FF  mmu instance 1
 *   0x3000..0x3FFF  core
 *
 * Everything else in the window (0x2200..0x2FFF, 0x4000..0x7FFF and their
 * core 1 copies) is refused: it is not described anywhere, and a read that
 * decodes to nothing can be an SError on this SoC. The driver never writes.
 */
#define RKNPU_REGS_WINDOW       0x10000u
#define RKNPU_MAX_READ_WORDS    1024u   /* 4 KiB, one block */

typedef struct rknpu_read_regs_in {
    unsigned int offset;                /* bytes, 4-aligned */
    unsigned int count;                 /* words, 1..RKNPU_MAX_READ_WORDS */
} rknpu_read_regs_in;
/* output: count words */

#pragma pack(pop)

/*
 * The READ_REGS rule above, as code: the driver enforces it, the tool checks
 * it first for a clearer message, and its selftest exercises it on any host.
 */
#ifdef _MSC_VER
#define RKNPU_INLINE static __inline
#else
#define RKNPU_INLINE static inline
#endif

RKNPU_INLINE int rknpu_read_range_ok(unsigned int offset, unsigned int count)
{
    static const unsigned int start[5] = { 0x0000, 0x1000, 0x2000, 0x2100, 0x3000 };
    static const unsigned int len[5]   = { 0x1000, 0x1000, 0x0100, 0x0100, 0x1000 };
    unsigned int rel, i;

    if ((offset & 3) || count == 0 || count > RKNPU_MAX_READ_WORDS || offset >= RKNPU_REGS_WINDOW)
        return 0;
    rel = offset % 0x8000u;             /* core 1 is core 0 at +0x8000 */
    for (i = 0; i < 5; i++)
        if (rel >= start[i] && rel - start[i] + count * 4 <= len[i])
            return 1;
    return 0;
}

/* Layout checks: the same on every compiler that builds this header. */
#define RKNPU_ASSERT_SIZE(t, n) typedef char rknpu_size_check_##t[(sizeof(t) == (n)) ? 1 : -1]
RKNPU_ASSERT_SIZE(rknpu_core_info, 132);
RKNPU_ASSERT_SIZE(rknpu_clock_info, 64);
RKNPU_ASSERT_SIZE(rknpu_info, 424);
RKNPU_ASSERT_SIZE(rknpu_read_regs_in, 8);

#endif /* RKNPU_IOCTL_H */
