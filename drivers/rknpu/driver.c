/*
 * driver.c - RK3576 NPU (RKNN, two cores), M2 driver skeleton
 *
 * A KMDF function driver for ACPI\RKCP3576, which edk2-rk3576 publishes in
 * an SSDT (Npu.asl, OEM table ID RKNPU) only when "NPU (experimental)" is
 * enabled in setup. The firmware sets the rail, powers PD_NPU, PD_NPUTOP,
 * PD_NPU0 and PD_NPU1, sets clk_rknn_dsu0 and opens the gates (Npu.c); the
 * SSDT's _STA reports the device only while all of that holds. This driver
 * touches none of it and has no power management of its own: D0 entry and
 * exit do not change power, clocks or resets.
 *
 * What M2 does:
 *   - maps the 64 KiB register window (_CRS memory 0: both cores, each with
 *     pc, cna, core and two IOMMU instances);
 *   - connects both interrupts (_CRS interrupt 0 = GSIV 279, core 0 and its
 *     IOMMU; interrupt 1 = GSIV 280, core 1 and its IOMMU). The ISR acks
 *     what it finds, the DPC counts. No job is ever started in M2, so the
 *     counts are expected to stay 0;
 *   - creates \\.\RKNPU, administrators and SYSTEM only, one handle at a
 *     time, with GET_INFO and READ_REGS. Both only read.
 *
 * The only register writes this driver makes, all to the PC block of a
 * core: INTERRUPT_MASK = 0 and INTERRUPT_CLEAR = 0x1ffff at D0 entry (stale
 * state from firmware or a previous OS), and in the ISR, when the PC block
 * raised the interrupt, INTERRUPT_MASK = 0, OPERATION_ENABLE = 0 and
 * INTERRUPT_CLEAR = 0x1ffff, which is what rocket_job_irq_handler() and
 * rocket_job_handle_irq() write. An IOMMU interrupt is acked with its own
 * INT_CLEAR. Nothing writes GLOBAL_OPERATION_ENABLE (0xf008): from core 0's
 * window it hits NPU1 in single-core mode.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rknpu.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD NpuEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE NpuEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE NpuEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY NpuEvtD0Entry;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED NpuEvtD0EntryPostInterrupts;
EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED NpuEvtD0ExitPreInterrupts;
EVT_WDF_INTERRUPT_ISR NpuEvtIsr;
EVT_WDF_INTERRUPT_DPC NpuEvtDpc;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL NpuEvtIoDeviceControl;

#define REGS_MIN_LEN    RKNPU_REGS_WINDOW

static const ULONG MmuOff[RKNPU_MMUS_PER_CORE] = { NPU_MMU0_OFF, NPU_MMU1_OFF };

/* ------------------------------------------------- PMU and CRU (read only) */

static ULONG
PmuRd(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Off
    )
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Ctx->Pmu + Off));
}

static ULONG
CruRd(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Off
    )
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Ctx->Cru + Off));
}

/*
 * The SSDT's _STA test: PD_NPU on (STATUS0 bit 0 clear), NPUTOP, NPU0 and
 * NPU1 on (REPAIR0 bits 22..24), the NPU clocks ungated.
 */
static BOOLEAN
NpuDomainsOn(
    _In_ const rknpu_clock_info *C
    )
{
    return (C->pmu_status0 & 0x1) == 0 &&
           (C->pmu_repair0 & 0x01C00000) == 0x01C00000 &&
           (C->cru_gate28 & 0x200) == 0 && (C->cru_gate29 & 0x1) == 0 &&
           (C->cru_gate31 & 0x130) == 0 && (C->cru_gate32 & 0x1021) == 0;
}

/* GPLL as edk2-rk3576 Npu.c NpuGpllRate() computes it. */
static ULONG
NpuGpllHz(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    ULONG con0, con1, m, p, s;
    SHORT k;
    LONGLONG rate;

    if (((CruRd(Ctx, CRU_MODE_CON00) >> CRU_GPLL_MODE_SHIFT) & 0x3) != 1)
        return OSC_HZ;
    con0 = CruRd(Ctx, CRU_GPLL_CON0);
    con1 = CruRd(Ctx, CRU_GPLL_CON0 + 4);
    k = (SHORT)(CruRd(Ctx, CRU_GPLL_CON0 + 8) & 0xFFFF);
    if (con0 & 0x8000)
        return OSC_HZ;
    m = con0 & 0x3FF;
    p = con1 & 0x3F;
    s = (con1 >> 6) & 0x7;
    if (m == 0 || p == 0)
        return OSC_HZ;
    rate = (LONGLONG)OSC_HZ * m;
    if (k != 0)
        rate += ((LONGLONG)OSC_HZ * k) / 65536;
    return (ULONG)(rate / ((LONGLONG)p << s));
}

/* TRUE if the clocks block was filled. */
static BOOLEAN
NpuReadClocks(
    _In_ PDEVICE_CONTEXT Ctx,
    _Out_ rknpu_clock_info *C
    )
{
    RtlZeroMemory(C, sizeof(*C));
    if (!Ctx->Pmu || !Ctx->Cru)
        return FALSE;

    C->pmu_status0  = PmuRd(Ctx, PMU_STATUS0);
    C->pmu_repair0  = PmuRd(Ctx, PMU_REPAIR0);
    C->cru_clksel86 = CruRd(Ctx, CRU_CLKSEL86);
    C->cru_gate28   = CruRd(Ctx, CRU_GATE(28));
    C->cru_gate29   = CruRd(Ctx, CRU_GATE(29));
    C->cru_gate31   = CruRd(Ctx, CRU_GATE(31));
    C->cru_gate32   = CruRd(Ctx, CRU_GATE(32));
    C->gpll_hz      = NpuGpllHz(Ctx);
    /* clk_rknn_dsu0: mux [8:7] (0 = gpll), div [6:2] */
    if (((C->cru_clksel86 >> 7) & 0x3) == 0)
        C->dsu0_hz = C->gpll_hz / (((C->cru_clksel86 >> 2) & 0x1F) + 1);
    return TRUE;
}

/*
 * FALSE only when the PMU and CRU were read and say the NPU is not powered
 * or not clocked: then no NPU register may be read. Without the PMU and CRU
 * the driver relies on _STA, which reported the device only when it was.
 */
static BOOLEAN
NpuSafeToRead(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    rknpu_clock_info c;

    if (!NpuReadClocks(Ctx, &c))
        return TRUE;
    return NpuDomainsOn(&c);
}

/* -------------------------------------------------------- interrupts */

BOOLEAN
NpuEvtIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
    )
{
    PDEVICE_CONTEXT ctx = GetContext(WdfInterruptGetDevice(Interrupt));
    ULONG n = GetInterruptContext(Interrupt)->CoreIndex;
    CORE_STATE *core = &ctx->Core[n];
    ULONG base = NpuCoreBase(n);
    BOOLEAN ours = FALSE;
    ULONG st, i;

    UNREFERENCED_PARAMETER(MessageID);

    /* The PC block. M2 starts no job and leaves the mask at 0, so this is
     * not expected to fire; if it does, do what rocket does on completion. */
    st = NpuRd(ctx, base + PC_INTERRUPT_STATUS);
    if (st) {
        NpuWr(ctx, base + PC_INTERRUPT_MASK, 0);
        NpuWr(ctx, base + PC_OPERATION_ENABLE, 0);
        NpuWr(ctx, base + PC_INTERRUPT_CLEAR, PC_INTERRUPT_ALL);
        core->LastPcStatus = st;
        ours = TRUE;
    }

    /* The core's two IOMMU instances share the line. */
    for (i = 0; i < RKNPU_MMUS_PER_CORE; i++) {
        ULONG ms = NpuRd(ctx, base + MmuOff[i] + RK_MMU_INT_STATUS);
        if (!ms)
            continue;
        core->LastMmuIntStatus[i] = ms;
        core->LastMmuFault[i] = NpuRd(ctx, base + MmuOff[i] + RK_MMU_PAGE_FAULT_ADDR);
        NpuWr(ctx, base + MmuOff[i] + RK_MMU_INT_CLEAR, ms);
        ours = TRUE;
    }

    if (!ours) {
        InterlockedIncrement(&core->IsrUnclaimed);
        return FALSE;
    }
    InterlockedIncrement(&core->IsrCount);
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

VOID
NpuEvtDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject
    )
{
    PDEVICE_CONTEXT ctx = GetContext(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(AssociatedObject);

    /* M3 completes jobs here. */
    InterlockedIncrement(&ctx->Core[GetInterruptContext(Interrupt)->CoreIndex].DpcCount);
}

/* ---------------------------------------------------------- hardware */

NTSTATUS
NpuEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PDEVICE_CONTEXT ctx = GetContext(Device);
    PHYSICAL_ADDRESS pa;
    ULONG i, nmem = 0, nint = 0;

    for (i = 0; i < WdfCmResourceListGetCount(ResourcesTranslated); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (d->Type == CmResourceTypeMemory) {
            if (nmem == 0) {
                ctx->RegsPa = d->u.Memory.Start;
                ctx->RegsLen = d->u.Memory.Length;
            }
            nmem++;
        }
    }
    /* The raw list keeps the GSIV in the vector field; for GET_INFO only. */
    for (i = 0; i < WdfCmResourceListGetCount(ResourcesRaw); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(ResourcesRaw, i);

        if (d->Type == CmResourceTypeInterrupt) {
            if (nint < RKNPU_NUM_CORES)
                ctx->Core[nint].Gsiv = d->u.Interrupt.Vector;
            nint++;
        }
    }
    if (nmem == 0 || ctx->RegsLen < REGS_MIN_LEN || nint < RKNPU_NUM_CORES) {
        RKNPU_LOG("resources: %u memory (first 0x%llx+0x%llx), %u interrupts; need 64 KiB and 2",
                  nmem, ctx->RegsPa.QuadPart, (ULONGLONG)ctx->RegsLen, nint);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ctx->Regs = (PUCHAR)MmMapIoSpaceEx(ctx->RegsPa, REGS_MIN_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    if (!ctx->Regs)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Not in _CRS, and not fatal: GET_INFO then reports no clocks. */
    pa.QuadPart = PMU_PAGE_PA;
    ctx->Pmu = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    pa.QuadPart = CRU_PAGE_PA;
    ctx->Cru = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);

    RKNPU_LOG("registers at 0x%llx+0x%llx, GSIV %u/%u, PMU %s, CRU %s",
              ctx->RegsPa.QuadPart, (ULONGLONG)ctx->RegsLen, ctx->Core[0].Gsiv, ctx->Core[1].Gsiv,
              ctx->Pmu ? "mapped" : "not mapped", ctx->Cru ? "mapped" : "not mapped");
    return STATUS_SUCCESS;
}

NTSTATUS
NpuEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PDEVICE_CONTEXT ctx = GetContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (ctx->Cru) {
        MmUnmapIoSpace(ctx->Cru, PAGE_SIZE);
        ctx->Cru = NULL;
    }
    if (ctx->Pmu) {
        MmUnmapIoSpace(ctx->Pmu, PAGE_SIZE);
        ctx->Pmu = NULL;
    }
    if (ctx->Regs) {
        MmUnmapIoSpace(ctx->Regs, REGS_MIN_LEN);
        ctx->Regs = NULL;
    }
    return STATUS_SUCCESS;
}

/* No power change: the firmware powered the NPU and nothing here moves it. */
NTSTATUS
NpuEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PDEVICE_CONTEXT ctx = GetContext(Device);
    rknpu_clock_info c;
    ULONG n;

    UNREFERENCED_PARAMETER(PreviousState);

    if (NpuReadClocks(ctx, &c)) {
        RKNPU_LOG("D0 entry: STATUS0 0x%08x REPAIR0 0x%08x CLKSEL86 0x%08x, gpll %u Hz, dsu0 %u Hz",
                  c.pmu_status0, c.pmu_repair0, c.cru_clksel86, c.gpll_hz, c.dsu0_hz);
        if (!NpuDomainsOn(&c)) {
            /* _STA said on when the device was enumerated; something has
             * changed since. Reading the NPU now could be an SError. */
            RKNPU_LOG("D0 entry: the NPU is not powered or not clocked; failing the start");
            return STATUS_DEVICE_NOT_READY;
        }
    }

    for (n = 0; n < RKNPU_NUM_CORES; n++) {
        ULONG base = NpuCoreBase(n);

        RKNPU_LOG("core %u version=0x%08x version_num=0x%08x mmu status=0x%08x/0x%08x",
                  n, NpuRd(ctx, base + PC_VERSION), NpuRd(ctx, base + PC_VERSION_NUM),
                  NpuRd(ctx, base + NPU_MMU0_OFF + RK_MMU_STATUS),
                  NpuRd(ctx, base + NPU_MMU1_OFF + RK_MMU_STATUS));
        /* stale interrupt state, before the interrupts are connected */
        NpuWr(ctx, base + PC_INTERRUPT_MASK, 0);
        NpuWr(ctx, base + PC_INTERRUPT_CLEAR, PC_INTERRUPT_ALL);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NpuEvtD0EntryPostInterrupts(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    UNREFERENCED_PARAMETER(PreviousState);
    GetContext(Device)->InterruptsConnected = TRUE;
    return STATUS_SUCCESS;
}

/* No power change and no register access. */
NTSTATUS
NpuEvtD0ExitPreInterrupts(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    UNREFERENCED_PARAMETER(TargetState);
    GetContext(Device)->InterruptsConnected = FALSE;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------- dispatch */

static VOID
NpuFillCore(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N,
    _In_ BOOLEAN ReadHw,
    _Out_ rknpu_core_info *O
    )
{
    CORE_STATE *core = &Ctx->Core[N];
    ULONG base = NpuCoreBase(N);
    ULONG i;

    if (ReadHw) {
        O->pc_version          = NpuRd(Ctx, base + PC_VERSION);
        O->pc_version_num      = NpuRd(Ctx, base + PC_VERSION_NUM);
        O->pc_operation_enable = NpuRd(Ctx, base + PC_OPERATION_ENABLE);
        O->pc_interrupt_mask   = NpuRd(Ctx, base + PC_INTERRUPT_MASK);
        O->pc_interrupt_status = NpuRd(Ctx, base + PC_INTERRUPT_STATUS);
        O->pc_interrupt_raw    = NpuRd(Ctx, base + PC_INTERRUPT_RAW_STATUS);
        O->pc_task_con         = NpuRd(Ctx, base + PC_TASK_CON);
        O->pc_task_status      = NpuRd(Ctx, base + PC_TASK_STATUS);
        for (i = 0; i < RKNPU_MMUS_PER_CORE; i++) {
            O->mmu_status[i]      = NpuRd(Ctx, base + MmuOff[i] + RK_MMU_STATUS);
            O->mmu_dte_addr[i]    = NpuRd(Ctx, base + MmuOff[i] + RK_MMU_DTE_ADDR);
            O->mmu_int_rawstat[i] = NpuRd(Ctx, base + MmuOff[i] + RK_MMU_INT_RAWSTAT);
            O->mmu_int_mask[i]    = NpuRd(Ctx, base + MmuOff[i] + RK_MMU_INT_MASK);
        }
    }
    O->gsiv           = core->Gsiv;
    O->isr_count      = (ULONG)core->IsrCount;
    O->isr_unclaimed  = (ULONG)core->IsrUnclaimed;
    O->dpc_count      = (ULONG)core->DpcCount;
    O->last_pc_status = core->LastPcStatus;
    for (i = 0; i < RKNPU_MMUS_PER_CORE; i++) {
        O->last_mmu_int_status[i] = core->LastMmuIntStatus[i];
        O->last_mmu_fault_addr[i] = core->LastMmuFault[i];
    }
}

VOID
NpuEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    PDEVICE_CONTEXT ctx = GetContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR info = 0;
    ULONG i;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_RKNPU_GET_INFO: {
        rknpu_info *o;
        BOOLEAN on;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*o), (PVOID *)&o, NULL);
        if (!NT_SUCCESS(status))
            break;
        RtlZeroMemory(o, sizeof(*o));
        o->interface_version = RKNPU_INTERFACE_VERSION;
        o->struct_size = sizeof(*o);
        o->core_count = RKNPU_NUM_CORES;
        o->regs_pa = (ULONGLONG)ctx->RegsPa.QuadPart;
        o->regs_len = (ULONG)ctx->RegsLen;
        if (ctx->InterruptsConnected)
            o->flags |= RKNPU_INFO_INTERRUPTS_CONNECTED;
        if (NpuReadClocks(ctx, &o->clocks)) {
            o->flags |= RKNPU_INFO_CLOCKS_VALID;
            on = NpuDomainsOn(&o->clocks);
        } else {
            on = TRUE;      /* _STA's word for it */
        }
        if (on)
            o->flags |= RKNPU_INFO_DOMAINS_ON;
        for (i = 0; i < RKNPU_NUM_CORES; i++)
            NpuFillCore(ctx, i, on, &o->core[i]);
        info = sizeof(*o);
        break;
    }
    case IOCTL_RKNPU_READ_REGS: {
        rknpu_read_regs_in *in;
        ULONG *o, off, n;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
        if (!NT_SUCCESS(status))
            break;
        off = in->offset;           /* METHOD_BUFFERED: in and out share the buffer */
        n = in->count;
        /* only the blocks the device tree describes (rknpu_ioctl.h) */
        if (!rknpu_read_range_ok(off, n)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (!NpuSafeToRead(ctx)) {
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
        status = WdfRequestRetrieveOutputBuffer(Request, (size_t)n * 4, (PVOID *)&o, NULL);
        if (!NT_SUCCESS(status))
            break;
        for (i = 0; i < n; i++)
            o[i] = NpuRd(ctx, off + i * 4);
        info = (ULONG_PTR)n * 4;
        break;
    }
    default:
        /* includes the function codes reserved for M3 */
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    WdfRequestCompleteWithInformation(Request, status, info);
}

/* ------------------------------------------------------------- setup */

static NTSTATUS
NpuCreateInterrupt(
    _In_ WDFDEVICE Device,
    _In_ ULONG CoreIndex,
    _Out_ WDFINTERRUPT *Interrupt
    )
{
    WDF_INTERRUPT_CONFIG icfg;
    WDF_OBJECT_ATTRIBUTES attr;
    NTSTATUS status;

    WDF_INTERRUPT_CONFIG_INIT(&icfg, NpuEvtIsr, NpuEvtDpc);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, INTERRUPT_CONTEXT);
    status = WdfInterruptCreate(Device, &icfg, &attr, Interrupt);
    if (NT_SUCCESS(status))
        GetInterruptContext(*Interrupt)->CoreIndex = CoreIndex;
    return status;
}

NTSTATUS
NpuEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    NTSTATUS status;
    ULONG n;
    DECLARE_CONST_UNICODE_STRING(name, RKNPU_NT_NAME);
    DECLARE_CONST_UNICODE_STRING(link, RKNPU_DOS_NAME);
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = NpuEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = NpuEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = NpuEvtD0Entry;
    pnp.EvtDeviceD0EntryPostInterruptsEnabled = NpuEvtD0EntryPostInterrupts;
    pnp.EvtDeviceD0ExitPreInterruptsDisabled = NpuEvtD0ExitPreInterrupts;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WdfDeviceInitSetExclusive(DeviceInit, TRUE);
    status = WdfDeviceInitAssignName(DeviceInit, &name);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
    if (!NT_SUCCESS(status))
        return status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status))
        return status;

    ctx = GetContext(device);
    ctx->Device = device;

    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status))
        return status;

    /* Interrupt objects take the interrupt resources in creation order:
     * GSIV 279 (core 0), then 280 (core 1). */
    for (n = 0; n < RKNPU_NUM_CORES; n++) {
        status = NpuCreateInterrupt(device, n, &ctx->Core[n].Interrupt);
        if (!NT_SUCCESS(status))
            return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchSequential);
    qcfg.EvtIoDeviceControl = NpuEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfIoQueueCreate(device, &qcfg, &attr, &ctx->Queue);
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG cfg;

    WDF_DRIVER_CONFIG_INIT(&cfg, NpuEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
