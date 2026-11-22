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
 * LOCKING. Every wait (job completion, the _DSM call, a breadcrumb flush)
 * happens at PASSIVE_LEVEL holding at most the two wait locks. The interrupt
 * spin lock is held only around register writes and job bookkeeping, never
 * across a wait; the DPC takes nothing else.
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
    status = WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ | KEY_WRITE,
                                      WDF_NO_OBJECT_ATTRIBUTES, &Ctx->CrumbKey);
    if (!NT_SUCCESS(status)) {
        RKNPU_LOG("breadcrumbs: cannot open Device Parameters: 0x%08x", status);
        Ctx->CrumbKey = NULL;
    }
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

static BOOLEAN
AnyJobActive(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    ULONG n;

    for (n = 0; n < RKNPU_NUM_CORES; n++)
        if (JobActive(&Ctx->Core[n].Job))
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
        timeout.QuadPart = -10000LL * TimeoutMs;
        wait = KeWaitForSingleObject(&core->Done, Executive, KernelMode, FALSE, &timeout);
        if (wait == STATUS_TIMEOUT)
            timedOut = NpuJobStop(Ctx, N);
    }
    t1 = KeQueryPerformanceCounter(NULL);

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
        if (NpuSafeToRead(Ctx)) {
            O->version_after = timedOut ? ver : NpuRd(Ctx, NpuCoreBase(N) + PC_VERSION);
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
    PLIST_ENTRY e;

    for (e = Ctx->Bos.Flink; e != &Ctx->Bos; e = e->Flink) {
        PNPU_BO bo = CONTAINING_RECORD(e, NPU_BO, Link);
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

    for (e = Ctx->Bos.Flink; e != &Ctx->Bos; e = e->Flink) {
        PNPU_BO bo = CONTAINING_RECORD(e, NPU_BO, Link);
        ULONGLONG lo = (ULONGLONG)bo->Pa.QuadPart, hi = lo + bo->Size;

        if (bo->Owner == Owner && Addr >= lo && (ULONGLONG)Addr + Len <= hi)
            return bo;
    }
    return NULL;
}

/*
 * The stream rules of rknpu_regcmd.h, over every entry the PC will fetch
 * (it fetches pairs, so an odd count reads one entry more). An all-zero
 * entry is padding and allowed. The stream's buffer must not be mapped into
 * a process: the caller could otherwise rewrite it after this check.
 * BoLock held, PASSIVE_LEVEL. *Bad is the first refused entry.
 */
static NTSTATUS
NpuCheckStreamLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFFILEOBJECT Owner,
    _In_ const rknpu_task *T,
    _Out_ ULONG *Bad
    )
{
    ULONG fetched = ((T->regcmd_count + 1) / 2) * 2;
    PNPU_BO bo = BoFindRangeLocked(Ctx, Owner, T->regcmd, fetched * 8);
    const volatile ULONGLONG *ent;
    ULONG i;

    *Bad = 0;
    if (!bo)
        return STATUS_INVALID_PARAMETER;
    if (bo->UserVa)
        return STATUS_INVALID_DEVICE_STATE;
    ent = (const volatile ULONGLONG *)((PUCHAR)bo->Kva + (T->regcmd - (ULONG)bo->Pa.QuadPart));
    for (i = 0; i < fetched; i++) {
        ULONGLONG e = ent[i];
        ULONG reg = RKNPU_RC_REG(e), v = RKNPU_RC_VALUE(e);

        if (e == 0)
            continue;
        if (rknpu_rc_is_pc_reg(reg) ||
            (rknpu_rc_is_addr_reg(reg) && v != 0 && !BoFindRangeLocked(Ctx, Owner, v, 1))) {
            *Bad = i;
            RKNPU_LOG("SUBMIT: stream 0x%08x entry %u (0x%016llx) refused: %s", T->regcmd, i, e,
                      rknpu_rc_is_pc_reg(reg) ? "writes the PC block" : "address outside the caller's buffers");
            return STATUS_ACCESS_VIOLATION;
        }
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

/* Already off the list. PASSIVE_LEVEL. */
static VOID
BoDestroy(
    _In_ PNPU_BO Bo
    )
{
    if (!BoUnmap(Bo)) {
        RKNPU_LOG("buffer %u (0x%llx + 0x%Ix) is still mapped in another process; leaking it",
                  Bo->Handle, Bo->Pa.QuadPart, Bo->Size);
        return;
    }
    if (Bo->Mdl)
        IoFreeMdl(Bo->Mdl);
    if (Bo->Kva)
        MmFreeContiguousMemory(Bo->Kva);
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
    if (Ctx->BoBytes + size > RKNPU_BO_MAX_TOTAL) {
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
    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFF;
    skip.QuadPart = 0;
    bo->Kva = MmAllocateContiguousNodeMemory(size, low, high, skip,
                                             PAGE_READWRITE | PAGE_WRITECOMBINE, MM_ANY_NODE_OK);
    if (!bo->Kva) {
        ExFreePoolWithTag(bo, RKNPU_POOL_TAG);
        NpuBoUnreserve(Ctx, size);
        RKNPU_LOG("CREATE_BO: no contiguous %Iu bytes below 4 GiB", size);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(bo->Kva, size);
    bo->Size = size;
    bo->Pa = MmGetPhysicalAddress(bo->Kva);
    if (bo->Pa.QuadPart == 0 || (ULONGLONG)bo->Pa.QuadPart + size > FOURGIB) {
        /* cannot happen with high = 4 GiB - 1; a stream address is 32 bits */
        RKNPU_LOG("CREATE_BO: got PA 0x%llx + 0x%Ix, not usable", bo->Pa.QuadPart, size);
        BoDestroy(bo);
        NpuBoUnreserve(Ctx, size);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    bo->Owner = WdfRequestGetFileObject(Request);

    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    do {
        bo->Handle = ++Ctx->NextHandle;
    } while (bo->Handle == 0);
    InsertTailList(&Ctx->Bos, &bo->Link);
    Ctx->BoCount++;
    WdfWaitLockRelease(Ctx->BoLock);

    NpuCrumb(Ctx, RKNPU_CRUMB_BO_CREATE, 0, 0, (ULONG)size, (ULONG)bo->Pa.QuadPart);
    io->size = (unsigned int)size;
    io->flags = 0;
    io->handle = bo->Handle;
    io->dma_address = (unsigned int)bo->Pa.QuadPart;
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
    } else if (AnyJobActive(Ctx)) {
        /* a mapped stream could be rewritten after SUBMIT checked it */
        status = STATUS_DEVICE_BUSY;
    } else {
        if (!bo->Mdl) {
            bo->Mdl = IoAllocateMdl(bo->Kva, (ULONG)bo->Size, FALSE, FALSE, NULL);
            if (bo->Mdl)
                MmBuildMdlForNonPagedPool(bo->Mdl);
        }
        if (bo->Mdl) {
            __try {
                va = MmMapLockedPagesSpecifyCache(bo->Mdl, UserMode, MmWriteCombined, NULL, FALSE,
                                                  NormalPagePriority | MdlMappingNoExecute);
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

    NpuCrumb(Ctx, RKNPU_CRUMB_BO_MAP, 0, 0, handle, (ULONG)bo->Pa.QuadPart);
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

/* Caller context (it may unmap). Refused while any job runs: the NPU may be reading it. */
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
    } else if (AnyJobActive(Ctx)) {
        status = STATUS_DEVICE_BUSY;
        bo = NULL;
    } else {
        RemoveEntryList(&bo->Link);
        Ctx->BoCount--;
        Ctx->BoBytes -= bo->Size;
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
    } else if (Write && AnyJobActive(Ctx)) {
        status = STATUS_DEVICE_BUSY;
    } else {
        if (Write)
            RtlCopyMemory((PUCHAR)bo->Kva + req.offset, data, req.length);
        else
            RtlCopyMemory(data, (PUCHAR)bo->Kva + req.offset, req.length);
        *Info = Write ? 0 : req.length;
    }
    WdfWaitLockRelease(Ctx->BoLock);
    return status;
}

/* ------------------------------------------------------ submit, wait */

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
        (in.flags & ~RKNPU_SUBMIT_NO_CRUMBS))
        return STATUS_INVALID_PARAMETER;
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
    for (i = 0; i < RKNPU_MMUS_PER_CORE; i++) {
        if (NpuRd(Ctx, base + MmuInst[i] + RK_MMU_STATUS) & RK_MMU_STATUS_PAGING) {
            RKNPU_LOG("SUBMIT: core %u IOMMU %u has paging on; M3 runs with it off", n, i);
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
            goto out_job;
        }
    }

    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    /* Every stream inside one of the caller's buffers, and within the rules. */
    for (i = 0; i < in.task_count; i++) {
        ULONG bad;

        status = NpuCheckStreamLocked(Ctx, fo, &in.tasks[i], &bad);
        if (!NT_SUCCESS(status))
            goto out_bo;
    }

    seq = ++Ctx->NextSeq;
    RtlZeroMemory(j, sizeof(*j));
    j->Seq = seq;
    j->Owner = fo;
    j->TaskCount = in.task_count;
    j->NoCrumbs = (in.flags & RKNPU_SUBMIT_NO_CRUMBS) ? TRUE : FALSE;
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
    if (in.flags != 0)
        return STATUS_INVALID_PARAMETER;
    ms = in.timeout_ms ? in.timeout_ms : RKNPU_WAIT_DEFAULT_MS;
    if (ms > RKNPU_WAIT_MAX_MS)
        ms = RKNPU_WAIT_MAX_MS;

    WdfWaitLockAcquire(Ctx->JobLock, NULL);
    for (n = 0; n < RKNPU_NUM_CORES; n++)
        if (in.seqno != 0 && Ctx->Core[n].Job.Seq == in.seqno && Ctx->Core[n].Job.Owner == fo)
            break;
    if (n == RKNPU_NUM_CORES) {
        RtlZeroMemory(out, sizeof(*out));
        out->seqno = in.seqno;
        out->result = RKNPU_JOB_UNKNOWN;
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

    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    for (e = Ctx->Bos.Flink; e != &Ctx->Bos; e = next) {
        PNPU_BO bo = CONTAINING_RECORD(e, NPU_BO, Link);
        next = e->Flink;
        if (bo->Owner == FileObject) {
            RemoveEntryList(&bo->Link);
            Ctx->BoCount--;
            Ctx->BoBytes -= bo->Size;
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
}

/* Device cleanup: every handle has closed, so nothing is mapped or running. */
VOID
NpuM3FreeAll(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    LIST_ENTRY mine;

    if (!Ctx->BoLock)
        return;
    InitializeListHead(&mine);
    WdfWaitLockAcquire(Ctx->BoLock, NULL);
    while (!IsListEmpty(&Ctx->Bos))
        InsertTailList(&mine, RemoveHeadList(&Ctx->Bos));
    Ctx->BoCount = 0;
    Ctx->BoBytes = 0;
    WdfWaitLockRelease(Ctx->BoLock);
    while (!IsListEmpty(&mine))
        BoDestroy(CONTAINING_RECORD(RemoveHeadList(&mine), NPU_BO, Link));
}
