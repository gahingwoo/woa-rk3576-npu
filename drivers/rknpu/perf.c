/*
 * perf.c - rknpu.sys as a kernel-mode performance counter (PCW) provider
 *
 * Two counter sets, described in rknpu.man and compiled by ctrpp into
 * rknpuCounters.h (the Register, Unregister and Add helpers used here) and
 * rknpuCounters.rc (the names, linked in through rknpu.rc):
 *
 *   "RK3576 NPU"         instances core0 and core1 (RKNPU_PERF_CORE)
 *   "RK3576 NPU Memory"  one instance (RKNPU_PERF_MEMORY)
 *
 * Both are callback providers: nothing is copied anywhere until a consumer
 * (perfmon, typeperf, Get-Counter, PDH) asks, and then the callback builds
 * the values from the running totals. The job path pays for a handful of
 * 64-bit adds under the interrupt lock it already holds (CORE_PERF in
 * rknpu.h), using the QPC timestamps M3 and M7 already take: T0 at the first
 * OPERATION_ENABLE, T1 at the last interrupt, T2 in the DPC, and the SUBMIT's
 * own entry time.
 *
 * % busy counts a running job up to the moment of the query, so a long job
 * shows as busy while it runs, not as a spike when it ends.
 *
 * Registering can fail (or the provider may never have been installed with
 * lodctr); the driver then runs exactly as before, without counters.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rknpu.h"
#include "rknpu_perf.h"

/*
 * The ctrpp helpers register with PCW_CURRENT_VERSION, which the 26100 WDK
 * makes PCW_VERSION_2 (build 20348 and later). Windows 10 19041's PcwRegister
 * takes only PCW_VERSION_1 and fails anything else with
 * STATUS_INVALID_PARAMETER_2 (0xC00000F0, measured on the CM5-IO Windows 10
 * 19045), so the counters never existed there. Ask for the version the
 * running kernel knows; Windows 11 keeps PCW_VERSION_2.
 */
static ULONG
NpuPcwVersion(VOID)
{
    return RtlIsNtDdiVersionAvailable(NTDDI_WIN10_FE) ? PCW_VERSION_2 : PCW_VERSION_1;
}
#undef PCW_CURRENT_VERSION
#define PCW_CURRENT_VERSION NpuPcwVersion()

#include "rknpuCounters.h"      /* ctrpp, from rknpu.man, in the object directory */

#define PERF_100NS_PER_S    10000000ull
#define PERF_US_PER_S       1000000ull

static BOOLEAN NpuCoreRegistered;
static BOOLEAN NpuMemoryRegistered;

static const PCWSTR CoreName[RKNPU_NUM_CORES] = { L"core0", L"core1" };

/* ------------------------------------------------ writers (interrupt lock) */

static __forceinline VOID
PerfWriteBegin(
    _Inout_ CORE_PERF *P
    )
{
    InterlockedIncrement(&P->Seq);      /* odd: a writer is inside; full barrier */
}

static __forceinline VOID
PerfWriteEnd(
    _Inout_ CORE_PERF *P
    )
{
    InterlockedIncrement(&P->Seq);      /* even again; full barrier */
}

/* SUBMIT, right after T0, before the first task starts. */
VOID
NpuPerfJobStartLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N,
    _In_ LONGLONG SubmitQpc
    )
{
    CORE_PERF *p = &Ctx->Core[N].Perf;

    PerfWriteBegin(p);
    p->RunStart = Ctx->Core[N].Job.T0.QuadPart;
    if (p->RunStart == 0)
        p->RunStart = 1;                /* 0 means idle */
    p->SubmitQpc = SubmitQpc;
    PerfWriteEnd(p);
}

/* The job left the hardware (finished, DMA error, IOMMU fault, stopped); T1 is set. */
VOID
NpuPerfJobEndLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    CORE_PERF *p = &Ctx->Core[N].Perf;
    const CORE_JOB *j = &Ctx->Core[N].Job;
    LONGLONG ran = j->T1.QuadPart - j->T0.QuadPart;

    if (p->RunStart == 0)
        return;                         /* already ended (or never started here) */
    PerfWriteBegin(p);
    if (ran > 0)
        p->BusyTicks += (ULONGLONG)ran;
    p->JobsEnded++;
    p->TasksDone += j->TasksDone;
    p->RunStart = 0;
    PerfWriteEnd(p);
}

/* The DPC completed the job (DONE, DMA error or IOMMU fault); T2 is set. */
VOID
NpuPerfJobDoneLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    CORE_PERF *p = &Ctx->Core[N].Perf;
    LONGLONG lat = Ctx->Core[N].Job.T2.QuadPart - p->SubmitQpc;

    PerfWriteBegin(p);
    if (p->SubmitQpc != 0 && lat > 0)
        p->LatencyTicks += (ULONGLONG)lat;
    p->JobsDone++;
    PerfWriteEnd(p);
}

/* ------------------------------------------------------------- reader */

typedef struct _PERF_SNAP {
    LONGLONG    RunStart;
    ULONGLONG   BusyTicks;
    ULONGLONG   JobsEnded;
    ULONGLONG   TasksDone;
    ULONGLONG   JobsDone;
    ULONGLONG   LatencyTicks;
} PERF_SNAP;

/*
 * A consistent copy without the interrupt lock: retry while a writer is
 * inside or one came and went. A writer runs at DIRQL under a spin lock, so
 * it finishes quickly; the bound only guards against a bug.
 */
static VOID
PerfSnapshot(
    _In_ CORE_PERF *P,
    _Out_ PERF_SNAP *S
    )
{
    LONG s1, s2;
    ULONG tries = 0;

    do {
        s1 = ReadAcquire(&P->Seq);
        if (s1 & 1) {
            YieldProcessor();
            continue;
        }
        S->RunStart     = ReadNoFence64(&P->RunStart);
        S->BusyTicks    = (ULONGLONG)ReadNoFence64((volatile LONG64 *)&P->BusyTicks);
        S->JobsEnded    = (ULONGLONG)ReadNoFence64((volatile LONG64 *)&P->JobsEnded);
        S->TasksDone    = (ULONGLONG)ReadNoFence64((volatile LONG64 *)&P->TasksDone);
        S->JobsDone     = (ULONGLONG)ReadNoFence64((volatile LONG64 *)&P->JobsDone);
        S->LatencyTicks = (ULONGLONG)ReadNoFence64((volatile LONG64 *)&P->LatencyTicks);
        KeMemoryBarrier();
        s2 = ReadNoFence(&P->Seq);
        if (s1 == s2)
            return;
    } while (++tries < 100000);
}

static VOID
NpuPerfFillCore(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N,
    _Out_ RKNPU_PERF_CORE *V
    )
{
    CORE_STATE *core = &Ctx->Core[N];
    ULONGLONG freq = (ULONGLONG)Ctx->QpcFreq.QuadPart;
    ULONGLONG busy, busy100, last;
    LONGLONG now;
    PERF_SNAP s;

    RtlZeroMemory(&s, sizeof(s));
    PerfSnapshot(&core->Perf, &s);
    now = KeQueryPerformanceCounter(NULL).QuadPart;

    busy = s.BusyTicks;
    if (s.RunStart != 0 && now > s.RunStart)
        busy += (ULONGLONG)(now - s.RunStart);
    busy100 = rknpu_perf_scale(busy, freq, PERF_100NS_PER_S);

    /* The running job was counted up to "now"; when it ends its T1 can be a
     * little earlier than a previous query's "now". Never step back. */
    for (;;) {
        last = (ULONGLONG)ReadNoFence64(&core->Perf.LastBusy100ns);
        if (busy100 <= last) {
            busy100 = last;
            break;
        }
        if ((ULONGLONG)InterlockedCompareExchange64(&core->Perf.LastBusy100ns, (LONG64)busy100,
                                                    (LONG64)last) == last)
            break;
    }

    RtlZeroMemory(V, sizeof(*V));
    V->BusyTime           = busy100;
    V->JobsCompleted      = s.JobsDone;
    V->TasksCompleted     = s.TasksDone;
    V->JobTimeUs          = rknpu_perf_scale(s.BusyTicks, freq, PERF_US_PER_S);
    V->JobTimeBase        = (unsigned int)s.JobsEnded;
    V->LatencyUs          = rknpu_perf_scale(s.LatencyTicks, freq, PERF_US_PER_S);
    V->LatencyBase        = (unsigned int)s.JobsDone;
    V->JobsSubmitted      = (ULONG)ReadNoFence(&core->JobsSubmitted);
    V->JobsCompletedTotal = s.JobsDone;
    V->Timeouts           = (ULONG)ReadNoFence(&core->JobsTimedOut);
    V->Resets             = (ULONG)ReadNoFence(&core->Resets);
    V->MmuFaults          = (ULONG)ReadNoFence(&core->MmuFaults);
    V->CbufWaits          = (ULONG)ReadNoFence(&core->CbufWaits);
    V->Running            = s.RunStart != 0 ? 1 : 0;
}

/* Read without BoLock: aligned word reads, a moment's staleness is fine. */
static VOID
NpuPerfFillMemory(
    _In_ PDEVICE_CONTEXT Ctx,
    _Out_ RKNPU_PERF_MEMORY *V
    )
{
    RtlZeroMemory(V, sizeof(*V));
    V->BufferBytes    = (ULONGLONG)*(volatile SIZE_T *)&Ctx->BoBytes;
    V->Buffers        = *(volatile ULONG *)&Ctx->BoCount;
    V->IovaBytes      = Ctx->Mmu.On ? (ULONGLONG)*(volatile ULONG *)&Ctx->Mmu.ChunksUsed << NPU_IOVA_CHUNK_SHIFT : 0;
    V->PageTablePages = *(volatile ULONG *)&Ctx->Mmu.PtPages;
}

/* ---------------------------------------------------------- callbacks */

static NTSTATUS NTAPI
NpuPerfCoreCallback(
    _In_ PCW_CALLBACK_TYPE Type,
    _In_ PPCW_CALLBACK_INFORMATION Info,
    _In_opt_ PVOID Context
    )
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    RKNPU_PERF_CORE v;
    UNICODE_STRING name;
    NTSTATUS status;
    ULONG n;

    if (!ctx)
        return STATUS_SUCCESS;
    for (n = 0; n < RKNPU_NUM_CORES; n++) {
        RtlInitUnicodeString(&name, CoreName[n]);
        switch (Type) {
        case PcwCallbackEnumerateInstances:
            status = RknpuAddNpuCore(Info->EnumerateInstances.Buffer, &name, n, NULL);
            break;
        case PcwCallbackCollectData:
            NpuPerfFillCore(ctx, n, &v);
            status = RknpuAddNpuCore(Info->CollectData.Buffer, &name, n, &v);
            break;
        default:
            return STATUS_SUCCESS;
        }
        if (!NT_SUCCESS(status))
            return status;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
NpuPerfMemoryCallback(
    _In_ PCW_CALLBACK_TYPE Type,
    _In_ PPCW_CALLBACK_INFORMATION Info,
    _In_opt_ PVOID Context
    )
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    RKNPU_PERF_MEMORY v;
    UNICODE_STRING name;

    if (!ctx)
        return STATUS_SUCCESS;
    RtlInitUnicodeString(&name, L"default");    /* a single-instance set; consumers do not show the name */
    switch (Type) {
    case PcwCallbackEnumerateInstances:
        return RknpuAddNpuMemory(Info->EnumerateInstances.Buffer, &name, 0, NULL);
    case PcwCallbackCollectData:
        NpuPerfFillMemory(ctx, &v);
        return RknpuAddNpuMemory(Info->CollectData.Buffer, &name, 0, &v);
    default:
        return STATUS_SUCCESS;
    }
}

/* ------------------------------------------------------- registration */

/* From EvtDeviceAdd, once the context is set up. Failure is not fatal. */
VOID
NpuPerfRegister(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    NTSTATUS status;

    if (!NpuCoreRegistered) {
        status = RknpuRegisterNpuCore(NpuPerfCoreCallback, Ctx);
        if (NT_SUCCESS(status))
            NpuCoreRegistered = TRUE;
        else
            RKNPU_LOG("performance counters: \"RK3576 NPU\" not registered: 0x%08x; running without", status);
    }
    if (!NpuMemoryRegistered) {
        status = RknpuRegisterNpuMemory(NpuPerfMemoryCallback, Ctx);
        if (NT_SUCCESS(status))
            NpuMemoryRegistered = TRUE;
        else
            RKNPU_LOG("performance counters: \"RK3576 NPU Memory\" not registered: 0x%08x; running without", status);
    }
}

/* From the device's cleanup. PcwUnregister returns after any running callback. */
VOID
NpuPerfUnregister(VOID)
{
    if (NpuCoreRegistered) {
        RknpuUnregisterNpuCore();
        NpuCoreRegistered = FALSE;
    }
    if (NpuMemoryRegistered) {
        RknpuUnregisterNpuMemory();
        NpuMemoryRegistered = FALSE;
    }
}
