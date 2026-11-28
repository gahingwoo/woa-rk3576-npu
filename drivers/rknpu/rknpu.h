/*
 * rknpu.h - private definitions of rknpu.sys
 *
 * Register offsets: drivers/accel/rocket/rocket_registers.h (PC block) and
 * drivers/iommu/rockchip-iommu.c in Linux; the block layout and the PMU and
 * CRU addresses: the SSDT (edk2-rk3576 Silicon/Rockchip/RK3576/AcpiTables/
 * Npu.asl) and Npu.c beside it, which read the same registers.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RKNPU_DRV_H
#define RKNPU_DRV_H

#include <ntifs.h>     /* includes ntddk.h */
#include <wdf.h>
#include <intrin.h>

#include "rknpu_ioctl.h"
#include "rknpu_regcmd.h"

#define RKNPU_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "rknpu: " fmt "\n", __VA_ARGS__)

/* The window: core 0 at +0x0000, core 1 at +0x8000. */
#define NPU_CORE_STRIDE         0x8000u
#define NPU_PC_OFF              0x0000u
#define NPU_CNA_OFF             0x1000u
#define NPU_MMU0_OFF            0x2000u
#define NPU_MMU1_OFF            0x2100u
#define NPU_CORE_OFF            0x3000u

/* PC (rocket_registers.h) */
#define PC_VERSION              0x0000u
#define PC_VERSION_NUM          0x0004u
#define PC_OPERATION_ENABLE     0x0008u
#define PC_INTERRUPT_MASK       0x0020u
#define PC_INTERRUPT_CLEAR      0x0024u
#define PC_INTERRUPT_STATUS     0x0028u
#define PC_INTERRUPT_RAW_STATUS 0x002Cu
#define PC_TASK_CON             0x0030u
#define PC_TASK_STATUS          0x003Cu
#define PC_INTERRUPT_ALL        0x1FFFFu    /* rocket_job_handle_irq() */

/* M3: the job registers rocket_job_hw_submit() writes (rocket_registers.h) */
#define PC_BASE_ADDRESS         0x0010u
#define PC_REGISTER_AMOUNTS     0x0014u
#define PC_TASK_DMA_BASE_ADDR   0x0034u
#define PC_INT_DPU_0            0x00000100u /* PC_INTERRUPT_*_DPU_0 */
#define PC_INT_DPU_1            0x00000200u
#define PC_INT_DMA_READ_ERROR   0x00001000u
#define PC_INT_DMA_WRITE_ERROR  0x00002000u
#define PC_DATA_AMOUNT_MASK     0x0000FFFFu /* PC_REGISTER_AMOUNTS_PC_DATA_AMOUNT */
#define CNA_S_POINTER           (NPU_CNA_OFF + 0x0004u)     /* REG_CNA_S_POINTER */
#define CORE_S_POINTER          (NPU_CORE_OFF + 0x0004u)    /* REG_CORE_S_POINTER */
#define S_POINTER_PP_EN         (1u << 1)   /* *_S_POINTER_POINTER_PP_EN(1) */
#define S_POINTER_EXECUTER_PP_EN (1u << 2)  /* *_S_POINTER_EXECUTER_PP_EN(1) */
#define S_POINTER_PP_MODE       (1u << 3)   /* *_S_POINTER_POINTER_PP_MODE(1) */
#define S_POINTER_CORE_BIT      0x10000000u /* "extra_bit", times the core index */
/* v17-0009, rocket_job.c: the RK3576 PC_TASK_CON layout */
#define RK3576_TASK_NUMBER(n)   ((n) & 0xFFFFu)
#define RK3576_TASK_PP_EN       (1u << 16)
#define RK3576_TASK_COUNT_CLEAR (1u << 17)
#define RK3576_TASK_LAST_LAYER_CLEAR (1u << 18)
#define RK_MMU_STATUS_PAGING    0x00000001u

/* IOMMU (rockchip-iommu.c) */
#define RK_MMU_DTE_ADDR         0x00u
#define RK_MMU_STATUS           0x04u
#define RK_MMU_COMMAND          0x08u   /* M6 */
#define RK_MMU_PAGE_FAULT_ADDR  0x0Cu
#define RK_MMU_ZAP_ONE_LINE     0x10u   /* M6 */
#define RK_MMU_INT_RAWSTAT      0x14u
#define RK_MMU_INT_CLEAR        0x18u
#define RK_MMU_INT_MASK         0x1Cu
#define RK_MMU_INT_STATUS       0x20u
#define RK_MMU_AUTO_GATING      0x24u   /* M6 */

/* M6: RK_MMU_STATUS bits, RK_MMU_COMMAND values, interrupt bits */
#define RK_MMU_ST_PAGE_FAULT_ACTIVE 0x00000002u
#define RK_MMU_ST_STALL_ACTIVE  0x00000004u
#define RK_MMU_CMD_ENABLE_PAGING    0u
#define RK_MMU_CMD_DISABLE_PAGING   1u
#define RK_MMU_CMD_ENABLE_STALL     2u
#define RK_MMU_CMD_DISABLE_STALL    3u
#define RK_MMU_CMD_ZAP_CACHE        4u
#define RK_MMU_CMD_PAGE_FAULT_DONE  5u
#define RK_MMU_CMD_FORCE_RESET      6u
#define RK_MMU_IRQ_PAGE_FAULT   0x01u
#define RK_MMU_IRQ_BUS_ERROR    0x02u
#define RK_MMU_IRQ_MASK         (RK_MMU_IRQ_PAGE_FAULT | RK_MMU_IRQ_BUS_ERROR)
#define RK_MMU_DISABLE_FETCH_DTE_TIME_LIMIT 0x80000000u   /* AUTO_GATING bit 31 */

/* M6: the NPU address space, in 64 KiB chunks (iommu.c) */
#define NPU_IOVA_CHUNK_SHIFT    16u
#define NPU_IOVA_CHUNKS         65536u      /* 4 GiB */
#define NPU_IOVA_FIRST_CHUNK    16u         /* nothing below 1 MiB: no buffer at IOVA 0 */
#define NPU_PT_ENTRIES          1024u

/* PMU and CRU, one page each, read only (see rknpu_clock_info). */
#define PMU_PAGE_PA             0x27380000ull
#define PMU_STATUS0             0x230u
#define PMU_REPAIR0             0x570u
#define CRU_PAGE_PA             0x27200000ull
#define CRU_GPLL_CON0           0x1C0u
#define CRU_MODE_CON00          0x280u
#define CRU_GPLL_MODE_SHIFT     2
#define CRU_CLKSEL86            (0x300u + 86u * 4u)
#define CRU_GATE(n)             (0x800u + (n) * 4u)
#define OSC_HZ                  24000000u

#define RKNPU_POOL_TAG          'UPNR'

/*
 * One buffer. M3/M4 (bypass): contiguous, below 4 GiB, Dma = physical. M6
 * (IOMMU): pages from MmAllocatePagesForMdlEx, anywhere, Dma = the IOVA the
 * driver mapped them at. Cached or write-combined (Cached), the same in
 * every mapping of it.
 */
typedef struct _NPU_BO {
    LIST_ENTRY      Link;
    LIST_ENTRY      HashLink;       /* M6: Ctx->BoHash[Handle % RKNPU_BO_HASH] */
    ULONG           Handle;
    ULONG           Dma;            /* the NPU's address of byte 0 */
    PVOID           Kva;
    PHYSICAL_ADDRESS Pa;            /* bypass mode only */
    SIZE_T          Size;
    PMDL            Mdl;            /* bypass: built on the first MAP_BO; IOMMU: the pages */
    BOOLEAN         PagesMdl;       /* M6: Mdl came from MmAllocatePagesForMdlEx */
    BOOLEAN         Cached;         /* M6: RKNPU_BO_CACHED */
    BOOLEAN         IovaMapped;     /* M6: in the page table (iommu.c) */
    PVOID           UserVa;         /* the caller's mapping, or NULL */
    PEPROCESS       Process;        /* referenced while UserVa is set */
    WDFFILEOBJECT   Owner;
} NPU_BO, *PNPU_BO;

#define RKNPU_BO_HASH           256u

/*
 * M6: the driver's NPU address space (iommu.c). One page directory, its page
 * tables allocated as needed, all write-combined so the IOMMU (not coherent)
 * sees every write after a barrier; every one of the four IOMMU instances
 * points at it. BoLock protects all of it.
 */
typedef struct _NPU_MMU {
    BOOLEAN         On;             /* IOMMU mode (Device Parameters\M6Iommu, default 1) */
    PULONG          Dt;             /* 1024 directory entries */
    PHYSICAL_ADDRESS DtPa;
    PULONG          Pt[NPU_PT_ENTRIES];
    PHYSICAL_ADDRESS PtPa[NPU_PT_ENTRIES];
    ULONG           PtPages;        /* the directory included */
    RTL_BITMAP      Chunks;         /* NPU_IOVA_CHUNKS bits, one per 64 KiB */
    PULONG          ChunkBits;
    PNPU_BO        *ChunkOwner;     /* NPU_IOVA_CHUNKS entries: the buffer, or NULL */
    ULONG           ChunksUsed;
    ULONG           Hint;
} NPU_MMU;

/*
 * The job on a core (M3). Written at PASSIVE_LEVEL under the interrupt lock
 * before the first task starts; afterwards only the ISR moves it, until the
 * DPC or a recovery takes it back. No wait ever happens under that lock.
 */
typedef struct _CORE_JOB {
    BOOLEAN         InFlight;       /* the hardware owns the job */
    BOOLEAN         CompletePending;/* the ISR finished it; the DPC has not run */
    BOOLEAN         NoCrumbs;       /* RKNPU_SUBMIT_NO_CRUMBS */
    BOOLEAN         Copied;         /* M4: RKNPU_SUBMIT_COPY, the streams are in the arena */
    BOOLEAN         FaultReset;     /* M6: the core was reset after this job's IOMMU fault */
    ULONG           CbufKey;        /* M6: rknpu_rc_cbuf_key() of its streams (the shared CBUF) */
    ULONG           Result;         /* RKNPU_JOB_* once finished */
    ULONGLONG       Seq;            /* 0: never used */
    WDFFILEOBJECT   Owner;
    ULONG           TaskCount;
    ULONG           NextTask;
    ULONG           TasksDone;
    ULONG           IsrCount;
    ULONG           RawStatus;
    ULONG           TaskStatus;
    ULONG           FaultAddr;      /* M6: RKNPU_JOB_MMU_FAULT */
    ULONG           FaultStatus;
    LARGE_INTEGER   T0;             /* QPC at the first OPERATION_ENABLE */
    LARGE_INTEGER   T1;             /* QPC at the last interrupt */
    LARGE_INTEGER   T2;             /* M7: QPC when the DPC set Done */
    rknpu_task      Tasks[RKNPU_MAX_TASKS];
} CORE_JOB;

/*
 * The performance counters' running totals for one core (perf.c). Written
 * only under the core's interrupt lock (ISR, DPC, SUBMIT's start of the job,
 * the stop paths), so the writers never race each other; Seq is odd while a
 * writer is inside, and the counter callback reads a consistent copy
 * without taking the lock. Times are QPC ticks, converted only when read.
 */
typedef struct _CORE_PERF {
    volatile LONG   Seq;
    LONGLONG        RunStart;       /* QPC of the running job's first OPERATION_ENABLE; 0: idle */
    LONGLONG        SubmitQpc;      /* QPC when that job's SUBMIT came in */
    ULONGLONG       BusyTicks;      /* sum of T1 - T0 over the jobs that ended */
    ULONGLONG       JobsEnded;      /* done, DMA error, IOMMU fault, timeout, stopped */
    ULONGLONG       TasksDone;
    ULONGLONG       JobsDone;       /* finished through the DPC (what a WAIT sees as completed) */
    ULONGLONG       LatencyTicks;   /* sum of SUBMIT in to DPC, over JobsDone */
    volatile LONG64 LastBusy100ns;  /* reader only: keeps % busy monotonic */
} CORE_PERF;

typedef struct _CORE_STATE {
    WDFINTERRUPT    Interrupt;
    ULONG           Gsiv;
    volatile LONG   IsrCount;
    volatile LONG   IsrUnclaimed;
    volatile LONG   DpcCount;
    ULONG           LastPcStatus;
    ULONG           LastMmuIntStatus[RKNPU_MMUS_PER_CORE];
    ULONG           LastMmuFault[RKNPU_MMUS_PER_CORE];

    /* M3 */
    CORE_JOB        Job;
    KEVENT          Done;           /* set by the DPC when Job finishes */
    ULONG           Version;        /* PC VERSION at D0 entry, to check a reset against */
    volatile LONG   JobsSubmitted;
    volatile LONG   JobsCompleted;
    volatile LONG   JobsTimedOut;
    volatile LONG   Resets;

    /* M4: the copy arena, allocated by the first RKNPU_SUBMIT_COPY on the core */
    PUCHAR          Arena;
    PHYSICAL_ADDRESS ArenaPa;
    ULONG           ArenaDma;       /* M6: its NPU address (= ArenaPa in bypass mode) */
    PMDL            ArenaMdl;       /* M6, IOMMU mode: its pages */
    PUCHAR          ArenaShadow;    /* M8: a cached, driver-only copy where the streams are checked */

    /* M8: where SUBMIT's time goes (QPC ticks, JobLock) */
    ULONGLONG       SubmitCount;
    ULONGLONG       SubmitTicks[RKNPU_SUBMIT_PHASES];

    /* M6 */
    volatile LONG   MmuFaults;
    volatile LONG   MmuEnables;
    volatile LONG   CbufWaits;      /* SUBMITs that waited for the other core's same-window job */

    /* performance counters (perf.c) */
    CORE_PERF       Perf;
} CORE_STATE;

/* M4: a finished job, kept so a WAIT that comes after another handle's job
 * took the core still gets its answer. */
typedef struct _JOB_RECORD {
    ULONGLONG       Seq;            /* 0: empty */
    WDFFILEOBJECT   Owner;
    ULONG           Core;
    ULONG           Result;
    ULONG           RawStatus;
    ULONG           TaskStatus;
    ULONG           TasksDone;
    ULONG           TaskCount;
    ULONG           IsrCount;
    ULONG           HwUs;
    ULONG           FaultAddr;      /* M6 */
    ULONG           FaultStatus;
} JOB_RECORD;

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE       Device;
    PUCHAR          Regs;
    SIZE_T          RegsLen;
    PHYSICAL_ADDRESS RegsPa;
    PUCHAR          Pmu;            /* NULL if it could not be mapped */
    PUCHAR          Cru;
    BOOLEAN         InterruptsConnected;
    CORE_STATE      Core[RKNPU_NUM_CORES];
    WDFQUEUE        Queue;

    /* M3. Lock order: JobLock, then BoLock, then an interrupt lock. */
    WDFWAITLOCK     JobLock;        /* SUBMIT, WAIT, RESET, cleanup */
    WDFWAITLOCK     BoLock;         /* the buffer list */
    LIST_ENTRY      Bos;
    ULONG           NextHandle;
    ULONG           BoCount;
    SIZE_T          BoBytes;
    ULONGLONG       NextSeq;
    LONG            CrumbCounts[16];
    WDFKEY          CrumbKey;       /* Device Parameters; NULL if it would not open */
    LARGE_INTEGER   QpcFreq;

    /* M4 */
    JOB_RECORD      History[RKNPU_JOB_HISTORY];     /* JobLock */
    ULONG           HistoryAt;
    BOOLEAN         AllowUnlisted;  /* Device Parameters\M4AllowUnlisted, read at start */
    BOOLEAN         UserAccess;     /* Device Parameters\UserAccess = 1 and the IOMMU asked for:
                                     * interactive users may open \\.\RKNPU (driver.c) */
    volatile LONG   UnlistedLogged;

    /* M6 */
    NPU_MMU         Mmu;
    BOOLEAN         CachedBos;      /* Device Parameters\M6CachedBos */
    SIZE_T          BoMaxTotal;     /* RKNPU_BO_MAX_TOTAL, or _IOMMU */
    LIST_ENTRY      BoHash[RKNPU_BO_HASH];

    /* M7 */
    KPRIORITY       WakeBoost;      /* Device Parameters\M7WakeBoost: KeSetEvent's increment */
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetContext)

typedef struct _INTERRUPT_CONTEXT {
    ULONG           CoreIndex;
} INTERRUPT_CONTEXT, *PINTERRUPT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(INTERRUPT_CONTEXT, GetInterruptContext)

static __forceinline ULONG NpuRd(PDEVICE_CONTEXT c, ULONG off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(c->Regs + off));
}

static __forceinline VOID NpuWr(PDEVICE_CONTEXT c, ULONG off, ULONG v)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(c->Regs + off), v);
}

/*
 * Memory the NPU reads or writes and the CPU maps uncached: the copy arena,
 * the IOMMU tables, and buffers made with M6CachedBos = 0 or in bypass mode.
 * Windows 10 (19041) raises STATUS_DATATYPE_MISALIGNMENT for a misaligned
 * access to non-coherent DMA memory where Windows 11 lets it through, and
 * RtlCopyMemory and RtlZeroMemory are free to make one: memcpy covers a
 * head and a tail with overlapping, unaligned LDP/STP, and memset clears
 * with DC ZVA, which faults on Device memory at any alignment. So the
 * driver touches such memory only through these two. The device side gets
 * naturally aligned 8-byte accesses and single bytes at the ends; volatile
 * keeps the compiler from merging them into pairs or vectors, or from
 * turning the loop back into a memset or memcpy call. The other side may be
 * misaligned only when it is ordinary cached memory (pool, an IOCTL buffer),
 * where any access is fine. A cached buffer still uses RtlCopyMemory.
 */
static __forceinline VOID
NpuDevZero(
    _Out_writes_bytes_all_(Len) PVOID Dst,
    _In_ SIZE_T Len
    )
{
    volatile UCHAR *d = (volatile UCHAR *)Dst;

    for (; Len && ((ULONG_PTR)d & 7); Len--)
        *d++ = 0;
    for (; Len >= 8; Len -= 8, d += 8)
        *(volatile ULONG64 *)d = 0;
    for (; Len; Len--)
        *d++ = 0;
}

/* DevIsSrc: which side is device memory, so which one the loop aligns. */
static __forceinline VOID
NpuDevCopy(
    _Out_writes_bytes_all_(Len) PVOID Dst,
    _In_reads_bytes_(Len) const VOID *Src,
    _In_ SIZE_T Len,
    _In_ BOOLEAN DevIsSrc
    )
{
    volatile UCHAR *d = (volatile UCHAR *)Dst;
    const volatile UCHAR *s = (const volatile UCHAR *)Src;

    for (; Len && ((ULONG_PTR)(DevIsSrc ? (const volatile void *)s : (const volatile void *)d) & 7); Len--)
        *d++ = *s++;
    for (; Len >= 8; Len -= 8, d += 8, s += 8)
        *(volatile ULONG64 UNALIGNED *)d = *(const volatile ULONG64 UNALIGNED *)s;
    for (; Len; Len--)
        *d++ = *s++;
}

static __forceinline ULONG NpuCoreBase(ULONG core)
{
    return core * NPU_CORE_STRIDE;
}

/* driver.c */
BOOLEAN NpuSafeToRead(_In_ PDEVICE_CONTEXT Ctx);
BOOLEAN NpuSafeToReadFast(_In_ PDEVICE_CONTEXT Ctx);

/* m3.c */
NTSTATUS NpuM3DeviceInit(_In_ PDEVICE_CONTEXT Ctx);
VOID NpuM3D0Entry(_In_ PDEVICE_CONTEXT Ctx);
VOID NpuM3D0Exit(_In_ PDEVICE_CONTEXT Ctx);
BOOLEAN NpuM3Isr(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG N);
VOID NpuM3Dpc(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG N);
BOOLEAN NpuM3InCallerContext(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _In_ ULONG IoControlCode);
BOOLEAN NpuM3DeviceControl(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _In_ ULONG IoControlCode,
                           _Out_ NTSTATUS *Status, _Out_ ULONG_PTR *Info);
VOID NpuM3FileCleanup(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFFILEOBJECT FileObject);
VOID NpuM3FreeAll(_In_ PDEVICE_CONTEXT Ctx);
VOID NpuM3JobFaultLocked(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG N, _In_ ULONG Iova, _In_ ULONG Status);

/* perf.c: the "RK3576 NPU" and "RK3576 NPU Memory" counter sets. Register
 * failing is logged and otherwise ignored. The *Locked calls hold the
 * core's interrupt lock. */
VOID NpuPerfRegister(_In_ PDEVICE_CONTEXT Ctx);
VOID NpuPerfUnregister(VOID);
VOID NpuPerfJobStartLocked(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG N, _In_ LONGLONG SubmitQpc);
VOID NpuPerfJobEndLocked(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG N);
VOID NpuPerfJobDoneLocked(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG N);

/* iommu.c (M6). BoLock held for everything but Init, Free and the ISR part. */
NTSTATUS NpuMmuInit(_In_ PDEVICE_CONTEXT Ctx);
VOID NpuMmuFree(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS NpuMmuMapLocked(_In_ PDEVICE_CONTEXT Ctx, _In_ PMDL Mdl, _In_ SIZE_T Size, _In_ BOOLEAN Writable,
                         _In_opt_ PNPU_BO Bo, _Out_ ULONG *Iova);
VOID NpuMmuUnmapLocked(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Iova, _In_ SIZE_T Size);
PNPU_BO NpuMmuOwnerLocked(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Iova);
NTSTATUS NpuMmuEnsureLocked(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG N);
VOID NpuMmuDisableLocked(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG N);
BOOLEAN NpuMmuIsr(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG N);

/* cache.asm (M6): data cache maintenance by VA to the point of coherency, then DSB SY */
VOID RknpuDcClean(_In_ PVOID Va, _In_ SIZE_T Length);
VOID RknpuDcCleanInvalidate(_In_ PVOID Va, _In_ SIZE_T Length);

#endif
