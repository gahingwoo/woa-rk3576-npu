/*
 * rknpu_ioctl.h - the interface between rknpu.sys and user mode
 *
 * Shared by the driver (drivers/rknpu), the test tool (tools/rknputest) and,
 * from M4, charsiu's device_win.c. Include after <windows.h> or <ntddk.h>;
 * on any other host it defines what it needs itself, so the tool and the
 * layout checks also build with gcc on Linux.
 *
 * M2: GET_INFO and READ_REGS, both read only. M3 (function codes 0x810 on):
 * buffers, submission, completion and a core reset; see the M3 section. The
 * job structures follow include/uapi/drm/rocket_accel.h (MIT): a task is
 * the NPU address of a register command stream and its entry count, as in
 * struct drm_rocket_task.
 *
 * Every structure is fixed size, uses 32- and 64-bit fields only, and is
 * checked by the size assertions at the end of this file. A change to any
 * of them bumps RKNPU_INTERFACE_VERSION. M3 only gave names to fields that
 * were reserved (and read 0 from an M2 driver) and added structures, so the
 * version is still 1; RKNPU_M3_VERSION in rknpu_info says M3 is there.
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

/* ---- M3: buffers, submission, completion -------------------------------- */
/*
 * Everything below is new in M3 and is reached only through the function
 * codes below; GET_INFO and READ_REGS behave as in M2. Nothing in the driver
 * writes a job register, allocates a buffer or runs a _DSM until one of these
 * is called, so an installed M3 driver that only ever sees `rknputest info`
 * does what the M2 driver did.
 *
 * The model is rocket's (include/uapi/drm/rocket_accel.h, MIT, and
 * drivers/accel/rocket/rocket_job.c): buffers with a 32-bit NPU address, jobs
 * of tasks, each task one register command stream given by its NPU address
 * and its entry count. What differs, and why:
 *
 *   - Buffers are physically contiguous, below 4 GiB, and mapped
 *     write-combined (Normal non-cacheable on ARM64) in the kernel and, after
 *     MAP_BO, in the caller. The NPU IOMMUs are left as the firmware leaves
 *     them, paging off, so the NPU address of a buffer is its physical
 *     address. No cache maintenance is needed, so rocket's PREP_BO and
 *     FINI_BO have no M3 counterpart; their codes stay reserved for the
 *     cached buffers of M5.
 *   - WRITE_BO and READ_BO copy through the driver, so a test can run with
 *     no user mapping of NPU memory at all. MAP_BO is there, and optional.
 *   - SUBMIT returns at once with a sequence number; WAIT waits for it, with
 *     a timeout. A job that times out is stopped and its core is reset
 *     through the SSDT's _DSM (function 1) before WAIT returns.
 *   - One job per core at a time: no software queue yet (M5).
 */
#define RKNPU_M3_VERSION        1       /* rknpu_info.m3_version */

#define RKNPU_FN_CREATE_BO      0x810   /* DRM_ROCKET_CREATE_BO, no mmap */
#define RKNPU_FN_FREE_BO        0x811   /* DRM_IOCTL_GEM_CLOSE */
#define RKNPU_FN_PREP_BO        0x812   /* M6: DRM_ROCKET_PREP_BO, cached buffers (see M6) */
#define RKNPU_FN_FINI_BO        0x813   /* M6: DRM_ROCKET_FINI_BO */
#define RKNPU_FN_SUBMIT         0x814   /* DRM_ROCKET_SUBMIT, one job */
#define RKNPU_FN_WAIT           0x815   /* wait for a job's sequence number */
#define RKNPU_FN_RESET          0x816   /* core soft reset through _DSM function 1 */
#define RKNPU_FN_MAP_BO         0x817   /* map a buffer into the caller (mmap) */
#define RKNPU_FN_UNMAP_BO       0x818   /* undo MAP_BO */
#define RKNPU_FN_WRITE_BO       0x819   /* copy into a buffer */
#define RKNPU_FN_READ_BO        0x81A   /* copy out of a buffer */
#define RKNPU_FN_SUBMIT_STATS   0x81B   /* M8: where SUBMIT's time goes, per core */
#define RKNPU_FN_RESERVED_LAST  0x81F   /* 0x81C..0x81F: RESERVED */

/* ---- M4: what charsiu needs ---------------------------------------------- */
/*
 * No new function codes. M4 names reserved fields and adds flags, so an M3
 * caller sees the M3 driver; RKNPU_M4_VERSION in rknpu_info says M4 is there.
 *
 *   - SUBMIT with RKNPU_SUBMIT_COPY: charsiu writes its streams through the
 *     buffer's mapping, as it does under rocket, so the M3 rule (a stream's
 *     buffer may not be mapped) cannot hold. With this flag the driver copies
 *     every stream into a buffer of its own for the core (the copy arena,
 *     never mapped into any process), checks the COPY, and points the PC at
 *     the copy. What runs is what was checked, whatever the caller writes
 *     afterwards. The streams of one SUBMIT must fit the arena together:
 *     each takes its fetched length (rounded up to an even entry count) times
 *     8 bytes, and more than RKNPU_COPY_ARENA_BYTES in total is refused with
 *     STATUS_INVALID_BUFFER_SIZE (ERROR_INVALID_USER_BUFFER, 1784). charsiu's
 *     largest SUBMIT is 16 streams of 148 entries, 18944 bytes.
 *   - Because a copied stream cannot change after the check, the M3 rules
 *     that kept a checked stream fixed (MAP_BO and WRITE_BO refused while a
 *     job runs) now apply only while a job submitted WITHOUT the flag runs,
 *     and (M6) only to that job's own handle: no other handle can write or
 *     map those buffers anyway, so its WRITE_BO and MAP_BO go ahead.
 *     FREE_BO is refused only while a job of the same handle runs: a job can
 *     only reach its own handle's buffers through the registers checked.
 *   - WAIT with RKNPU_WAIT_POLL returns RKNPU_JOB_RUNNING if the job has not
 *     finished, and stops nothing. Without the flag WAIT is M3's.
 *   - A finished job stays answerable: WAIT finds it in a short history even
 *     after another handle's job has taken its core.
 *   - The address registers SUBMIT checks are every one charsiu's kernels
 *     write (rknpu_regcmd.h), not only the five of the M3 int8 matmul.
 */
#define RKNPU_M4_VERSION        1       /* rknpu_info.m4_version */
#define RKNPU_COPY_ARENA_BYTES  (256u << 10)    /* per core: 64 tasks of 512 entries */
#define RKNPU_JOB_HISTORY       32      /* finished jobs WAIT can still answer for */

/* ---- M6: the driver owns the NPU IOMMUs ---------------------------------- */
/*
 * No new layout: M6 names reserved fields, implements the two reserved codes
 * PREP_BO and FINI_BO, and adds flags. RKNPU_M6_VERSION in rknpu_info says it
 * is there; an M4 caller still works unchanged against an M6 driver.
 *
 *   - Memory mode (rknpu_info.mem_mode). RKNPU_MEM_IOMMU, the default: the
 *     driver builds one Rockchip IOMMU v2 page table (rockchip-iommu.c's
 *     format: 32-bit NPU addresses, 4 KiB pages, 40-bit physical) and points
 *     all four NPU IOMMU instances at it. A buffer is any pages Windows has,
 *     above 4 GiB too, and its dma_address is an NPU virtual address (IOVA).
 *     Nothing needs to be physically contiguous any more. RKNPU_MEM_BYPASS is
 *     M4: paging off, contiguous memory below 4 GiB, dma_address = physical.
 *     Device Parameters\M6Iommu = 0 selects it, read when the device starts.
 *   - Each buffer has its own IOVA range with at least one unmapped page
 *     after it, so a stream that runs past a buffer faults instead of landing
 *     in the next one. A fault stops the job (RKNPU_JOB_MMU_FAULT), and WAIT
 *     resets the core as it does on a timeout. SUBMIT still checks every
 *     stream as M4 does (allow list, PC block, every address register inside
 *     one of the caller's buffers): the table is shared by every handle, so
 *     the IOMMU bounds what the NPU can reach to what the driver mapped, and
 *     the check keeps one handle out of another's buffers.
 *   - Cached buffers (rknpu_create_bo.flags out: RKNPU_BO_CACHED). By default
 *     in IOMMU mode the CPU mappings, the driver's and MAP_BO's, are cached,
 *     as rocket's are. The NPU is not coherent, so the caller brackets CPU
 *     access as it does with rocket: FINI_BO after writing a buffer the NPU
 *     will read (cleans the range to memory), PREP_BO after a job that wrote
 *     it and before reading (cleans and invalidates). Neither waits for a
 *     job; wait first. On a buffer without RKNPU_BO_CACHED both are no-ops.
 *     WRITE_BO and READ_BO do the maintenance themselves. Device
 *     Parameters\M6CachedBos = 0 makes every buffer write-combined again
 *     (M4's model, no maintenance); in bypass mode that is the default.
 *   - More than one handle may be open (M4 allowed one), so charsiu's two
 *     devices drive both cores at once, and the driver's queue is parallel:
 *     a WAIT on one core no longer holds up a SUBMIT on the other.
 *   - The cores share the CBUF: a job only runs alongside the other core's
 *     job when their streams use different CBUF windows (CNA 0x1040,
 *     rknpu_regcmd.h); otherwise SUBMIT waits for the other job to finish
 *     (rknpu_core_info.cbuf_waits counts those), and returns
 *     STATUS_DEVICE_BUSY only if it does not finish in RKNPU_WAIT_MAX_MS.
 */
#define RKNPU_M6_VERSION        1       /* rknpu_info.m6_version */
#define RKNPU_MEM_BYPASS        0       /* rknpu_info.mem_mode: M4, physical addresses */
#define RKNPU_MEM_IOMMU         1       /* rknpu_info.mem_mode: M6, the driver's page table */

#define IOCTL_RKNPU_PREP_BO     CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_PREP_BO, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_RKNPU_FINI_BO     CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_FINI_BO, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#ifndef METHOD_IN_DIRECT
#define METHOD_IN_DIRECT  1
#define METHOD_OUT_DIRECT 2
#endif

#define IOCTL_RKNPU_CREATE_BO   CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_CREATE_BO, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_RKNPU_FREE_BO     CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_FREE_BO, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_RKNPU_SUBMIT      CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_SUBMIT, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_RKNPU_WAIT        CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_WAIT, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_RKNPU_RESET       CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_RESET, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_RKNPU_MAP_BO      CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_MAP_BO, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_RKNPU_UNMAP_BO    CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_UNMAP_BO, METHOD_BUFFERED, FILE_WRITE_ACCESS)
/* the data travel in the second buffer, described by an MDL (direct I/O) */
#define IOCTL_RKNPU_WRITE_BO    CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_WRITE_BO, METHOD_IN_DIRECT, FILE_WRITE_ACCESS)
#define IOCTL_RKNPU_READ_BO     CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_READ_BO, METHOD_OUT_DIRECT, FILE_READ_ACCESS)

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

    /* M3 (0 from an M2 driver): jobs on this core since the driver started */
    unsigned int jobs_submitted;
    unsigned int jobs_completed;        /* by the interrupt */
    unsigned int jobs_timed_out;
    unsigned int resets;                /* _DSM resets of this core */
    /* M6 (0 from an M4 driver) */
    unsigned int mmu_faults;            /* page faults and bus errors its IOMMUs raised */
    unsigned int mmu_enables;           /* times the driver turned paging on for this core */
    unsigned int cbuf_waits;            /* SUBMITs that waited for the other core's job on the
                                         * same CBUF window (rknpu_regcmd.h, the shared CBUF) */
    unsigned int reserved[1];
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
    unsigned int m3_version;            /* RKNPU_M3_VERSION; 0 from an M2 driver */
    unsigned int bo_count;              /* buffers allocated now, all handles */
    unsigned int bo_kib;                /* their size */
    /* M4 (0 from an M3 driver) */
    unsigned int m4_version;            /* RKNPU_M4_VERSION */
    unsigned int copy_arena_bytes;      /* RKNPU_COPY_ARENA_BYTES */
    unsigned int bo_max_kib;            /* RKNPU_BO_MAX_SIZE / 1024 */
    unsigned int bo_max_total_kib;      /* RKNPU_BO_MAX_TOTAL / 1024, or the IOMMU mode's */
    /* M6 (0 from an M4 driver) */
    unsigned int m6_version;            /* RKNPU_M6_VERSION */
    unsigned int mem_mode;              /* RKNPU_MEM_* */
    unsigned int bo_cached;             /* 1: new buffers are cached (RKNPU_BO_CACHED) */
    unsigned int iova_kib;              /* NPU address space in use, guard pages included */
    unsigned int pt_pages;              /* page-table pages the driver holds (directory included) */
    /* M7 (0 from an M6 driver) */
    unsigned int m7_version;            /* RKNPU_M7_VERSION */
    /* M8 (0 from an M7 driver) */
    unsigned int m8_version;            /* RKNPU_M8_VERSION */
    unsigned int reserved[2];
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

/* ---- M3 structures ------------------------------------------------------ */

#define RKNPU_BO_MAX_SIZE       (64u << 20)     /* one buffer */
#define RKNPU_BO_MAX_TOTAL      (1024u << 20)   /* all buffers of the device; M3 had 512 MiB,
                                                 * Llama-3.2-1B through charsiu needs about
                                                 * 700 MiB at 512 tokens of context, 790 at 2048 */
#define RKNPU_BO_MAX_TOTAL_IOMMU (1536u << 20)  /* M6, IOMMU mode: no contiguous memory is needed,
                                                 * so the int8 Llama-3.2-1B (1379 MiB) fits */
#define RKNPU_BO_IO_MAX         (16u << 20)     /* one WRITE_BO or READ_BO */
#define RKNPU_MAX_TASKS         64              /* per job */
#define RKNPU_MAX_REGCMD        131072u         /* entries per task: REGISTER_AMOUNTS is 16 bits of pairs */

/* CREATE_BO, in and out. */
typedef struct rknpu_create_bo {
    unsigned int size;                  /* in: bytes; out: rounded up to 4 KiB */
    unsigned int flags;                 /* in: 0; out: RKNPU_BO_* (M6) */
    unsigned int handle;                /* out: nonzero */
    unsigned int dma_address;           /* out: the NPU's address: physical in bypass mode,
                                         * an IOVA in IOMMU mode (M6); never 0 */
} rknpu_create_bo;

/* rknpu_create_bo.flags, out (M6) */
#define RKNPU_BO_CACHED         0x00000001u /* cached mappings: PREP_BO and FINI_BO are needed */

/* PREP_BO and FINI_BO, in (M6). length 0 is the whole buffer (offset 0). */
typedef struct rknpu_bo_sync {
    unsigned int handle;
    unsigned int flags;                 /* 0 */
    unsigned int offset;                /* bytes into the buffer */
    unsigned int length;                /* bytes; 0 = to the end */
} rknpu_bo_sync;

/* FREE_BO and UNMAP_BO, in. FREE_BO also undoes MAP_BO. */
typedef struct rknpu_bo_handle {
    unsigned int handle;
    unsigned int reserved;
} rknpu_bo_handle;

/* MAP_BO, in and out: the whole buffer, write-combined, in the caller. */
typedef struct rknpu_map_bo {
    unsigned int handle;                /* in */
    unsigned int reserved;
    unsigned long long user_va;         /* out */
} rknpu_map_bo;

/*
 * WRITE_BO and READ_BO, in. The data are the other buffer of the call:
 * WRITE_BO reads `length` bytes from it into the buffer at `offset`,
 * READ_BO writes them to it. Both inside the buffer, at most RKNPU_BO_IO_MAX.
 */
typedef struct rknpu_bo_io {
    unsigned int handle;
    unsigned int offset;
    unsigned int length;
    unsigned int reserved;
} rknpu_bo_io;

/* struct drm_rocket_task: the stream's NPU address and its 64-bit entry count. */
/*
 * SUBMIT reads each stream first and fails with STATUS_ACCESS_VIOLATION
 * (ERROR_NOACCESS, 998) if an entry breaks the rules in rknpu_regcmd.h, and
 * with STATUS_INVALID_DEVICE_STATE (ERROR_BAD_COMMAND, 22) if the stream's
 * buffer is mapped with MAP_BO. The task is never started then.
 */
typedef struct rknpu_task {
    unsigned int regcmd;                /* 16-byte aligned, inside one of this handle's buffers */
    unsigned int regcmd_count;          /* 1..RKNPU_MAX_REGCMD */
} rknpu_task;

/* rknpu_submit_in.flags */
#define RKNPU_SUBMIT_NO_CRUMBS  0x00000001u /* skip the registry breadcrumbs (timing runs) */
#define RKNPU_SUBMIT_COPY       0x00000002u /* M4: run copies of the streams (see the M4 section) */

/* SUBMIT, in. Tasks run in order on one core, one interrupt each. */
typedef struct rknpu_submit_in {
    unsigned int core;                  /* 0 or 1 */
    unsigned int flags;                 /* RKNPU_SUBMIT_* */
    unsigned int task_count;            /* 1..RKNPU_MAX_TASKS */
    unsigned int reserved;
    rknpu_task tasks[RKNPU_MAX_TASKS];
} rknpu_submit_in;

/* SUBMIT, out. */
typedef struct rknpu_submit_out {
    unsigned long long seqno;           /* for WAIT */
    unsigned int core;
    unsigned int drv_us;                /* M7: time inside the driver's SUBMIT (0 from M6) */
} rknpu_submit_out;

/* WAIT, in. */
typedef struct rknpu_wait_in {
    unsigned long long seqno;
    unsigned int timeout_ms;            /* 0: RKNPU_WAIT_DEFAULT_MS */
    unsigned int flags;                 /* 0 or RKNPU_WAIT_POLL */
} rknpu_wait_in;

/* rknpu_wait_in.flags (M4) */
#define RKNPU_WAIT_POLL         0x00000001u /* return RKNPU_JOB_RUNNING at once if not finished */

#define RKNPU_WAIT_DEFAULT_MS   500     /* rocket's JOB_TIMEOUT_MS */
#define RKNPU_WAIT_MAX_MS       10000

/* rknpu_wait_out.result */
#define RKNPU_JOB_DONE          0       /* every task raised its DPU interrupt */
#define RKNPU_JOB_TIMEOUT       1       /* stopped; the core was reset (reset_status) */
#define RKNPU_JOB_DMA_ERROR     2       /* a DMA read or write error was raised; stopped */
#define RKNPU_JOB_UNKNOWN       3       /* no such job on this handle */
#define RKNPU_JOB_RUNNING       4       /* M4, RKNPU_WAIT_POLL only: not finished; nothing was stopped */
#define RKNPU_JOB_MMU_FAULT     5       /* M6: an IOMMU page fault or bus error; stopped, the core was reset */

/* WAIT, out. */
typedef struct rknpu_wait_out {
    unsigned long long seqno;
    unsigned int result;                /* RKNPU_JOB_* */
    unsigned int core;
    unsigned int raw_status;            /* PC INTERRUPT_RAW_STATUS at the last interrupt, or at the timeout */
    unsigned int task_status;           /* PC TASK_STATUS, same moment */
    unsigned int tasks_done;            /* tasks that raised their interrupt */
    unsigned int task_count;
    unsigned int isr_count;             /* interrupts claimed for this job */
    unsigned int hw_us;                 /* first OPERATION_ENABLE to the last interrupt */
    unsigned int wait_us;               /* time spent in WAIT */
    unsigned int reset_status;          /* NTSTATUS of the _DSM reset; 0 if none */
    unsigned int version_after;         /* PC VERSION read after the job (after the reset) */
    unsigned int mmu_status[RKNPU_MMUS_PER_CORE];
    unsigned int fault_addr;            /* M6, RKNPU_JOB_MMU_FAULT: the IOVA that faulted */
    unsigned int fault_status;          /* M6: the IOMMU INT_STATUS (1 page fault, 2 bus error)
                                         * in bits 0..7, the MMU STATUS in bits 8..31 */
    /* M7 (0 from an M6 driver) */
    unsigned int irq_dpc_us;            /* the job's last interrupt to its DPC setting Done */
    unsigned int dpc_wake_us;           /* Done set to this WAIT running again (see m7_flags) */
    unsigned int m7_flags;              /* RKNPU_WAIT_OUT_* */
} rknpu_wait_out;

/* RESET, in. */
typedef struct rknpu_reset_in {
    unsigned int core;
    unsigned int reserved;
} rknpu_reset_in;

/* RESET, out. */
typedef struct rknpu_reset_out {
    unsigned int dsm_status;            /* NTSTATUS of IOCTL_ACPI_EVAL_METHOD */
    unsigned int dsm_return;            /* the _DSM's return value: 0 done, 1 bad core */
    unsigned int version_after;         /* PC VERSION after the reset */
    unsigned int mmu_status[RKNPU_MMUS_PER_CORE];
    unsigned int reserved[3];
} rknpu_reset_out;

/*
 * Breadcrumbs. At each M3 stage the driver writes these DWORDs under the
 * device's "Device Parameters" key and flushes the key (ZwFlushKey), so they
 * survive a hang that takes the board down before the tool can print:
 *
 *   HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3576\0\Device Parameters
 *
 *   M3Stage     the last stage reached, RKNPU_CRUMB_*
 *   M3Seq       its job's sequence number (low 32 bits)
 *   M3Core      its core
 *   M3Value1    stage-specific: CREATE_BO size, SUBMIT stream address,
 *               WAIT raw status, RESET NTSTATUS
 *   M3Value2    stage-specific: CREATE_BO address, SUBMIT entry count,
 *               WAIT task status, RESET version after
 *   M3Count     the stage's running count (buffers, jobs, timeouts, ...)
 *
 * SUBMIT writes and flushes its crumb before OPERATION_ENABLE.
 */
#define RKNPU_CRUMB_BO_CREATE   1
#define RKNPU_CRUMB_BO_MAP      2
#define RKNPU_CRUMB_SUBMIT      3       /* flushed, then OPERATION_ENABLE */
#define RKNPU_CRUMB_IRQ_SEEN    4       /* WAIT found the job completed by the interrupt */
#define RKNPU_CRUMB_STATUS      5       /* WAIT returned; Value1/2 = raw / task status */
#define RKNPU_CRUMB_TIMEOUT     6       /* WAIT timed out; about to reset */
#define RKNPU_CRUMB_RESET       7       /* a _DSM reset returned */
#define RKNPU_CRUMB_BO_UNMAP    8
#define RKNPU_CRUMB_BO_FREE     9
#define RKNPU_CRUMB_REFUSED     10      /* M4: SUBMIT refused a stream; Value1 = entry index,
                                         * Value2 = target << 16 | register */
#define RKNPU_CRUMB_ARENA       11      /* M4: a core's copy arena was allocated; Value1 = size,
                                         * Value2 = its address */
#define RKNPU_CRUMB_IOMMU       12      /* M6: paging turned on for a core; Value1 = NTSTATUS,
                                         * Value2 = instance 0 STATUS << 16 | instance 1 STATUS */
#define RKNPU_CRUMB_MMU_FAULT   13      /* M6: WAIT found a job stopped by a fault; Value1 = IOVA,
                                         * Value2 = fault_status */

/* ---- M7: fewer IOCTLs a call, and the driver's own clock ------------------ */
/*
 * No new function codes and no new layout for M6 callers. A decode call in
 * charsiu was ten IOCTLs on M6 (two FINI_BO of the inputs, two SUBMIT, two
 * WAIT, two PREP_BO of the outputs, two FINI_BO of the outputs); M7 lets it
 * be four:
 *
 *   - SUBMIT with RKNPU_SUBMIT_CLEAN takes an rknpu_submit_m7_in (the M6
 *     struct followed by up to RKNPU_M7_MAX_CLEAN handles) and does FINI_BO
 *     on each of those buffers, whole, before it starts the job. A handle
 *     that is not the caller's fails the SUBMIT (STATUS_INVALID_HANDLE) and
 *     nothing starts. A buffer only reaches the NPU through a SUBMIT on its
 *     own handle (the address check), so cleaning at the next SUBMIT is
 *     never later than the NPU's first read of it.
 *   - WAIT with RKNPU_WAIT_PREP takes an rknpu_wait_m7_in and, once the job
 *     is no longer running, does PREP_BO on prep_handle, whole
 *     (RKNPU_WAIT_OUT_PREPPED says it did).
 *   - WAIT with RKNPU_WAIT_LEAN skips the diagnostic reads of PC VERSION and
 *     the IOMMU status after a job that finished cleanly (version_after and
 *     mmu_status are then 0).
 *   - SUBMIT says how long it spent inside the driver (drv_us), and WAIT
 *     the interrupt-to-DPC and DPC-to-waiter times (irq_dpc_us,
 *     dpc_wake_us), so a caller can split its own IOCTL time into the
 *     driver's and the operating system's.
 *
 * An M6 driver refuses the new flags with STATUS_INVALID_PARAMETER, so a
 * caller uses them only when rknpu_info.m7_version says so.
 */
#define RKNPU_M7_VERSION        1       /* rknpu_info.m7_version */
#define RKNPU_M7_MAX_CLEAN      32

#define RKNPU_SUBMIT_CLEAN      0x00000004u /* M7: rknpu_submit_m7_in, FINI_BO the listed buffers first */
#define RKNPU_WAIT_PREP         0x00000002u /* M7: rknpu_wait_m7_in, PREP_BO prep_handle after the job */
#define RKNPU_WAIT_LEAN         0x00000004u /* M7: no diagnostic register reads after a clean finish */

/* rknpu_wait_out.m7_flags */
#define RKNPU_WAIT_OUT_SLEPT    0x00000001u /* WAIT blocked on the job (dpc_wake_us is a wake-up);
                                             * otherwise it arrived after the DPC (a late caller) */
#define RKNPU_WAIT_OUT_PREPPED  0x00000002u /* RKNPU_WAIT_PREP was done */

typedef struct rknpu_submit_m7_in {
    rknpu_submit_in base;
    unsigned int clean_count;           /* 0..RKNPU_M7_MAX_CLEAN */
    unsigned int reserved;
    unsigned int clean[RKNPU_M7_MAX_CLEAN];
} rknpu_submit_m7_in;

typedef struct rknpu_wait_m7_in {
    rknpu_wait_in base;
    unsigned int prep_handle;           /* RKNPU_WAIT_PREP: a buffer of this handle */
    unsigned int reserved;
} rknpu_wait_m7_in;

/* ---- M8: where SUBMIT's time goes ---------------------------------------- */
/*
 * The M7 board runs measured a SUBMIT at about 110 us, about 100 of it inside
 * the driver's handler (drv_us). M8 splits that time into the phases below,
 * two ways:
 *
 *   - SUBMIT returns the split of that one call when the output buffer is an
 *     rknpu_submit_m8_out (the M7 output followed by phase_ns[]);
 *   - IOCTL_RKNPU_SUBMIT_STATS returns the per-core sums since the driver
 *     started, or since the last call that asked for a reset.
 *
 * And M8 makes two phases cheaper: the power and clock check reads only the
 * registers the answer depends on (not the PLL), and the streams are checked
 * in a cached copy that only the driver can see, which is then written to the
 * copy arena in one go; M7 checked them in the arena itself, which is
 * write-combined, so every entry was read from DRAM, twice (the rules, then
 * the CBUF key). What runs is still exactly what was checked.
 * RKNPU_SUBMIT_M7_PATH asks for M7's way of both, for a measurement.
 */
#define RKNPU_M8_VERSION        1       /* rknpu_info.m8_version */

#define RKNPU_SUBMIT_M7_PATH    0x00000008u /* M8: M7's clock check and in-arena stream check */

/* SUBMIT phases, in the order they run (phase_ns[] and the stats) */
#define RKNPU_SP_ENTRY          0   /* the handler's entry to holding JobLock: input checks, the lock wait */
#define RKNPU_SP_SAFE           1   /* the power and clock check (PMU and CRU reads) */
#define RKNPU_SP_IDLE           2   /* OPERATION_ENABLE, and in bypass mode the IOMMU status reads */
#define RKNPU_SP_ARENA          3   /* the copy arena (allocated once) and the wait for BoLock */
#define RKNPU_SP_MMU            4   /* the IOMMU check (turned on again after a reset) */
#define RKNPU_SP_CLEAN          5   /* M7's FINI_BOs riding on the SUBMIT (cache maintenance) */
#define RKNPU_SP_COPY           6   /* the streams copied out of the caller's buffers */
#define RKNPU_SP_CHECK          7   /* the stream rules and the CBUF key */
#define RKNPU_SP_CBUF           8   /* the wait for the other core's job on the same CBUF window */
#define RKNPU_SP_BOOK           9   /* job bookkeeping, breadcrumbs, the barrier */
#define RKNPU_SP_START          10  /* the interrupt lock and the start registers */
#define RKNPU_SP_TOTAL          11  /* the whole handler (= drv_us, in ns) */
#define RKNPU_SUBMIT_PHASES     12

typedef struct rknpu_submit_m8_out {
    rknpu_submit_out base;
    unsigned int phase_ns[RKNPU_SUBMIT_PHASES];
} rknpu_submit_m8_out;

/* SUBMIT_STATS, in (optional): flags. Out: rknpu_submit_stats. */
#define RKNPU_STATS_RESET       0x00000001u /* zero the sums after reading them */

typedef struct rknpu_submit_stats_in {
    unsigned int flags;
    unsigned int reserved;
} rknpu_submit_stats_in;

typedef struct rknpu_submit_core_stats {
    unsigned long long submits;         /* SUBMITs that started a job */
    unsigned long long phase_ns[RKNPU_SUBMIT_PHASES];
} rknpu_submit_core_stats;

typedef struct rknpu_submit_stats {
    unsigned int version;               /* RKNPU_M8_VERSION */
    unsigned int phases;                /* RKNPU_SUBMIT_PHASES */
    rknpu_submit_core_stats core[RKNPU_NUM_CORES];
} rknpu_submit_stats;

#define IOCTL_RKNPU_SUBMIT_STATS CTL_CODE(RKNPU_DEVICE_TYPE, RKNPU_FN_SUBMIT_STATS, METHOD_BUFFERED, FILE_READ_ACCESS)

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
RKNPU_ASSERT_SIZE(rknpu_create_bo, 16);
RKNPU_ASSERT_SIZE(rknpu_bo_handle, 8);
RKNPU_ASSERT_SIZE(rknpu_map_bo, 16);
RKNPU_ASSERT_SIZE(rknpu_bo_io, 16);
RKNPU_ASSERT_SIZE(rknpu_bo_sync, 16);
RKNPU_ASSERT_SIZE(rknpu_task, 8);
RKNPU_ASSERT_SIZE(rknpu_submit_in, 528);
RKNPU_ASSERT_SIZE(rknpu_submit_out, 16);
RKNPU_ASSERT_SIZE(rknpu_wait_in, 16);
RKNPU_ASSERT_SIZE(rknpu_wait_out, 80);
RKNPU_ASSERT_SIZE(rknpu_reset_in, 8);
RKNPU_ASSERT_SIZE(rknpu_reset_out, 32);
RKNPU_ASSERT_SIZE(rknpu_submit_m7_in, 664);
RKNPU_ASSERT_SIZE(rknpu_wait_m7_in, 24);
RKNPU_ASSERT_SIZE(rknpu_submit_m8_out, 64);
RKNPU_ASSERT_SIZE(rknpu_submit_stats_in, 8);
RKNPU_ASSERT_SIZE(rknpu_submit_stats, 216);

#endif /* RKNPU_IOCTL_H */
