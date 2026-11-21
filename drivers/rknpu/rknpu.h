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

#include <ntddk.h>
#include <wdf.h>

#include "rknpu_ioctl.h"

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

typedef struct _CORE_STATE {
    WDFINTERRUPT    Interrupt;
    ULONG           Gsiv;
    volatile LONG   IsrCount;
    volatile LONG   IsrUnclaimed;
    volatile LONG   DpcCount;
    ULONG           LastPcStatus;
    ULONG           LastMmuIntStatus[RKNPU_MMUS_PER_CORE];
    ULONG           LastMmuFault[RKNPU_MMUS_PER_CORE];
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

#endif
