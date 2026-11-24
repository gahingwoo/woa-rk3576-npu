/*
 * m3.c - RK3576 NPU, M3: buffers, job submission, completion, core reset
 *
 * Reached only through the IOCTLs from 0x810 on (rknpu_ioctl.h, M3 section).
 * Until one of them is called nothing here touches the hardware, allocates
 * memory or writes the registry; the M2 paths (GET_INFO, READ_REGS) are
 * unchanged apart from reporting the M3 counters.
 *
 * MEMORY. Every buffer is MmAllocateContiguousNodeMemory below 4 GiB,
 * PAGE_WRITECOMBINE: Normal non-cacheable on ARM64, so the CPU never holds a
 * line the NPU (not coherent, _CCA 0) would miss, and unaligned CPU access
 * still works. The one optional user mapping (MAP_BO) uses MmWriteCombined
 * too, so no page is ever mapped with two different attributes. This is the
 * vdpu383 driver's model (woa-rk3576 drivers/video/vdpu383). Without MAP_BO a
 * test moves data with WRITE_BO and READ_BO and no NPU page is ever mapped
 * into a process.
 *
 * IOMMU. Left as the firmware leaves it: reset state, paging off (edk2-rk3576
 * Npu.c logs STATUS 0x18 for all four instances; M2 read the same under
 * Windows). The NPU address of a buffer is then its physical address. SUBMIT
 * refuses a core whose IOMMU has paging on, since every address in the
 * stream would then be translated through tables nobody set up.
 *
 * With paging off nothing stands between a stream and physical memory, so
 * SUBMIT reads every stream before it starts it (NpuCheckStreamLocked) and
 * refuses one that writes the PC block or puts an address outside the
 * caller's buffers into one of the address registers rknpu_regcmd.h lists.
 * The list is incomplete by construction (rknpu_regcmd.h says why); the
 * check catches a wrong buffer address from rknputest or charsiu, not a
 * hostile stream. A stream's buffer may not be user-mapped at SUBMIT, and
 * MAP_BO is refused while a job runs, so a checked stream cannot change.
 *
 * SUBMISSION is rocket_job_hw_submit() (drivers/accel/rocket/rocket_job.c,
 * with v17-0009's RK3576 TASK_CON), one task at a time, the next task started
 * from the ISR as rocket_job_next_locked() does. COMPLETION is
 * rocket_job_irq_handler() plus rocket_job_handle_irq(): on a DPU interrupt,
 * INTERRUPT_MASK = 0, then OPERATION_ENABLE = 0 and INTERRUPT_CLEAR = 0x1ffff
 * under the same lock that SUBMIT holds when it writes OPERATION_ENABLE.
 *
 * TIMEOUT AND RESET follow rocket_reset(): mask and clear, wait for any
 * handler still running (KeFlushQueuedDpcs is the synchronize_irq here),
 * then the core's soft reset, which is the SSDT's _DSM function 1
 * (SRST_A_RKNN0/1 for 10 us, rocket_core_reset()). The CRU is not ours, so
 * the driver never writes it.
 *
 * M4 (rknpu_ioctl.h, M4 section). SUBMIT with RKNPU_SUBMIT_COPY copies each
 * stream into the core's copy arena (contiguous, below 4 GiB, never mapped
 * into a process), checks the copy and starts the PC on it, so a stream in a
 * user-mapped buffer can be run safely: what was checked is what runs. The
 * check now also refuses any register charsiu never writes
 * (rknpu_regcmd_allow.h). With copies, the M3 rules that froze a checked
 * stream (no MAP_BO, no WRITE_BO while a job runs) only matter while an M3
 * style job runs, and FREE_BO only waits for the caller's own jobs. WAIT can
 * poll, and a finished job is kept in a short history so its WAIT still
 * answers after another handle's job took the core.
 *
 * M6 (rknpu_ioctl.h, M6 section; the IOMMU itself is iommu.c). In IOMMU
 * mode a buffer is MmAllocatePagesForMdlEx pages, anywhere in memory, mapped
 * at an IOVA, and its dma_address is that IOVA; SUBMIT turns paging on for
 * the core when it finds it off. Buffers are cached unless M6CachedBos = 0,
 * and PREP_BO / FINI_BO do the cache maintenance (cache.asm) the
 * non-coherent NPU needs. The copy arena is mapped read only for the NPU.
 * A buffer is found by its handle through a hash and by an address through
 * the IOVA chunk table, so neither SUBMIT's check nor PREP/FINI walks the
 * list of a thousand buffers. Bypass mode (M6Iommu = 0) is M4 exactly.
 *
 * LOCKING. Every wait (job completion, the _DSM call, a breadcrumb flush)
 * happens at PASSIVE_LEVEL holding at most the two wait locks. The interrupt
 * spin lock is held only around register writes and job bookkeeping, never
 * across a wait; the DPC takes nothing else. M6: WAIT waits for a job with
 * no lock held (the queue is parallel), so a WAIT on one core does not hold
 * up a SUBMIT on the other; it takes JobLock again only to collect the
 * result, or to stop a job that timed out. The IOMMU registers and tables
 * are touched under BoLock.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rknpu.h"
#include <acpiioct.h>

/* edk2-rk3576 Silicon/Rockchip/RK3576/AcpiTables/Npu.asl, _DSM */
static const GUID NpuDsmGuid =
    { 0x8599ea0d, 0x52b6, 0x41f1, { 0x99, 0x3a, 0xce, 0x31, 0x48, 0x72, 0x56, 0x6c } };
#define NPU_DSM_REVISION        0
#define NPU_DSM_FN_RESET_CORE   1

#define FOURGIB                 0x100000000ull

static const ULONG MmuInst[RKNPU_MMUS_PER_CORE] = { NPU_MMU0_OFF, NPU_MMU1_OFF };

/* ------------------------------------------------- cache maintenance (M6) */

/* The CPU's writes to [Off, Off + Len) of a cached buffer reach memory. */
static VOID
BoClean(
    _In_ const NPU_BO *Bo,
    _In_ SIZE_T Off,
    _In_ SIZE_T Len
    )
{
    if (Bo->Cached && Len)
        RknpuDcClean((PUCHAR)Bo->Kva + Off, Len);
}

/* ... and the CPU will read that range from memory, not from its caches. */
static VOID
BoCleanInvalidate(
    _In_ const NPU_BO *Bo,
    _In_ SIZE_T Off,
    _In_ SIZE_T Len
    )
{
    if (Bo->Cached && Len)
        RknpuDcCleanInvalidate((PUCHAR)Bo->Kva + Off, Len);
}

static ULONG
QpcToUs(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ LONGLONG Ticks
    )
{
    if (Ticks <= 0 || Ctx->QpcFreq.QuadPart == 0)
        return 0;
    return (ULONG)(Ticks * 1000000 / Ctx->QpcFreq.QuadPart);
}

/* ------------------------------------------------------- breadcrumbs */

static VOID
CrumbSet(
    _In_ WDFKEY Key,
    _In_ PCWSTR Name,
    _In_ ULONG Value
    )
{
    UNICODE_STRING n;

    RtlInitUnicodeString(&n, Name);
    (VOID)WdfRegistryAssignULong(Key, &n, Value);
}

/*
 * Six DWORDs under Device Parameters, then ZwFlushKey: a hang that takes the
 * board down still leaves the last stage on disk. PASSIVE_LEVEL only.
 */
static VOID
NpuCrumb(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Stage,
    _In_ ULONGLONG Seq,
    _In_ ULONG Core,
    _In_ ULONG Value1,
    _In_ ULONG Value2
    )
{
    NTSTATUS status;
    LONG count;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !Ctx->CrumbKey)
        return;
    count = InterlockedIncrement(&Ctx->CrumbCounts[Stage & 15]);
    /*
     * M4: charsiu makes about 1600 buffers for Llama-3.2-1B, and a flushed
     * crumb for each create, map and free would be thousands of registry
     * flushes on eMMC. The buffer stages leave the first 32 of each kind,
     * then every 256th; the job stages are the caller's choice
     * (RKNPU_SUBMIT_NO_CRUMBS), and a timeout or a refusal always writes.
     */
    if ((Stage == RKNPU_CRUMB_BO_CREATE || Stage == RKNPU_CRUMB_BO_MAP ||
         Stage == RKNPU_CRUMB_BO_UNMAP || Stage == RKNPU_CRUMB_BO_FREE) &&
        count > 32 && count % 256 != 0)
        return;
    CrumbSet(Ctx->CrumbKey, L"M3Stage", Stage);
    CrumbSet(Ctx->CrumbKey, L"M3Seq", (ULONG)Seq);
    CrumbSet(Ctx->CrumbKey, L"M3Core", Core);
    CrumbSet(Ctx->CrumbKey, L"M3Value1", Value1);
    CrumbSet(Ctx->CrumbKey, L"M3Value2", Value2);
    CrumbSet(Ctx->CrumbKey, L"M3Count", (ULONG)count);
    status = ZwFlushKey(WdfRegistryWdmGetHandle(Ctx->CrumbKey));
    if (!NT_SUCCESS(status))
        RKNPU_LOG("breadcrumbs: ZwFlushKey 0x%08x", status);
}

/* ------------------------------------------------------------- setup */

NTSTATUS
NpuM3DeviceInit(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    WDF_OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    ULONG n;

    InitializeListHead(&Ctx->Bos);
    for (n = 0; n < RKNPU_BO_HASH; n++)
        InitializeListHead(&Ctx->BoHash[n]);
    KeQueryPerformanceCounter(&Ctx->QpcFreq);
    for (n = 0; n < RKNPU_NUM_CORES; n++)
        KeInitializeEvent(&Ctx->Core[n].Done, NotificationEvent, FALSE);

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Ctx->Device;
    status = WdfWaitLockCreate(&attr, &Ctx->JobLock);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfWaitLockCreate(&attr, &Ctx->BoLock);
    if (!NT_SUCCESS(status))
        return status;

    /* Opened here, once, so no two callers race to open it. Writing it
     * starts with the first M3 call; failing to open it is not fatal. */
    /* M6 defaults: the driver's IOMMU, cached buffers */
    Ctx->Mmu.On = TRUE;
    Ctx->CachedBos = TRUE;
    status = WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ | KEY_WRITE,
                                      WDF_NO_OBJECT_ATTRIBUTES, &Ctx->CrumbKey);
    if (!NT_SUCCESS(status)) {
        RKNPU_LOG("breadcrumbs: cannot open Device Parameters: 0x%08x", status);
        Ctx->CrumbKey = NULL;
    } else {
        UNICODE_STRING name;
        ULONG v = 0;

        /* M4: the back-out for a register the list missed (rknpu_regcmd.h) */
        RtlInitUnicodeString(&name, L"M4AllowUnlisted");
        if (NT_SUCCESS(WdfRegistryQueryULong(Ctx->CrumbKey, &name, &v)) && v == 1) {
            Ctx->AllowUnlisted = TRUE;
            RKNPU_LOG("M4AllowUnlisted = 1: registers charsiu never writes are let through (logged)");
        }
        /* M6: M6Iommu = 0 is M4's bypass mode, and then uncached by default */
        RtlInitUnicodeString(&name, L"M6Iommu");
        if (NT_SUCCESS(WdfRegistryQueryULong(Ctx->CrumbKey, &name, &v)) && v == 0) {
            Ctx->Mmu.On = FALSE;
            Ctx->CachedBos = FALSE;
        }
        RtlInitUnicodeString(&name, L"M6CachedBos");
        if (NT_SUCCESS(WdfRegistryQueryULong(Ctx->CrumbKey, &name, &v)))
            Ctx->CachedBos = v ? TRUE : FALSE;
    }
    if (Ctx->Mmu.On && !NT_SUCCESS(NpuMmuInit(Ctx))) {
        RKNPU_LOG("IOMMU mode: no memory for the page directory; running in bypass mode");
        Ctx->Mmu.On = FALSE;
    }
    Ctx->BoMaxTotal = Ctx->Mmu.On ? RKNPU_BO_MAX_TOTAL_IOMMU : RKNPU_BO_MAX_TOTAL;
    RKNPU_LOG("memory: %s, %s buffers, at most %Iu MiB", Ctx->Mmu.On ? "IOMMU" : "bypass (M4)",
              Ctx->CachedBos ? "cached" : "write-combined", Ctx->BoMaxTotal >> 20);
    return STATUS_SUCCESS;
}

VOID
NpuM3D0Entry(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    ULONG n;

    for (n = 0; n < RKNPU_NUM_CORES; n++)
        Ctx->Core[n].Version = NpuRd(Ctx, NpuCoreBase(n) + PC_VERSION);
}

/* -------------------------------------------------------------- jobs */

static BOOLEAN
JobActive(
    _In_ const CORE_JOB *J
    )
{
    return J->InFlight || J->CompletePending;
}

/*
 * A job of Owner's whose streams are in Owner's buffers (M3, no
 * RKNPU_SUBMIT_COPY): while one runs, nothing may make those streams
 * writable. Only Owner can write or map its own buffers (every buffer call
 * looks the handle up with its owner), so another handle's WRITE_BO or
 * MAP_BO cannot touch them and is not held up. M3 and M4 asked "any job of
 * any handle", which refused the second handle's WRITE_BO with
 * ERROR_BUSY the moment two handles ran at once (M6 board run, rknputest
 * m6, 2026-11-24).
 */
static BOOLEAN
OwnerUncopiedJobActive(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFFILEOBJECT Owner
    )
{
    ULONG n;

    for (n = 0; n < RKNPU_NUM_CORES; n++)
        if (JobActive(&Ctx->Core[n].Job) && !Ctx->Core[n].Job.Copied && Ctx->Core[n].Job.Owner == Owner)
            return TRUE;
    return FALSE;
}

/* A job of this handle runs: its buffers may be in use by the NPU. */
static BOOLEAN
OwnerJobActive(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFFILEOBJECT Owner
    )
{
    ULONG n;

    for (n = 0; n < RKNPU_NUM_CORES; n++)
        if (JobActive(&Ctx->Core[n].Job) && Ctx->Core[n].Job.Owner == Owner)
            return TRUE;
    return FALSE;
}

/*
 * rocket_job_hw_submit() for the job's next task. Called with the core's
 * interrupt lock held: from SUBMIT under WdfInterruptAcquireLock, or from the
 * ISR, which runs under it.
 */
static VOID
NpuHwSubmit(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    CORE_JOB *j = &Ctx->Core[N].Job;
    const rknpu_task *t = &j->Tasks[j->NextTask];
    ULONG base = NpuCoreBase(N);
    ULONG sp = S_POINTER_PP_EN | S_POINTER_EXECUTER_PP_EN | S_POINTER_PP_MODE | (S_POINTER_CORE_BIT * N);

    j->NextTask++;
    NpuWr(Ctx, base + PC_BASE_ADDRESS, 0x1);
    NpuWr(Ctx, base + CNA_S_POINTER, sp);
    NpuWr(Ctx, base + CORE_S_POINTER, sp);
    NpuWr(Ctx, base + PC_BASE_ADDRESS, t->regcmd);
    NpuWr(Ctx, base + PC_REGISTER_AMOUNTS, ((t->regcmd_count + 1) / 2 - 1) & PC_DATA_AMOUNT_MASK);
    NpuWr(Ctx, base + PC_INTERRUPT_MASK, PC_INT_DPU_0 | PC_INT_DPU_1);
    NpuWr(Ctx, base + PC_INTERRUPT_CLEAR, PC_INT_DPU_0 | PC_INT_DPU_1);
    NpuWr(Ctx, base + PC_TASK_CON, RK3576_TASK_LAST_LAYER_CLEAR | RK3576_TASK_COUNT_CLEAR |
                                   RK3576_TASK_PP_EN | RK3576_TASK_NUMBER(1));
    NpuWr(Ctx, base + PC_TASK_DMA_BASE_ADDR, 0);
    NpuWr(Ctx, base + PC_OPERATION_ENABLE, 1);
}

/*
 * From NpuEvtIsr, at DIRQL with the interrupt lock held. TRUE if this was a
 * job's DPU interrupt (claimed and acked). rocket_job_irq_handler() keys on
 * the RAW status; so does this, but only while a job is in flight, so the M2
 * path still sees anything else.
 */
BOOLEAN
NpuM3Isr(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    CORE_JOB *j = &Ctx->Core[N].Job;
    ULONG base = NpuCoreBase(N);
    ULONG raw, ts;

    if (!j->InFlight)
        return FALSE;
    raw = NpuRd(Ctx, base + PC_INTERRUPT_RAW_STATUS);
    if (!(raw & (PC_INT_DPU_0 | PC_INT_DPU_1)))
        return FALSE;
    ts = NpuRd(Ctx, base + PC_TASK_STATUS);

    NpuWr(Ctx, base + PC_INTERRUPT_MASK, 0);            /* rocket_job_irq_handler() */
    NpuWr(Ctx, base + PC_OPERATION_ENABLE, 0);          /* rocket_job_handle_irq() */
    NpuWr(Ctx, base + PC_INTERRUPT_CLEAR, PC_INTERRUPT_ALL);

    j->IsrCount++;
    j->TasksDone++;
    j->RawStatus = raw;
    j->TaskStatus = ts;
    if (raw & (PC_INT_DMA_READ_ERROR | PC_INT_DMA_WRITE_ERROR)) {
        j->Result = RKNPU_JOB_DMA_ERROR;
    } else if (j->NextTask < j->TaskCount) {
        NpuHwSubmit(Ctx, N);                            /* rocket_job_next_locked() */
        return TRUE;
    } else {
        j->Result = RKNPU_JOB_DONE;
    }
    j->T1 = KeQueryPerformanceCounter(NULL);
    j->InFlight = FALSE;
    j->CompletePending = TRUE;
    return TRUE;
}

/* From NpuEvtDpc, at DISPATCH_LEVEL. */
VOID
NpuM3Dpc(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    CORE_STATE *core = &Ctx->Core[N];
    BOOLEAN done;

    WdfInterruptAcquireLock(core->Interrupt);
    done = core->Job.CompletePending;
    core->Job.CompletePending = FALSE;
    WdfInterruptReleaseLock(core->Interrupt);
    if (done) {
        InterlockedIncrement(&core->JobsCompleted);
        KeSetEvent(&core->Done, IO_NO_INCREMENT, FALSE);
    }
}

/*
 * M6: an IOMMU on core N raised a page fault or bus error while its job ran
 * (iommu.c, NpuMmuIsr). Stop the job as the completion path does and finish
 * it with RKNPU_JOB_MMU_FAULT; its WAIT resets the core. DIRQL, the core's
 * interrupt lock held (the ISR holds it).
 */
VOID
NpuM3JobFaultLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N,
    _In_ ULONG Iova,
    _In_ ULONG Status
    )
{
    CORE_JOB *j = &Ctx->Core[N].Job;
    ULONG base = NpuCoreBase(N);

    if (!j->InFlight)
        return;
    j->RawStatus = NpuRd(Ctx, base + PC_INTERRUPT_RAW_STATUS);
    j->TaskStatus = NpuRd(Ctx, base + PC_TASK_STATUS);
    NpuWr(Ctx, base + PC_INTERRUPT_MASK, 0);
    NpuWr(Ctx, base + PC_OPERATION_ENABLE, 0);
    NpuWr(Ctx, base + PC_INTERRUPT_CLEAR, PC_INTERRUPT_ALL);
    j->FaultAddr = Iova;
    j->FaultStatus = Status;
    j->Result = RKNPU_JOB_MMU_FAULT;
    j->T1 = KeQueryPerformanceCounter(NULL);
    j->InFlight = FALSE;
    j->CompletePending = TRUE;
}

/*
 * Stop a job the interrupt has not finished: what rocket_reset() writes
 * before it waits. TRUE if the job was still in flight (it timed out); FALSE
 * if it finished after all, in which case its DPC has run when this returns.
 * PASSIVE_LEVEL.
 */
static BOOLEAN
NpuJobStop(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    CORE_STATE *core = &Ctx->Core[N];
    CORE_JOB *j = &core->Job;
    ULONG base = NpuCoreBase(N);
    BOOLEAN stopped = FALSE;

    WdfInterruptAcquireLock(core->Interrupt);
    if (j->InFlight) {
        j->RawStatus = NpuRd(Ctx, base + PC_INTERRUPT_RAW_STATUS);
        j->TaskStatus = NpuRd(Ctx, base + PC_TASK_STATUS);
        NpuWr(Ctx, base + PC_INTERRUPT_MASK, 0);
        NpuWr(Ctx, base + PC_OPERATION_ENABLE, 0);
        NpuWr(Ctx, base + PC_INTERRUPT_CLEAR, PC_INTERRUPT_ALL);
        j->InFlight = FALSE;
        j->CompletePending = FALSE;
        j->Result = RKNPU_JOB_TIMEOUT;
        j->T1 = KeQueryPerformanceCounter(NULL);
        stopped = TRUE;
    }
    WdfInterruptReleaseLock(core->Interrupt);
    /* a handler that already ran has its DPC queued; let it finish */
    KeFlushQueuedDpcs();
    return stopped;
}

/* _DSM (Npu.asl): Arg0 the UUID, Arg1 revision 0, Arg2 function 1, Arg3 Package () { core }. */
static NTSTATUS
NpuDsmReset(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N,
    _Out_ ULONG *Ret
    )
{
    ULONG inbuf[64];
    ULONG outbuf[16];
    PACPI_EVAL_INPUT_BUFFER_COMPLEX in = (PACPI_EVAL_INPUT_BUFFER_COMPLEX)inbuf;
    PACPI_EVAL_OUTPUT_BUFFER out = (PACPI_EVAL_OUTPUT_BUFFER)outbuf;
    PACPI_METHOD_ARGUMENT a, inner;
    WDF_MEMORY_DESCRIPTOR ind, outd;
    WDF_REQUEST_SEND_OPTIONS opt;
    ULONG_PTR got = 0;
    NTSTATUS status;

    *Ret = 0xFFFFFFFF;
    RtlZeroMemory(inbuf, sizeof(inbuf));
    RtlZeroMemory(outbuf, sizeof(outbuf));
    in->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    RtlCopyMemory(in->MethodName, "_DSM", 4);
    in->ArgumentCount = 4;
    a = in->Argument;
    ACPI_METHOD_SET_ARGUMENT_BUFFER(a, &NpuDsmGuid, (USHORT)sizeof(GUID));
    a = ACPI_METHOD_NEXT_ARGUMENT(a);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(a, NPU_DSM_REVISION);
    a = ACPI_METHOD_NEXT_ARGUMENT(a);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(a, NPU_DSM_FN_RESET_CORE);
    a = ACPI_METHOD_NEXT_ARGUMENT(a);
    a->Type = ACPI_METHOD_ARGUMENT_PACKAGE_EX;
    a->DataLength = (USHORT)ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG));
    inner = (PACPI_METHOD_ARGUMENT)a->Data;
    ACPI_METHOD_SET_ARGUMENT_INTEGER(inner, N);
    a = ACPI_METHOD_NEXT_ARGUMENT(a);
    in->Size = (ULONG)((PUCHAR)a - (PUCHAR)in->Argument);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&ind, inbuf, (ULONG)((PUCHAR)a - (PUCHAR)inbuf));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outd, outbuf, sizeof(outbuf));
    WDF_REQUEST_SEND_OPTIONS_INIT(&opt, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opt, WDF_REL_TIMEOUT_IN_SEC(2));
    status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Ctx->Device), NULL,
                                               IOCTL_ACPI_EVAL_METHOD, &ind, &outd, &opt, &got);
    if (!NT_SUCCESS(status))
        return status;
    if (got < sizeof(ACPI_EVAL_OUTPUT_BUFFER) ||
        out->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || out->Count < 1 ||
        out->Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER)
        return STATUS_ACPI_INVALID_DATA;
    *Ret = out->Argument[0].Argument;
    return STATUS_SUCCESS;
}

/*
 * The core soft reset, then the PC interrupt state the D0 entry leaves.
 * The core's job must already be stopped. PASSIVE_LEVEL.
 */
static NTSTATUS
NpuResetCore(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N,
    _In_ ULONGLONG Seq,
    _Out_ ULONG *DsmRet,
    _Out_ ULONG *VersionAfter
    )
{
    CORE_STATE *core = &Ctx->Core[N];
    ULONG base = NpuCoreBase(N);
    NTSTATUS status;

    *VersionAfter = 0;
    /* M6: rocket detaches the domain before the reset; the next SUBMIT turns
     * paging on again, whatever the reset did to the IOMMU */
    if (Ctx->Mmu.On && NpuSafeToRead(Ctx)) {
        WdfWaitLockAcquire(Ctx->BoLock, NULL);
        NpuMmuDisableLocked(Ctx, N);
        WdfWaitLockRelease(Ctx->BoLock);
    }
    status = NpuDsmReset(Ctx, N, DsmRet);
    InterlockedIncrement(&core->Resets);
    if (NpuSafeToRead(Ctx)) {
        WdfInterruptAcquireLock(core->Interrupt);
        NpuWr(Ctx, base + PC_INTERRUPT_MASK, 0);
        NpuWr(Ctx, base + PC_INTERRUPT_CLEAR, PC_INTERRUPT_ALL);
        WdfInterruptReleaseLock(core->Interrupt);
        *VersionAfter = NpuRd(Ctx, base + PC_VERSION);
    }
    RKNPU_LOG("core %u reset: _DSM 0x%08x returned %u, version after 0x%08x (D0 entry 0x%08x)",
              N, status, *DsmRet, *VersionAfter, core->Version);
    NpuCrumb(Ctx, RKNPU_CRUMB_RESET, Seq, N, (ULONG)status, *VersionAfter);
    if (NT_SUCCESS(status) && *DsmRet != 0)
        status = STATUS_INVALID_PARAMETER;
    return status;
}

/* Wait for a job, stopping and resetting it on timeout. JobLock held. */
static VOID
NpuJobFinish(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N,
    _In_ ULONG TimeoutMs,
    _In_ BOOLEAN Crumbs,
    _Out_opt_ rknpu_wait_out *O
    )
{
    CORE_STATE *core = &Ctx->Core[N];
    CORE_JOB *j = &core->Job;
    LARGE_INTEGER timeout, t0, t1;
    NTSTATUS wait = STATUS_SUCCESS, rs = STATUS_SUCCESS;
    ULONG ret = 0, ver = 0, i;
    BOOLEAN timedOut = FALSE;

    t0 = KeQueryPerformanceCounter(NULL);
    if (JobActive(j)) {
        timeout.QuadPart = -10000LL * TimeoutMs;    /* 0: just look (M6 WAIT has waited) */
        wait = KeWaitForSingleObject(&core->Done, Executive, KernelMode, FALSE, &timeout);
        if (wait == STATUS_TIMEOUT)
            timedOut = NpuJobStop(Ctx, N);
    }
    t1 = KeQueryPerformanceCounter(NULL);

    /* M6: an IOMMU fault stopped it (NpuM3JobFaultLocked); reset the core once */
    if (!timedOut && j->Result == RKNPU_JOB_MMU_FAULT && !j->FaultReset) {
        j->FaultReset = TRUE;
        RKNPU_LOG("core %u job %llu stopped by an IOMMU fault at 0x%08x (status 0x%08x)",
                  N, j->Seq, j->FaultAddr, j->FaultStatus);
        NpuCrumb(Ctx, RKNPU_CRUMB_MMU_FAULT, j->Seq, N, j->FaultAddr, j->FaultStatus);
        rs = NpuResetCore(Ctx, N, j->Seq, &ret, &ver);
    }

    if (timedOut) {
        InterlockedIncrement(&core->JobsTimedOut);
        RKNPU_LOG("core %u job %llu timed out after %u ms: raw 0x%08x task_status 0x%08x, %u of %u tasks",
                  N, j->Seq, TimeoutMs, j->RawStatus, j->TaskStatus, j->TasksDone, j->TaskCount);
        NpuCrumb(Ctx, RKNPU_CRUMB_TIMEOUT, j->Seq, N, j->RawStatus, j->TaskStatus);
        rs = NpuResetCore(Ctx, N, j->Seq, &ret, &ver);
    } else if (Crumbs) {
        NpuCrumb(Ctx, RKNPU_CRUMB_IRQ_SEEN, j->Seq, N, j->IsrCount, j->TasksDone);
    }

    if (O) {
        RtlZeroMemory(O, sizeof(*O));
        O->seqno = j->Seq;
        O->result = j->Result;
        O->core = N;
        O->raw_status = j->RawStatus;
        O->task_status = j->TaskStatus;
        O->tasks_done = j->TasksDone;
        O->task_count = j->TaskCount;
        O->isr_count = j->IsrCount;
        O->hw_us = QpcToUs(Ctx, j->T1.QuadPart - j->T0.QuadPart);
        O->wait_us = QpcToUs(Ctx, t1.QuadPart - t0.QuadPart);
        O->reset_status = (ULONG)rs;
        O->fault_addr = j->FaultAddr;
        O->fault_status = j->FaultStatus;
        if (NpuSafeToRead(Ctx)) {
            O->version_after = (timedOut || ver) ? ver : NpuRd(Ctx, NpuCoreBase(N) + PC_VERSION);
            for (i = 0; i < RKNPU_MMUS_PER_CORE; i++)
                O->mmu_status[i] = NpuRd(Ctx, NpuCoreBase(N) + MmuInst[i] + RK_MMU_STATUS);
        }
    }
    if (Crumbs || timedOut)
        NpuCrumb(Ctx, RKNPU_CRUMB_STATUS, j->Seq, N, j->RawStatus, j->TaskStatus);
}

/* ----------------------------------------------------------- buffers */

static PNPU_BO
BoFindLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Handle,
    _In_ WDFFILEOBJECT Owner
    )
{
    PLIST_ENTRY head = &Ctx->BoHash[Handle % RKNPU_BO_HASH], e;

    for (e = head->Flink; e != head; e = e->Flink) {
        PNPU_BO bo = CONTAINING_RECORD(e, NPU_BO, HashLink);
        if (bo->Handle == Handle && bo->Owner == Owner)
            return bo;
    }
    return NULL;
}

/* The buffer of Owner's that holds [Addr, Addr + Len), or NULL. BoLock held. */
static PNPU_BO
BoFindRangeLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFFILEOBJECT Owner,
    _In_ ULONG Addr,
    _In_ ULONG Len
    )
{
    PLIST_ENTRY e;

    /* M6: the chunk table names the one buffer that can hold Addr */
    if (Ctx->Mmu.On) {
        PNPU_BO bo = NpuMmuOwnerLocked(Ctx, Addr);

        if (bo && bo->Owner == Owner && Addr >= bo->Dma && (ULONGLONG)Addr + Len <= (ULONGLONG)bo->Dma + bo->Size)
            return bo;
        return NULL;
    }
    for (e = Ctx->Bos.Flink; e != &Ctx->Bos; e = e->Flink) {
        PNPU_BO bo = CONTAINING_RECORD(e, NPU_BO, Link);
        ULONGLONG lo = bo->Dma, hi = lo + bo->Size;

        if (bo->Owner == Owner && Addr >= lo && (ULONGLONG)Addr + Len <= hi)
            return bo;
    }
    return NULL;
}

/* rknpu_rc_check_stream()'s in_bo: one of Owner's buffers, the last hit first
 * (a stream's five addresses are in three or four buffers). BoLock held. */
typedef struct _BO_LOOKUP {
    PDEVICE_CONTEXT Ctx;
    WDFFILEOBJECT   Owner;
    PNPU_BO         Last;
} BO_LOOKUP;

static int
BoLookupInBo(
    _In_ void *C,
    _In_ unsigned int Addr
    )
{
    BO_LOOKUP *l = (BO_LOOKUP *)C;
    PNPU_BO bo = l->Last;

    if (bo && Addr >= bo->Dma && (ULONGLONG)Addr < (ULONGLONG)bo->Dma + bo->Size)
        return 1;
    bo = BoFindRangeLocked(l->Ctx, l->Owner, Addr, 1);
    if (bo)
        l->Last = bo;
    return bo != NULL;
}

/*
 * rknpu_rc_check_stream() over the Count entries at Ent, with the log line
 * and the breadcrumb if it refuses. StreamAddr and Core are for those.
 * BoLock held, PASSIVE_LEVEL.
 */
static NTSTATUS
NpuCheckEntriesLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ BO_LOOKUP *L,
    _In_ const ULONGLONG *Ent,
    _In_ ULONG Count,
    _In_ ULONG StreamAddr,
    _In_ ULONG Core
    )
{
    unsigned int bad = 0, unlisted = 0;
    int why;

    why = rknpu_rc_check_stream_ex((const unsigned long long *)Ent, Count,
                                   Ctx->AllowUnlisted ? RKNPU_RC_ALLOW_UNLISTED : 0,
                                   BoLookupInBo, L, &bad, &unlisted);
    if (unlisted && InterlockedIncrement(&Ctx->UnlistedLogged) <= 16)
        RKNPU_LOG("SUBMIT: stream 0x%08x has %u entries off the list, let through (M4AllowUnlisted)",
                  StreamAddr, unlisted);
    if (why == RKNPU_RC_OK)
        return STATUS_SUCCESS;
    RKNPU_LOG("SUBMIT: stream 0x%08x entry %u (0x%016llx) refused: %s", StreamAddr, bad, Ent[bad],
              rknpu_rc_why(why));
    NpuCrumb(Ctx, RKNPU_CRUMB_REFUSED, Ctx->NextSeq + 1, Core, bad,
             RKNPU_RC_TARGET(Ent[bad]) << 16 | RKNPU_RC_REG(Ent[bad]));
    return STATUS_ACCESS_VIOLATION;
}

/*
 * M3: the stream rules over every entry the PC will fetch, in place (it
 * fetches pairs, so an odd count reads one entry more). The stream's buffer
 * must not be mapped into a process: the caller could otherwise rewrite it
 * after this check. BoLock held, PASSIVE_LEVEL.
 */
static NTSTATUS
NpuCheckStreamLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFFILEOBJECT Owner,
    _In_ const rknpu_task *T,
    _In_ ULONG Core
    )
{
    ULONG fetched = ((T->regcmd_count + 1) / 2) * 2;
    PNPU_BO bo = BoFindRangeLocked(Ctx, Owner, T->regcmd, fetched * 8);
    BO_LOOKUP l;

    if (!bo)
        return STATUS_INVALID_PARAMETER;
    if (bo->UserVa)
        return STATUS_INVALID_DEVICE_STATE;
    l.Ctx = Ctx;
    l.Owner = Owner;
    l.Last = NULL;
    /* mapped nowhere but the kernel, so it cannot change under the check */
    return NpuCheckEntriesLocked(Ctx, &l,
                                 (const ULONGLONG *)((PUCHAR)bo->Kva + (T->regcmd - bo->Dma)),
                                 fetched, T->regcmd, Core);
}

/* M4: the core's copy arena, allocated on first use. JobLock held, PASSIVE_LEVEL. */
static NTSTATUS
NpuArenaEnsure(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    CORE_STATE *core = &Ctx->Core[N];
    PHYSICAL_ADDRESS low, high, skip;
    NTSTATUS status;

    if (core->Arena)
        return STATUS_SUCCESS;
    low.QuadPart = 0;
    skip.QuadPart = 0;
    if (Ctx->Mmu.On) {
        /* M6: any pages, write-combined, mapped READ ONLY for the NPU */
        high.QuadPart = 0xFFFFFFFFFFll;
        core->ArenaMdl = MmAllocatePagesForMdlEx(low, high, skip, RKNPU_COPY_ARENA_BYTES, MmWriteCombined,
                                                 MM_ALLOCATE_FULLY_REQUIRED);
        if (!core->ArenaMdl)
            return STATUS_INSUFFICIENT_RESOURCES;
        core->Arena = (PUCHAR)MmMapLockedPagesSpecifyCache(core->ArenaMdl, KernelMode, MmWriteCombined, NULL,
                                                           FALSE, NormalPagePriority | MdlMappingNoExecute);
        status = core->Arena ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
        if (NT_SUCCESS(status)) {
            WdfWaitLockAcquire(Ctx->BoLock, NULL);
            status = NpuMmuMapLocked(Ctx, core->ArenaMdl, RKNPU_COPY_ARENA_BYTES, FALSE, NULL, &core->ArenaDma);
            WdfWaitLockRelease(Ctx->BoLock);
        }
        if (!NT_SUCCESS(status)) {
            if (core->Arena)
                MmUnmapLockedPages(core->Arena, core->ArenaMdl);
            core->Arena = NULL;
            MmFreePagesFromMdl(core->ArenaMdl);
            ExFreePoolWithTag(core->ArenaMdl, 0);
            core->ArenaMdl = NULL;
            RKNPU_LOG("SUBMIT: core %u's copy arena: 0x%08x", N, status);
            return status;
        }
        core->ArenaPa.QuadPart = 0;
    } else {
        high.QuadPart = 0xFFFFFFFF;
        core->Arena = (PUCHAR)MmAllocateContiguousNodeMemory(RKNPU_COPY_ARENA_BYTES, low, high, skip,
                                                             PAGE_READWRITE | PAGE_WRITECOMBINE, MM_ANY_NODE_OK);
        if (!core->Arena) {
            RKNPU_LOG("SUBMIT: no contiguous %u bytes below 4 GiB for core %u's copy arena",
                      RKNPU_COPY_ARENA_BYTES, N);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        core->ArenaPa = MmGetPhysicalAddress(core->Arena);
        if ((ULONGLONG)core->ArenaPa.QuadPart + RKNPU_COPY_ARENA_BYTES > FOURGIB) {
            MmFreeContiguousMemory(core->Arena);
            core->Arena = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        core->ArenaDma = (ULONG)core->ArenaPa.QuadPart;
    }
    RtlZeroMemory(core->Arena, RKNPU_COPY_ARENA_BYTES);
    RKNPU_LOG("core %u copy arena: %u bytes at NPU address 0x%08x", N, RKNPU_COPY_ARENA_BYTES, core->ArenaDma);
    NpuCrumb(Ctx, RKNPU_CRUMB_ARENA, 0, N, RKNPU_COPY_ARENA_BYTES, core->ArenaDma);
    return STATUS_SUCCESS;
}

/* Undo NpuArenaEnsure. No job may be running from it. */
static VOID
NpuArenaFree(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    CORE_STATE *core = &Ctx->Core[N];

    if (!core->Arena)
        return;
    if (core->ArenaMdl) {
        WdfWaitLockAcquire(Ctx->BoLock, NULL);
        NpuMmuUnmapLocked(Ctx, core->ArenaDma, RKNPU_COPY_ARENA_BYTES);
        WdfWaitLockRelease(Ctx->BoLock);
        MmUnmapLockedPages(core->Arena, core->ArenaMdl);
        MmFreePagesFromMdl(core->ArenaMdl);
        ExFreePoolWithTag(core->ArenaMdl, 0);
        core->ArenaMdl = NULL;
    } else {
        MmFreeContiguousMemory(core->Arena);
    }
    core->Arena = NULL;
    core->ArenaDma = 0;
}

/*
 * M4: copy each task's stream (its fetched length) from the caller's buffer
 * into the core's arena, check the copy, and point the task at it. The
 * caller may keep writing its buffer; the NPU runs the copy. In->tasks is
 * rewritten in place. The core has no job (SUBMIT checked), so nothing is
 * reading the arena. JobLock and BoLock held, PASSIVE_LEVEL.
 */
static NTSTATUS
NpuCopyStreamsLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFFILEOBJECT Owner,
    _In_ ULONG N,
    _Inout_ rknpu_submit_in *In
    )
{
    CORE_STATE *core = &Ctx->Core[N];
    BO_LOOKUP l;
    ULONG i, at = 0;
    NTSTATUS status;

    l.Ctx = Ctx;
    l.Owner = Owner;
    l.Last = NULL;
    for (i = 0; i < In->task_count; i++) {
        rknpu_task *t = &In->tasks[i];
        ULONG fetched = ((t->regcmd_count + 1) / 2) * 2, bytes = fetched * 8;
        PNPU_BO bo = BoFindRangeLocked(Ctx, Owner, t->regcmd, bytes);

        if (!bo)
            return STATUS_INVALID_PARAMETER;
        if (bytes > RKNPU_COPY_ARENA_BYTES - at)
            return STATUS_INVALID_BUFFER_SIZE;
        /* at stays a multiple of 16 (fetched is even), as the PC wants */
        RtlCopyMemory(core->Arena + at, (PUCHAR)bo->Kva + (t->regcmd - bo->Dma), bytes);
        status = NpuCheckEntriesLocked(Ctx, &l, (const ULONGLONG *)(core->Arena + at), fetched,
                                       t->regcmd, N);
        if (!NT_SUCCESS(status))
            return status;
        t->regcmd = core->ArenaDma + at;
        at += bytes;
    }
    return STATUS_SUCCESS;
}

/*
 * The mapping is undone in the process that made it, and only there.
 * PASSIVE_LEVEL. FALSE if the caller is another process: that happens only
 * when a duplicated handle outlived the process that mapped the buffer, and
 * then neither choice is safe. Attaching to a process whose address space
 * may already be gone and unmapping in it can bugcheck; freeing pages that
 * are still mapped in it leaves a user mapping of memory Windows will hand
 * to someone else. So the buffer is leaked instead (BoDestroy): its pages
 * stay allocated to this driver, and whatever is left of the mapping goes
 * with the process.
 */
static BOOLEAN
BoUnmap(
    _Inout_ PNPU_BO Bo
    )
{
    if (!Bo->UserVa)
        return TRUE;
    if (Bo->Process != PsGetCurrentProcess())
        return FALSE;
    MmUnmapLockedPages(Bo->UserVa, Bo->Mdl);
    Bo->UserVa = NULL;
    ObDereferenceObject(Bo->Process);
    Bo->Process = NULL;
    return TRUE;
}

/*
 * Take a buffer out of every lookup: the list, the handle hash, the counts
 * and (M6) the NPU address space, so nothing can reach it any more. No job
 * of its owner may be running. BoLock held.
 */
static VOID
BoReleaseLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ PNPU_BO Bo
    )
{
    RemoveEntryList(&Bo->Link);
    RemoveEntryList(&Bo->HashLink);
    InitializeListHead(&Bo->HashLink);
    Ctx->BoCount--;
    Ctx->BoBytes -= Bo->Size;
    if (Bo->IovaMapped) {
        NpuMmuUnmapLocked(Ctx, Bo->Dma, Bo->Size);
        Bo->IovaMapped = FALSE;
    }
}

/* Released already (or never entered). PASSIVE_LEVEL. */
static VOID
BoDestroy(
    _In_ PNPU_BO Bo
    )
{
    if (!BoUnmap(Bo)) {
        RKNPU_LOG("buffer %u (NPU 0x%08x + 0x%Ix) is still mapped in another process; leaking it",
                  Bo->Handle, Bo->Dma, Bo->Size);
        return;
    }
    if (Bo->PagesMdl) {
        /* M6: MmAllocatePagesForMdlEx pages, mapped once into the kernel */
        if (Bo->Kva)
            MmUnmapLockedPages(Bo->Kva, Bo->Mdl);
        MmFreePagesFromMdl(Bo->Mdl);
        ExFreePoolWithTag(Bo->Mdl, 0);
    } else {
        if (Bo->Mdl)
            IoFreeMdl(Bo->Mdl);
        if (Bo->Kva)
            MmFreeContiguousMemory(Bo->Kva);
    }
    ExFreePoolWithTag(Bo, RKNPU_POOL_TAG);
}

static VOID
NpuBoUnreserve(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ SIZE_T Size
    )
{
    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    Ctx->BoBytes -= Size;
    WdfWaitLockRelease(Ctx->BoLock);
}

static NTSTATUS
NpuBoCreate(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _Out_ ULONG_PTR *Info
    )
{
    rknpu_create_bo *io, req;
    PHYSICAL_ADDRESS low, high, skip;
    PNPU_BO bo;
    SIZE_T size;
    NTSTATUS status;
    MEMORY_CACHING_TYPE ct;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*io), (PVOID *)&io, NULL);
    if (!NT_SUCCESS(status))
        return status;
    req = *io;
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*io), (PVOID *)&io, NULL);
    if (!NT_SUCCESS(status))
        return status;
    if (req.flags != 0 || req.size == 0 || req.size > RKNPU_BO_MAX_SIZE)
        return STATUS_INVALID_PARAMETER;
    size = ((SIZE_T)req.size + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);

    /* reserved now, so two callers cannot both pass the check */
    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    if (Ctx->BoBytes + size > Ctx->BoMaxTotal) {
        WdfWaitLockRelease(Ctx->BoLock);
        return STATUS_QUOTA_EXCEEDED;
    }
    Ctx->BoBytes += size;
    WdfWaitLockRelease(Ctx->BoLock);

    bo = (PNPU_BO)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*bo), RKNPU_POOL_TAG);
    if (!bo) {
        NpuBoUnreserve(Ctx, size);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    InitializeListHead(&bo->HashLink);
    bo->Size = size;
    bo->Cached = Ctx->CachedBos;
    ct = bo->Cached ? MmCached : MmWriteCombined;
    low.QuadPart = 0;
    skip.QuadPart = 0;
    if (Ctx->Mmu.On) {
        /* M6: any pages below 2^40 (the v2 entries' reach), zeroed by Mm */
        high.QuadPart = 0xFFFFFFFFFFll;
        bo->Mdl = MmAllocatePagesForMdlEx(low, high, skip, size, ct, MM_ALLOCATE_FULLY_REQUIRED);
        if (!bo->Mdl) {
            ExFreePoolWithTag(bo, RKNPU_POOL_TAG);
            NpuBoUnreserve(Ctx, size);
            RKNPU_LOG("CREATE_BO: no %Iu bytes of pages", size);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        bo->PagesMdl = TRUE;
        bo->Kva = MmMapLockedPagesSpecifyCache(bo->Mdl, KernelMode, ct, NULL, FALSE,
                                               NormalPagePriority | MdlMappingNoExecute);
        if (!bo->Kva) {
            BoDestroy(bo);
            NpuBoUnreserve(Ctx, size);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    } else {
        high.QuadPart = 0xFFFFFFFF;
        bo->Kva = MmAllocateContiguousNodeMemory(size, low, high, skip,
                                                 PAGE_READWRITE | (bo->Cached ? 0 : PAGE_WRITECOMBINE),
                                                 MM_ANY_NODE_OK);
        if (!bo->Kva) {
            ExFreePoolWithTag(bo, RKNPU_POOL_TAG);
            NpuBoUnreserve(Ctx, size);
            RKNPU_LOG("CREATE_BO: no contiguous %Iu bytes below 4 GiB", size);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(bo->Kva, size);
        bo->Pa = MmGetPhysicalAddress(bo->Kva);
        if (bo->Pa.QuadPart == 0 || (ULONGLONG)bo->Pa.QuadPart + size > FOURGIB) {
            /* cannot happen with high = 4 GiB - 1; a stream address is 32 bits */
            RKNPU_LOG("CREATE_BO: got PA 0x%llx + 0x%Ix, not usable", bo->Pa.QuadPart, size);
            BoDestroy(bo);
            NpuBoUnreserve(Ctx, size);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        bo->Dma = (ULONG)bo->Pa.QuadPart;
    }
    /* the zeros, and nothing dirty, in memory before the NPU or another
     * mapping sees the pages (a dirty line evicted later would land on
     * what the NPU wrote) */
    BoCleanInvalidate(bo, 0, size);
    bo->Owner = WdfRequestGetFileObject(Request);

    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    if (Ctx->Mmu.On) {
        status = NpuMmuMapLocked(Ctx, bo->Mdl, size, TRUE, bo, &bo->Dma);
        if (!NT_SUCCESS(status)) {
            Ctx->BoBytes -= size;
            WdfWaitLockRelease(Ctx->BoLock);
            RKNPU_LOG("CREATE_BO: no NPU address space for %Iu bytes (0x%08x)", size, status);
            BoDestroy(bo);
            return status;
        }
        bo->IovaMapped = TRUE;
    }
    do {
        bo->Handle = ++Ctx->NextHandle;
    } while (bo->Handle == 0);
    InsertTailList(&Ctx->Bos, &bo->Link);
    InsertTailList(&Ctx->BoHash[bo->Handle % RKNPU_BO_HASH], &bo->HashLink);
    Ctx->BoCount++;
    WdfWaitLockRelease(Ctx->BoLock);

    NpuCrumb(Ctx, RKNPU_CRUMB_BO_CREATE, 0, 0, (ULONG)size, bo->Dma);
    io->size = (unsigned int)size;
    io->flags = bo->Cached ? RKNPU_BO_CACHED : 0;
    io->handle = bo->Handle;
    io->dma_address = bo->Dma;
    *Info = sizeof(*io);
    return STATUS_SUCCESS;
}

/* Caller context: the mapping goes into the calling process. */
static NTSTATUS
NpuBoMap(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _Out_ ULONG_PTR *Info
    )
{
    rknpu_map_bo *io;
    ULONG handle;
    PNPU_BO bo;
    PVOID va = NULL;
    NTSTATUS status;

    *Info = 0;
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*io), (PVOID *)&io, NULL);
    if (!NT_SUCCESS(status))
        return status;
    handle = io->handle;
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*io), (PVOID *)&io, NULL);
    if (!NT_SUCCESS(status))
        return status;

    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    bo = BoFindLocked(Ctx, handle, WdfRequestGetFileObject(Request));
    if (!bo) {
        status = STATUS_INVALID_HANDLE;
    } else if (bo->UserVa) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else if (OwnerUncopiedJobActive(Ctx, bo->Owner)) {
        /* an M3 job's stream could be rewritten after SUBMIT checked it */
        status = STATUS_DEVICE_BUSY;
    } else {
        if (!bo->Mdl) {
            bo->Mdl = IoAllocateMdl(bo->Kva, (ULONG)bo->Size, FALSE, FALSE, NULL);
            if (bo->Mdl)
                MmBuildMdlForNonPagedPool(bo->Mdl);
        }
        if (bo->Mdl) {
            /* the same attributes as the kernel's mapping of the pages */
            __try {
                va = MmMapLockedPagesSpecifyCache(bo->Mdl, UserMode, bo->Cached ? MmCached : MmWriteCombined,
                                                  NULL, FALSE, NormalPagePriority | MdlMappingNoExecute);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                va = NULL;
            }
        }
        if (va) {
            bo->UserVa = va;
            bo->Process = PsGetCurrentProcess();
            ObReferenceObject(bo->Process);
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    WdfWaitLockRelease(Ctx->BoLock);
    if (!NT_SUCCESS(status))
        return status;

    NpuCrumb(Ctx, RKNPU_CRUMB_BO_MAP, 0, 0, handle, bo->Dma);
    io->handle = handle;
    io->reserved = 0;
    io->user_va = (unsigned long long)(ULONG_PTR)va;
    *Info = sizeof(*io);
    return STATUS_SUCCESS;
}

/* Caller context. */
static NTSTATUS
NpuBoUnmap(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request
    )
{
    rknpu_bo_handle *in;
    PNPU_BO bo;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status))
        return status;
    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    bo = BoFindLocked(Ctx, in->handle, WdfRequestGetFileObject(Request));
    if (!bo)
        status = STATUS_INVALID_HANDLE;
    else if (!bo->UserVa)
        status = STATUS_INVALID_DEVICE_STATE;
    else if (!BoUnmap(bo))
        status = STATUS_ACCESS_DENIED;      /* mapped by another process */
    WdfWaitLockRelease(Ctx->BoLock);
    if (NT_SUCCESS(status))
        NpuCrumb(Ctx, RKNPU_CRUMB_BO_UNMAP, 0, 0, in->handle, 0);
    return status;
}

/* Caller context (it may unmap). Refused while a job of this handle runs: the
 * NPU may be reading it. Another handle's job can only reach its own buffers
 * through the registers SUBMIT checks (M4; M3 refused while any job ran). */
static NTSTATUS
NpuBoFree(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request
    )
{
    rknpu_bo_handle *in;
    PNPU_BO bo;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG handle;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status))
        return status;
    handle = in->handle;

    WdfWaitLockAcquire(Ctx->JobLock, NULL);
    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    bo = BoFindLocked(Ctx, handle, WdfRequestGetFileObject(Request));
    if (!bo) {
        status = STATUS_INVALID_HANDLE;
    } else if (OwnerJobActive(Ctx, bo->Owner)) {
        status = STATUS_DEVICE_BUSY;
        bo = NULL;
    } else {
        BoReleaseLocked(Ctx, bo);
    }
    WdfWaitLockRelease(Ctx->BoLock);
    WdfWaitLockRelease(Ctx->JobLock);
    if (!bo)
        return status;
    BoDestroy(bo);
    NpuCrumb(Ctx, RKNPU_CRUMB_BO_FREE, 0, 0, handle, 0);
    return STATUS_SUCCESS;
}

/* WRITE_BO and READ_BO. The data buffer is the request's direct-I/O buffer. */
static NTSTATUS
NpuBoCopy(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN Write,
    _Out_ ULONG_PTR *Info
    )
{
    rknpu_bo_io *in, req;
    PVOID data;
    size_t dataLen = 0;
    PNPU_BO bo;
    NTSTATUS status;

    *Info = 0;
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status))
        return status;
    req = *in;
    if (req.length == 0 || req.length > RKNPU_BO_IO_MAX)
        return STATUS_INVALID_PARAMETER;
    status = WdfRequestRetrieveOutputBuffer(Request, req.length, &data, &dataLen);
    if (!NT_SUCCESS(status))
        return status;

    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    bo = BoFindLocked(Ctx, req.handle, WdfRequestGetFileObject(Request));
    if (!bo) {
        status = STATUS_INVALID_HANDLE;
    } else if ((ULONGLONG)req.offset + req.length > bo->Size) {
        status = STATUS_INVALID_PARAMETER;
    } else if (Write && OwnerUncopiedJobActive(Ctx, bo->Owner)) {
        status = STATUS_DEVICE_BUSY;
    } else {
        /* M6: a cached buffer is made right for the other side here */
        if (Write) {
            RtlCopyMemory((PUCHAR)bo->Kva + req.offset, data, req.length);
            BoClean(bo, req.offset, req.length);
        } else {
            BoCleanInvalidate(bo, req.offset, req.length);
            RtlCopyMemory(data, (PUCHAR)bo->Kva + req.offset, req.length);
        }
        *Info = Write ? 0 : req.length;
    }
    WdfWaitLockRelease(Ctx->BoLock);
    return status;
}

/*
 * M6: PREP_BO (Prep) and FINI_BO, rocket's DRM_ROCKET_PREP_BO and
 * DRM_ROCKET_FINI_BO without the fence wait (the caller has waited). FINI
 * cleans the range so the NPU reads what the CPU wrote; PREP cleans and
 * invalidates it so the CPU reads what the NPU wrote. A write-combined
 * buffer needs neither. Holding BoLock keeps the buffer from being freed
 * under the maintenance; at the 14 GB/s charsiu measures for it, a typical
 * activation buffer takes a few microseconds.
 */
static NTSTATUS
NpuBoSync(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN Prep
    )
{
    rknpu_bo_sync *in, req;
    PNPU_BO bo;
    SIZE_T len;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status))
        return status;
    req = *in;
    if (req.flags != 0)
        return STATUS_INVALID_PARAMETER;

    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    bo = BoFindLocked(Ctx, req.handle, WdfRequestGetFileObject(Request));
    if (!bo) {
        status = STATUS_INVALID_HANDLE;
    } else if (req.offset > bo->Size || (ULONGLONG)req.offset + req.length > bo->Size) {
        status = STATUS_INVALID_PARAMETER;
    } else {
        len = req.length ? req.length : bo->Size - req.offset;
        if (Prep)
            BoCleanInvalidate(bo, req.offset, len);
        else
            BoClean(bo, req.offset, len);
    }
    WdfWaitLockRelease(Ctx->BoLock);
    return status;
}

/* ------------------------------------------------------ submit, wait */

/*
 * M4: the core's last job is finished and about to be replaced; keep what
 * its WAIT would have said. JobLock held.
 */
static VOID
NpuRecordLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    const CORE_JOB *j = &Ctx->Core[N].Job;
    JOB_RECORD *r;

    if (j->Seq == 0 || j->Owner == NULL)
        return;
    r = &Ctx->History[Ctx->HistoryAt++ % RKNPU_JOB_HISTORY];
    r->Seq = j->Seq;
    r->Owner = j->Owner;
    r->Core = N;
    r->Result = j->Result;
    r->RawStatus = j->RawStatus;
    r->TaskStatus = j->TaskStatus;
    r->TasksDone = j->TasksDone;
    r->TaskCount = j->TaskCount;
    r->IsrCount = j->IsrCount;
    r->HwUs = QpcToUs(Ctx, j->T1.QuadPart - j->T0.QuadPart);
    r->FaultAddr = j->FaultAddr;
    r->FaultStatus = j->FaultStatus;
}

static NTSTATUS
NpuSubmit(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _Out_ ULONG_PTR *Info
    )
{
    rknpu_submit_in *pin;
    rknpu_submit_out *out;
    rknpu_submit_in in;     /* METHOD_BUFFERED: in and out share one buffer */
    WDFFILEOBJECT fo = WdfRequestGetFileObject(Request);
    CORE_STATE *core;
    CORE_JOB *j;
    ULONG n, i, base;
    ULONGLONG seq;
    BOOLEAN copy;
    NTSTATUS status;

    *Info = 0;
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(in), (PVOID *)&pin, NULL);
    if (!NT_SUCCESS(status))
        return status;
    RtlCopyMemory(&in, pin, sizeof(in));
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status))
        return status;
    if (in.core >= RKNPU_NUM_CORES || in.task_count == 0 || in.task_count > RKNPU_MAX_TASKS ||
        (in.flags & ~(RKNPU_SUBMIT_NO_CRUMBS | RKNPU_SUBMIT_COPY)))
        return STATUS_INVALID_PARAMETER;
    copy = (in.flags & RKNPU_SUBMIT_COPY) ? TRUE : FALSE;
    for (i = 0; i < in.task_count; i++)
        if ((in.tasks[i].regcmd & 0xF) || in.tasks[i].regcmd_count == 0 ||
            in.tasks[i].regcmd_count > RKNPU_MAX_REGCMD)
            return STATUS_INVALID_PARAMETER;

    n = in.core;
    core = &Ctx->Core[n];
    j = &core->Job;
    base = NpuCoreBase(n);

    WdfWaitLockAcquire(Ctx->JobLock, NULL);
    if (!Ctx->InterruptsConnected || !NpuSafeToRead(Ctx)) {
        status = STATUS_DEVICE_NOT_READY;
        goto out_job;
    }
    if (JobActive(j) || NpuRd(Ctx, base + PC_OPERATION_ENABLE) != 0) {
        status = STATUS_DEVICE_BUSY;
        goto out_job;
    }
    /* bypass mode: paging must be off, or every address would be translated
     * through tables nobody set up (M6 turns it on in the BoLock section) */
    for (i = 0; !Ctx->Mmu.On && i < RKNPU_MMUS_PER_CORE; i++) {
        if (NpuRd(Ctx, base + MmuInst[i] + RK_MMU_STATUS) & RK_MMU_STATUS_PAGING) {
            RKNPU_LOG("SUBMIT: core %u IOMMU %u has paging on; bypass mode runs with it off", n, i);
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
            goto out_job;
        }
    }

    if (copy) {
        status = NpuArenaEnsure(Ctx, n);
        if (!NT_SUCCESS(status))
            goto out_job;
    }

    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    /* M6: paging on with the driver's table; after a reset this turns it on again */
    if (Ctx->Mmu.On) {
        status = NpuMmuEnsureLocked(Ctx, n);
        if (!NT_SUCCESS(status)) {
            RKNPU_LOG("SUBMIT: core %u IOMMU could not be turned on: 0x%08x", n, status);
            NpuCrumb(Ctx, RKNPU_CRUMB_IOMMU, 0, n, (ULONG)status,
                     NpuRd(Ctx, base + NPU_MMU0_OFF + RK_MMU_STATUS) << 16 |
                     (NpuRd(Ctx, base + NPU_MMU1_OFF + RK_MMU_STATUS) & 0xFFFF));
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
            goto out_bo;
        }
    }
    /* Every stream inside one of the caller's buffers, and within the rules:
     * M4 checks the copies it will run, M3 the streams in place. */
    if (copy) {
        status = NpuCopyStreamsLocked(Ctx, fo, n, &in);
        if (!NT_SUCCESS(status))
            goto out_bo;
    } else {
        for (i = 0; i < in.task_count; i++) {
            status = NpuCheckStreamLocked(Ctx, fo, &in.tasks[i], n);
            if (!NT_SUCCESS(status))
                goto out_bo;
        }
    }

    NpuRecordLocked(Ctx, n);
    seq = ++Ctx->NextSeq;
    RtlZeroMemory(j, sizeof(*j));
    j->Seq = seq;
    j->Owner = fo;
    j->TaskCount = in.task_count;
    j->NoCrumbs = (in.flags & RKNPU_SUBMIT_NO_CRUMBS) ? TRUE : FALSE;
    j->Copied = copy;
    RtlCopyMemory(j->Tasks, in.tasks, in.task_count * sizeof(rknpu_task));
    KeClearEvent(&core->Done);
    InterlockedIncrement(&core->JobsSubmitted);

    /* On disk before the hardware starts: if this hangs the board, it says so. */
    if (!(in.flags & RKNPU_SUBMIT_NO_CRUMBS))
        NpuCrumb(Ctx, RKNPU_CRUMB_SUBMIT, seq, n, in.tasks[0].regcmd, in.tasks[0].regcmd_count);

    /* Everything the CPU wrote to the buffers (write-combined) is in memory
     * before OPERATION_ENABLE: rocket's writel() has the same barrier. */
    KeMemoryBarrier();
#if defined(_M_ARM64)
    __dsb(_ARM64_BARRIER_SY);
#endif

    WdfInterruptAcquireLock(core->Interrupt);
    j->InFlight = TRUE;
    j->T0 = KeQueryPerformanceCounter(NULL);
    NpuHwSubmit(Ctx, n);
    WdfInterruptReleaseLock(core->Interrupt);

    RtlZeroMemory(out, sizeof(*out));
    out->seqno = seq;
    out->core = n;
    *Info = sizeof(*out);
    status = STATUS_SUCCESS;
out_bo:
    WdfWaitLockRelease(Ctx->BoLock);
out_job:
    WdfWaitLockRelease(Ctx->JobLock);
    return status;
}

static NTSTATUS
NpuWait(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _Out_ ULONG_PTR *Info
    )
{
    rknpu_wait_in *pin, in;
    rknpu_wait_out *out;
    WDFFILEOBJECT fo = WdfRequestGetFileObject(Request);
    ULONG n, ms;
    NTSTATUS status;

    *Info = 0;
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(in), (PVOID *)&pin, NULL);
    if (!NT_SUCCESS(status))
        return status;
    in = *pin;
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status))
        return status;
    if (in.flags & ~RKNPU_WAIT_POLL)
        return STATUS_INVALID_PARAMETER;
    ms = in.timeout_ms ? in.timeout_ms : RKNPU_WAIT_DEFAULT_MS;
    if (ms > RKNPU_WAIT_MAX_MS)
        ms = RKNPU_WAIT_MAX_MS;

    /*
     * M6: wait with no lock held, so the other core's SUBMIT and WAIT go on
     * meanwhile. The job stays the core's job until it finishes (SUBMIT
     * refuses a busy core), so while it is found active, Done belongs to it.
     * If another handle's job took the core between the completion and the
     * wait, the loop finds the sequence number gone and stops. Then JobLock
     * collects the result as before, and a job still running after the
     * whole timeout is stopped there (a zero-length wait).
     */
    if (!(in.flags & RKNPU_WAIT_POLL) && in.seqno != 0) {
        LARGE_INTEGER t0 = KeQueryPerformanceCounter(NULL), now, to;
        LONGLONG left;

        for (;;) {
            BOOLEAN active = FALSE;

            WdfWaitLockAcquire(Ctx->JobLock, NULL);
            for (n = 0; n < RKNPU_NUM_CORES; n++)
                if (Ctx->Core[n].Job.Seq == in.seqno && Ctx->Core[n].Job.Owner == fo)
                    break;
            if (n < RKNPU_NUM_CORES)
                active = JobActive(&Ctx->Core[n].Job);
            WdfWaitLockRelease(Ctx->JobLock);
            if (!active)
                break;
            now = KeQueryPerformanceCounter(NULL);
            left = (LONGLONG)ms * 1000 - (LONGLONG)QpcToUs(Ctx, now.QuadPart - t0.QuadPart);
            if (left <= 0)
                break;
            to.QuadPart = -10 * left;
            if (KeWaitForSingleObject(&Ctx->Core[n].Done, Executive, KernelMode, FALSE, &to) == STATUS_TIMEOUT)
                break;
        }
        now = KeQueryPerformanceCounter(NULL);
        left = (LONGLONG)ms * 1000 - (LONGLONG)QpcToUs(Ctx, now.QuadPart - t0.QuadPart);
        ms = left > 0 ? (ULONG)((left + 999) / 1000) : 0;
    }

    WdfWaitLockAcquire(Ctx->JobLock, NULL);
    for (n = 0; n < RKNPU_NUM_CORES; n++)
        if (in.seqno != 0 && Ctx->Core[n].Job.Seq == in.seqno && Ctx->Core[n].Job.Owner == fo)
            break;
    if (n == RKNPU_NUM_CORES) {
        ULONG h;

        RtlZeroMemory(out, sizeof(*out));
        out->seqno = in.seqno;
        out->result = RKNPU_JOB_UNKNOWN;
        /* M4: finished, and its core has run another handle's job since */
        for (h = 0; in.seqno != 0 && h < RKNPU_JOB_HISTORY; h++) {
            const JOB_RECORD *r = &Ctx->History[h];

            if (r->Seq == in.seqno && r->Owner == fo) {
                out->result = r->Result;
                out->core = r->Core;
                out->raw_status = r->RawStatus;
                out->task_status = r->TaskStatus;
                out->tasks_done = r->TasksDone;
                out->task_count = r->TaskCount;
                out->isr_count = r->IsrCount;
                out->hw_us = r->HwUs;
                out->fault_addr = r->FaultAddr;
                out->fault_status = r->FaultStatus;
                break;
            }
        }
    } else if ((in.flags & RKNPU_WAIT_POLL) && JobActive(&Ctx->Core[n].Job)) {
        /* M4: still running; stop nothing */
        RtlZeroMemory(out, sizeof(*out));
        out->seqno = in.seqno;
        out->core = n;
        out->result = RKNPU_JOB_RUNNING;
        out->task_count = Ctx->Core[n].Job.TaskCount;
        out->tasks_done = Ctx->Core[n].Job.TasksDone;
    } else {
        /* crumbs follow the submit's choice; a timeout always leaves them */
        NpuJobFinish(Ctx, n, ms, !Ctx->Core[n].Job.NoCrumbs, out);
    }
    WdfWaitLockRelease(Ctx->JobLock);
    *Info = sizeof(*out);
    return STATUS_SUCCESS;
}

static NTSTATUS
NpuReset(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _Out_ ULONG_PTR *Info
    )
{
    rknpu_reset_in *pin;
    rknpu_reset_out *out;
    ULONG n, i, ret = 0, ver = 0;
    NTSTATUS status, rs;

    *Info = 0;
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*pin), (PVOID *)&pin, NULL);
    if (!NT_SUCCESS(status))
        return status;
    n = pin->core;
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status))
        return status;
    if (n >= RKNPU_NUM_CORES)
        return STATUS_INVALID_PARAMETER;

    WdfWaitLockAcquire(Ctx->JobLock, NULL);
    if (!NpuSafeToRead(Ctx)) {
        status = STATUS_DEVICE_NOT_READY;
    } else if (JobActive(&Ctx->Core[n].Job)) {
        status = STATUS_DEVICE_BUSY;
    } else {
        rs = NpuResetCore(Ctx, n, 0, &ret, &ver);
        RtlZeroMemory(out, sizeof(*out));
        out->dsm_status = (unsigned int)rs;
        out->dsm_return = ret;
        out->version_after = ver;
        for (i = 0; i < RKNPU_MMUS_PER_CORE; i++)
            out->mmu_status[i] = NpuRd(Ctx, NpuCoreBase(n) + MmuInst[i] + RK_MMU_STATUS);
        *Info = sizeof(*out);
        status = STATUS_SUCCESS;
    }
    WdfWaitLockRelease(Ctx->JobLock);
    return status;
}

/* ---------------------------------------------------------- dispatch */

BOOLEAN
NpuM3InCallerContext(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode
    )
{
    ULONG_PTR info = 0;
    NTSTATUS status;

    switch (IoControlCode) {
    case IOCTL_RKNPU_MAP_BO:
        status = NpuBoMap(Ctx, Request, &info);
        break;
    case IOCTL_RKNPU_UNMAP_BO:
        status = NpuBoUnmap(Ctx, Request);
        break;
    case IOCTL_RKNPU_FREE_BO:
        status = NpuBoFree(Ctx, Request);
        break;
    default:
        return FALSE;
    }
    WdfRequestCompleteWithInformation(Request, status, info);
    return TRUE;
}

BOOLEAN
NpuM3DeviceControl(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode,
    _Out_ NTSTATUS *Status,
    _Out_ ULONG_PTR *Info
    )
{
    *Info = 0;
    switch (IoControlCode) {
    case IOCTL_RKNPU_CREATE_BO:
        *Status = NpuBoCreate(Ctx, Request, Info);
        return TRUE;
    case IOCTL_RKNPU_WRITE_BO:
        *Status = NpuBoCopy(Ctx, Request, TRUE, Info);
        return TRUE;
    case IOCTL_RKNPU_READ_BO:
        *Status = NpuBoCopy(Ctx, Request, FALSE, Info);
        return TRUE;
    case IOCTL_RKNPU_SUBMIT:
        *Status = NpuSubmit(Ctx, Request, Info);
        return TRUE;
    case IOCTL_RKNPU_WAIT:
        *Status = NpuWait(Ctx, Request, Info);
        return TRUE;
    case IOCTL_RKNPU_RESET:
        *Status = NpuReset(Ctx, Request, Info);
        return TRUE;
    case IOCTL_RKNPU_PREP_BO:
        *Status = NpuBoSync(Ctx, Request, TRUE);
        return TRUE;
    case IOCTL_RKNPU_FINI_BO:
        *Status = NpuBoSync(Ctx, Request, FALSE);
        return TRUE;
    default:
        *Status = STATUS_INVALID_DEVICE_REQUEST;
        return FALSE;
    }
}

/* -------------------------------------------------------- teardown */

/*
 * The handle is closing, in the process that held it. A job of this handle
 * still running is waited for (rocket's timeout), stopped and reset if it
 * does not finish, and only then are the buffers it may be writing freed.
 */
VOID
NpuM3FileCleanup(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFFILEOBJECT FileObject
    )
{
    LIST_ENTRY mine;
    PLIST_ENTRY e, next;
    ULONG n;

    InitializeListHead(&mine);
    WdfWaitLockAcquire(Ctx->JobLock, NULL);
    for (n = 0; n < RKNPU_NUM_CORES; n++)
        if (Ctx->Core[n].Job.Owner == FileObject && JobActive(&Ctx->Core[n].Job))
            NpuJobFinish(Ctx, n, RKNPU_WAIT_DEFAULT_MS, TRUE, NULL);
    for (n = 0; n < RKNPU_NUM_CORES; n++)
        if (Ctx->Core[n].Job.Owner == FileObject)
            Ctx->Core[n].Job.Owner = NULL;
    /* a later handle may get the same file object address */
    for (n = 0; n < RKNPU_JOB_HISTORY; n++)
        if (Ctx->History[n].Owner == FileObject)
            RtlZeroMemory(&Ctx->History[n], sizeof(Ctx->History[n]));

    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    for (e = Ctx->Bos.Flink; e != &Ctx->Bos; e = next) {
        PNPU_BO bo = CONTAINING_RECORD(e, NPU_BO, Link);
        next = e->Flink;
        if (bo->Owner == FileObject) {
            BoReleaseLocked(Ctx, bo);
            InsertTailList(&mine, &bo->Link);
        }
    }
    WdfWaitLockRelease(Ctx->BoLock);
    WdfWaitLockRelease(Ctx->JobLock);
    while (!IsListEmpty(&mine))
        BoDestroy(CONTAINING_RECORD(RemoveHeadList(&mine), NPU_BO, Link));
}

/*
 * Leaving D0 (disable, removal): no wait and no _DSM here, only the writes
 * rocket's completion makes, so nothing keeps running into the next start.
 * The buffers stay until their handle closes.
 */
VOID
NpuM3D0Exit(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    ULONG n;

    for (n = 0; n < RKNPU_NUM_CORES; n++) {
        CORE_STATE *core = &Ctx->Core[n];
        BOOLEAN wake = FALSE;

        WdfInterruptAcquireLock(core->Interrupt);
        if (core->Job.InFlight) {
            ULONG base = NpuCoreBase(n);
            NpuWr(Ctx, base + PC_INTERRUPT_MASK, 0);
            NpuWr(Ctx, base + PC_OPERATION_ENABLE, 0);
            NpuWr(Ctx, base + PC_INTERRUPT_CLEAR, PC_INTERRUPT_ALL);
            core->Job.InFlight = FALSE;
            core->Job.Result = RKNPU_JOB_TIMEOUT;
            wake = TRUE;
        }
        WdfInterruptReleaseLock(core->Interrupt);
        if (wake)
            KeSetEvent(&core->Done, IO_NO_INCREMENT, FALSE);
    }

    /* M6: leave the IOMMUs as the firmware left them, paging off. The
     * buffers keep their IOVAs; the next SUBMIT turns paging on again. */
    if (Ctx->Mmu.On && Ctx->BoLock) {
        WdfWaitLockAcquire(Ctx->BoLock, NULL);
        for (n = 0; n < RKNPU_NUM_CORES; n++)
            NpuMmuDisableLocked(Ctx, n);
        WdfWaitLockRelease(Ctx->BoLock);
    }
}

/* Device cleanup: every handle has closed, so nothing is mapped or running. */
VOID
NpuM3FreeAll(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    LIST_ENTRY mine;
    ULONG n;

    if (!Ctx->BoLock)
        return;
    InitializeListHead(&mine);
    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    while (!IsListEmpty(&Ctx->Bos)) {
        PNPU_BO bo = CONTAINING_RECORD(Ctx->Bos.Flink, NPU_BO, Link);

        BoReleaseLocked(Ctx, bo);
        InsertTailList(&mine, &bo->Link);
    }
    Ctx->BoCount = 0;
    Ctx->BoBytes = 0;
    WdfWaitLockRelease(Ctx->BoLock);
    while (!IsListEmpty(&mine))
        BoDestroy(CONTAINING_RECORD(RemoveHeadList(&mine), NPU_BO, Link));
    /* M4: every handle has closed, so no job is running from an arena */
    for (n = 0; n < RKNPU_NUM_CORES; n++)
        NpuArenaFree(Ctx, n);
}
