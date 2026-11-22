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
#define RK_MMU_PAGE_FAULT_ADDR  0x0Cu
#define RK_MMU_INT_RAWSTAT      0x14u
#define RK_MMU_INT_CLEAR        0x18u
#define RK_MMU_INT_MASK         0x1Cu
#define RK_MMU_INT_STATUS       0x20u

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

/* One buffer (M3): contiguous, below 4 GiB, write-combined. */
typedef struct _NPU_BO {
    LIST_ENTRY      Link;
    ULONG           Handle;
    PVOID           Kva;
    PHYSICAL_ADDRESS Pa;
    SIZE_T          Size;
    PMDL            Mdl;            /* built on the first MAP_BO */
    PVOID           UserVa;         /* the caller's mapping, or NULL */
    PEPROCESS       Process;        /* referenced while UserVa is set */
    WDFFILEOBJECT   Owner;
} NPU_BO, *PNPU_BO;

/*
 * The job on a core (M3). Written at PASSIVE_LEVEL under the interrupt lock
 * before the first task starts; afterwards only the ISR moves it, until the
 * DPC or a recovery takes it back. No wait ever happens under that lock.
 */
typedef struct _CORE_JOB {
    BOOLEAN         InFlight;       /* the hardware owns the job */
    BOOLEAN         CompletePending;/* the ISR finished it; the DPC has not run */
    BOOLEAN         NoCrumbs;       /* RKNPU_SUBMIT_NO_CRUMBS */
    ULONG           Result;         /* RKNPU_JOB_* once finished */
    ULONGLONG       Seq;            /* 0: never used */
    WDFFILEOBJECT   Owner;
    ULONG           TaskCount;
    ULONG           NextTask;
    ULONG           TasksDone;
    ULONG           IsrCount;
    ULONG           RawStatus;
    ULONG           TaskStatus;
    LARGE_INTEGER   T0;             /* QPC at the first OPERATION_ENABLE */
    LARGE_INTEGER   T1;             /* QPC at the last interrupt */
    rknpu_task      Tasks[RKNPU_MAX_TASKS];
} CORE_JOB;

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
} CORE_STATE;

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

static __forceinline ULONG NpuCoreBase(ULONG core)
{
    return core * NPU_CORE_STRIDE;
}

/* driver.c */
BOOLEAN NpuSafeToRead(_In_ PDEVICE_CONTEXT Ctx);

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

#endif
