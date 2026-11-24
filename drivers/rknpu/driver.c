/*
 * driver.c - RK3576 NPU (RKNN, two cores): device, interrupts, M2 IOCTLs
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
 *   - creates \\.\RKNPU, administrators and SYSTEM only (interactive users
 *     too with Device Parameters\UserAccess = 1 and the IOMMU on), one handle at a
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
 * M6 (iommu.c) programs the four IOMMU instances in the same window: their
 * page table, paging on before a job, off before a reset and at D0 exit,
 * and the fault interrupt. It also lets more than one handle open the device
 * and makes the queue parallel, so both cores can be driven at once.
 *
 * perf.c publishes the job counters as Windows performance counters
 * ("RK3576 NPU", one instance per core, and "RK3576 NPU Memory").
 *
 * M3 (m3.c) adds buffers, job submission, completion and a core reset, all
 * behind IOCTLs of their own (0x810 on). The writes above stay the only ones
 * until one of those is called; then the job registers rocket writes are
 * written too (m3.c says which). The ISR hands a job's DPU interrupt to m3.c
 * first and keeps the M2 handling for everything else.
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
EVT_WDF_IO_IN_CALLER_CONTEXT NpuEvtIoInCallerContext;
EVT_WDF_FILE_CLEANUP NpuEvtFileCleanup;
EVT_WDF_DEVICE_FILE_CREATE NpuEvtFileCreate;
EVT_WDF_OBJECT_CONTEXT_CLEANUP NpuEvtDeviceCleanup;

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
BOOLEAN
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
    ULONG st;

    UNREFERENCED_PARAMETER(MessageID);

    /* A job's DPU interrupt (M3): acked, and the next task started, there.
     * The PC block is then left alone: the next task may already be running. */
    if (NpuM3Isr(ctx, n))
        ours = TRUE;

    /* The PC block outside a job. The mask is 0 then, so this is not
     * expected to fire; if it does, do what rocket does on completion. */
    st = ours ? 0 : NpuRd(ctx, base + PC_INTERRUPT_STATUS);
    if (st) {
        NpuWr(ctx, base + PC_INTERRUPT_MASK, 0);
        NpuWr(ctx, base + PC_OPERATION_ENABLE, 0);
        NpuWr(ctx, base + PC_INTERRUPT_CLEAR, PC_INTERRUPT_ALL);
        core->LastPcStatus = st;
        ours = TRUE;
    }

    /* The core's two IOMMU instances share the line (M6: a fault stops the job). */
    if (NpuMmuIsr(ctx, n))
        ours = TRUE;

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
    ULONG n = GetInterruptContext(Interrupt)->CoreIndex;

    UNREFERENCED_PARAMETER(AssociatedObject);

    InterlockedIncrement(&ctx->Core[n].DpcCount);
    NpuM3Dpc(ctx, n);       /* completes a finished job */
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
    NpuM3D0Entry(ctx);      /* records each core's version, to check a reset against */
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

/* No power change. A job still running (M3) is stopped; otherwise no register access. */
NTSTATUS
NpuEvtD0ExitPreInterrupts(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    UNREFERENCED_PARAMETER(TargetState);
    GetContext(Device)->InterruptsConnected = FALSE;
    NpuM3D0Exit(GetContext(Device));
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
    O->jobs_submitted = (ULONG)core->JobsSubmitted;
    O->jobs_completed = (ULONG)core->JobsCompleted;
    O->jobs_timed_out = (ULONG)core->JobsTimedOut;
    O->resets         = (ULONG)core->Resets;
    O->mmu_faults     = (ULONG)core->MmuFaults;
    O->mmu_enables    = (ULONG)core->MmuEnables;
    O->cbuf_waits     = (ULONG)core->CbufWaits;
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
        o->m3_version = RKNPU_M3_VERSION;
        o->bo_count = ctx->BoCount;
        o->bo_kib = (ULONG)(ctx->BoBytes / 1024);
        o->m4_version = RKNPU_M4_VERSION;
        o->copy_arena_bytes = RKNPU_COPY_ARENA_BYTES;
        o->bo_max_kib = RKNPU_BO_MAX_SIZE / 1024;
        o->bo_max_total_kib = (ULONG)(ctx->BoMaxTotal / 1024);
        o->m6_version = RKNPU_M6_VERSION;
        o->mem_mode = ctx->Mmu.On ? RKNPU_MEM_IOMMU : RKNPU_MEM_BYPASS;
        o->bo_cached = ctx->CachedBos ? 1 : 0;
        o->iova_kib = ctx->Mmu.ChunksUsed << (NPU_IOVA_CHUNK_SHIFT - 10);
        o->pt_pages = ctx->Mmu.PtPages;
        o->m7_version = RKNPU_M7_VERSION;
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
        /* M3 (m3.c); anything it does not know, reserved codes included,
         * is STATUS_INVALID_DEVICE_REQUEST */
        NpuM3DeviceControl(ctx, Request, IoControlCode, &status, &info);
        break;
    }
    WdfRequestCompleteWithInformation(Request, status, info);
}

/*
 * MAP_BO, UNMAP_BO and FREE_BO need the caller's address space (a mapping is
 * made and undone in the process that holds it); m3.c completes those here.
 * Everything else goes to the queue as in M2.
 */
VOID
NpuEvtIoInCallerContext(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    WDF_REQUEST_PARAMETERS params;
    NTSTATUS status;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    if (params.Type == WdfRequestTypeDeviceControl &&
        NpuM3InCallerContext(GetContext(Device), Request, params.Parameters.DeviceIoControl.IoControlCode))
        return;
    status = WdfDeviceEnqueueRequest(Device, Request);
    if (!NT_SUCCESS(status))
        WdfRequestComplete(Request, status);
}

/* The handle closes, in its process: its jobs end and its buffers go (m3.c). */
VOID
NpuEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject
    )
{
    NpuM3FileCleanup(GetContext(WdfFileObjectGetDevice(FileObject)), FileObject);
}

VOID
NpuEvtDeviceCleanup(
    _In_ WDFOBJECT Object
    )
{
    NpuPerfUnregister();    /* first: no counter callback reads the context after this */
    NpuM3FreeAll(GetContext((WDFDEVICE)Object));
    NpuMmuFree(GetContext((WDFDEVICE)Object));     /* M6: the page tables, last */
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

/*
 * USER ACCESS (Device Parameters\UserAccess = 1, set by charsiu-install.ps1
 * -UserAccess and read when the device is added). The device then also
 * admits interactive users, so charsiu runs from an ordinary terminal and the
 * desktop app, as it does on Linux, where the accel node is the video
 * group's. It is only granted with the IOMMU on: every address a stream can
 * program is one of five registers, each checked against the caller's own
 * buffers (m3.c), and the IOMMU confines the NPU to the buffers that are
 * mapped. In bypass mode (M6Iommu = 0, or M4AllowUnlisted = 1) a stream's
 * unchecked sizes reach physical memory, so the device stays administrators
 * only, and if the IOMMU could not be set up at start this refuses every
 * caller that is not an administrator.
 */
static BOOLEAN
NpuCallerIsAdmin(
    _In_ WDFREQUEST Request
    )
{
    PIRP irp = WdfRequestWdmGetIrp(Request);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    PIO_SECURITY_CONTEXT sc = sp->Parameters.Create.SecurityContext;

    if (!sc || !sc->AccessState)
        return FALSE;
    return SeTokenIsAdmin(SeQuerySubjectContextToken(&sc->AccessState->SubjectSecurityContext));
}

VOID
NpuEvtFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject
    )
{
    PDEVICE_CONTEXT ctx = GetContext(Device);

    UNREFERENCED_PARAMETER(FileObject);
    if (ctx->UserAccess && !ctx->Mmu.On && WdfRequestGetRequestorMode(Request) == UserMode &&
        !NpuCallerIsAdmin(Request)) {
        RKNPU_LOG("open refused: UserAccess needs the IOMMU, and it is off");
        WdfRequestComplete(Request, STATUS_ACCESS_DENIED);
        return;
    }
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

/* Device Parameters values that decide the device's security, read before
 * the device object exists (m3.c reads the rest at start). */
static ULONG
NpuInitQueryULong(
    _In_ WDFKEY Key,
    _In_ PCWSTR Name,
    _In_ ULONG Default
    )
{
    UNICODE_STRING n;
    ULONG v;

    RtlInitUnicodeString(&n, Name);
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &n, &v)))
        return v;
    return Default;
}

NTSTATUS
NpuEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_FILEOBJECT_CONFIG fcfg;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    NTSTATUS status;
    ULONG n;
    DECLARE_CONST_UNICODE_STRING(name, RKNPU_NT_NAME);
    DECLARE_CONST_UNICODE_STRING(link, RKNPU_DOS_NAME);
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    /* interactive users may read and write (open the device, issue its
     * IOCTLs); see NpuEvtFileCreate for when */
    DECLARE_CONST_UNICODE_STRING(sddlUsers, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)");
    BOOLEAN userAccess = FALSE;
    WDFKEY key;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = NpuEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = NpuEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = NpuEvtD0Entry;
    pnp.EvtDeviceD0EntryPostInterruptsEnabled = NpuEvtD0EntryPostInterrupts;
    pnp.EvtDeviceD0ExitPreInterruptsDisabled = NpuEvtD0ExitPreInterrupts;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_FILEOBJECT_CONFIG_INIT(&fcfg, NpuEvtFileCreate, WDF_NO_EVENT_CALLBACK, NpuEvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fcfg, WDF_NO_OBJECT_ATTRIBUTES);
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, NpuEvtIoInCallerContext);

    /* M6: not exclusive; charsiu opens one handle per core. Each handle
     * owns its buffers and jobs (m3.c), as in M4. */
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);
    status = WdfDeviceInitAssignName(DeviceInit, &name);
    if (!NT_SUCCESS(status))
        return status;
    if (NT_SUCCESS(WdfFdoInitOpenRegistryKey(DeviceInit, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        userAccess = NpuInitQueryULong(key, L"UserAccess", 0) == 1 &&
                     NpuInitQueryULong(key, L"M6Iommu", 1) != 0 &&
                     NpuInitQueryULong(key, L"M4AllowUnlisted", 0) != 1;
        WdfRegistryClose(key);
    }
    status = WdfDeviceInitAssignSDDLString(DeviceInit, userAccess ? &sddlUsers : &sddl);
    if (!NT_SUCCESS(status))
        return status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    attr.EvtCleanupCallback = NpuEvtDeviceCleanup;
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status))
        return status;

    ctx = GetContext(device);
    ctx->Device = device;
    ctx->UserAccess = userAccess;
    if (userAccess)
        RKNPU_LOG("UserAccess = 1: interactive users may open the device (IOMMU mode)");
    status = NpuM3DeviceInit(ctx);
    if (!NT_SUCCESS(status))
        return status;

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

    /* M6: parallel, so a WAIT that blocks does not hold up the other core */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoDeviceControl = NpuEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(device, &qcfg, &attr, &ctx->Queue);
    if (!NT_SUCCESS(status))
        return status;

    /* Performance counters (perf.c): optional, the device starts without them */
    NpuPerfRegister(ctx);
    return STATUS_SUCCESS;
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
