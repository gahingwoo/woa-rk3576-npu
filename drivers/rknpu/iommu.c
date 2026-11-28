/*
 * iommu.c - RK3576 NPU, M6: the driver owns the four NPU IOMMUs
 *
 * Each NPU core has two Rockchip IOMMU instances at +0x2000 and +0x2100 of
 * its window (rk3576.dtsi: rknn_mmu_0/1, "rockchip,rk3576-npu-iommu",
 * "rockchip,rk3568-iommu", the v2 page-table format). Windows does not know
 * them: they are not SMMUs and have no IORT entry, the firmware leaves them
 * in reset with paging off (STATUS 0x18, edk2-rk3576 Npu.c), and they sit in
 * the window the SSDT already gives this driver, on the interrupt lines it
 * already has. Their clocks (aclk_rknn0/1, hclk_rknn_root, clk_rknn_dsu0,
 * aclk/hclk_rknn_cbuf) and power domains (PD_NPU0/1) are the ones Npu.c
 * turns on for the cores, so nothing in the firmware changes for M6.
 *
 * WHAT THIS DOES, all of it after drivers/iommu/rockchip-iommu.c (GPL-2.0):
 *
 *   - One address space for the device: a page directory (1024 entries of
 *     4 MiB) and page tables of 1024 4 KiB pages, allocated as they are
 *     needed. v2 entries carry a 40-bit physical address (rk_mk_dte_v2,
 *     rk_mk_pte_v2). All four instances point at the same directory.
 *   - The tables are write-combined (Normal non-cacheable): the IOMMU walks
 *     them over the bus and is not coherent, so a table write is visible to
 *     it after a DSB, with no cache maintenance (rockchip-iommu.c's
 *     rk_table_flush() is a dma_sync for the same reason).
 *   - IOVAs are handed out in 64 KiB chunks from 1 MiB up (no buffer at 0,
 *     which is the unset value of a stream pointer), and every buffer gets
 *     at least one unmapped page after its end, so an overrun faults rather
 *     than reaching the next buffer. A chunk table names the buffer that
 *     owns each chunk, which turns SUBMIT's address check into one lookup.
 *   - Paging is turned on per core when a job is about to run on it and is
 *     found off, or with the wrong directory (rk_iommu_enable(): stall,
 *     FORCE_RESET with the DTE_ADDR read-back test, DTE_ADDR, ZAP_CACHE,
 *     interrupt mask, AUTO_GATING bit 31, ENABLE_PAGING, unstall). A core
 *     reset can take the IOMMU with it; the next SUBMIT sees that and turns
 *     it on again, which is the recovery the Linux series needed
 *     ("MMU_DTE_ADDR is not functioning" after repeated resets) done on
 *     every job instead of once.
 *   - Before a core reset, and when the device leaves D0, paging is turned
 *     off (rk_iommu_disable()), so the firmware and the next OS find what the
 *     firmware left.
 *   - Unmapping invalidates the entries and zaps each page from both
 *     instances of both cores (rk_iommu_unmap()); mapping zaps the first and
 *     last page, the only ones whose table lines another mapping can share
 *     (rk_iommu_map()).
 *   - A page fault or bus error is acked in the ISR (ZAP_CACHE,
 *     PAGE_FAULT_DONE, INT_CLEAR, rk_iommu_irq()) and stops the job on that
 *     core (m3.c, RKNPU_JOB_MMU_FAULT); WAIT then resets the core.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rknpu.h"

static const ULONG MmuOff[RKNPU_MMUS_PER_CORE] = { NPU_MMU0_OFF, NPU_MMU1_OFF };

#define MMU_POLL_US             1000u   /* RK_MMU_POLL_TIMEOUT_US */
#define MMU_DTE_ADDR_DUMMY_V2   0xCAFEB000u /* rk_dte_pt_address_v2(DTE_ADDR_DUMMY), as a u32 */
#define PHYS_40BIT              0xFFFFFFFFFFull

static __forceinline ULONG
MmuRd(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N,
    _In_ ULONG I,
    _In_ ULONG Reg
    )
{
    return NpuRd(Ctx, NpuCoreBase(N) + MmuOff[I] + Reg);
}

static __forceinline VOID
MmuWr(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N,
    _In_ ULONG I,
    _In_ ULONG Reg,
    _In_ ULONG V
    )
{
    NpuWr(Ctx, NpuCoreBase(N) + MmuOff[I] + Reg, V);
}

/* Table writes (write-combined) are in memory before the IOMMU is told. */
static __forceinline VOID
TableBarrier(VOID)
{
    KeMemoryBarrier();
#if defined(_M_ARM64)
    __dsb(_ARM64_BARRIER_SY);
#endif
}

/* rk_mk_dte_v2(): 31:12 address 31:12, 11:8 address 35:32, 7:4 address 39:36, 0 valid */
static ULONG
MkDteV2(
    _In_ ULONGLONG Pa
    )
{
    ULONGLONG v = (Pa & 0xFFFFF000ull) | ((Pa & 0xF00000000ull) >> 24) | ((Pa & 0xF000000000ull) >> 32);

    return (ULONG)(v & 0xFFFFFFF0ull) | 0x1u;
}

/* rk_mk_pte_v2(): the same address encoding, bit 1 readable, bit 2 writable */
static ULONG
MkPteV2(
    _In_ ULONGLONG Pa,
    _In_ BOOLEAN Writable
    )
{
    return MkDteV2(Pa) | 0x2u | (Writable ? 0x4u : 0u);
}

/* ---------------------------------------------------------- the tables */

static PVOID
TablePageAlloc(
    _Out_ PHYSICAL_ADDRESS *Pa
    )
{
    PHYSICAL_ADDRESS low, high, skip;
    PVOID p;

    /* Below 4 GiB, although v2 entries reach 2^40: then the directory and
     * table addresses never use the shuffled bits 11:4 of an entry, and
     * DTE_ADDR reads back as rk_iommu_force_reset() expects. One page at a
     * time, so this never meets M4's contiguity problem. */
    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFF;
    skip.QuadPart = 0;
    p = MmAllocateContiguousNodeMemory(PAGE_SIZE, low, high, skip,
                                       PAGE_READWRITE | PAGE_WRITECOMBINE, MM_ANY_NODE_OK);
    if (!p)
        return NULL;
    NpuDevZero(p, PAGE_SIZE);   /* write-combined, so not RtlZeroMemory (rknpu.h) */
    *Pa = MmGetPhysicalAddress(p);
    return p;
}

NTSTATUS
NpuMmuInit(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    NPU_MMU *m = &Ctx->Mmu;

    if (!m->On)
        return STATUS_SUCCESS;
    m->Dt = (PULONG)TablePageAlloc(&m->DtPa);
    m->ChunkBits = (PULONG)ExAllocatePool2(POOL_FLAG_NON_PAGED, NPU_IOVA_CHUNKS / 8, RKNPU_POOL_TAG);
    m->ChunkOwner = (PNPU_BO *)ExAllocatePool2(POOL_FLAG_NON_PAGED, NPU_IOVA_CHUNKS * sizeof(PNPU_BO),
                                               RKNPU_POOL_TAG);
    if (!m->Dt || !m->ChunkBits || !m->ChunkOwner) {
        NpuMmuFree(Ctx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    m->PtPages = 1;
    RtlInitializeBitMap(&m->Chunks, m->ChunkBits, NPU_IOVA_CHUNKS);
    RtlClearAllBits(&m->Chunks);
    /* below 1 MiB, and the top chunk (an IOVA + length must not wrap) */
    RtlSetBits(&m->Chunks, 0, NPU_IOVA_FIRST_CHUNK);
    RtlSetBits(&m->Chunks, NPU_IOVA_CHUNKS - 1, 1);
    m->Hint = NPU_IOVA_FIRST_CHUNK;
    RKNPU_LOG("IOMMU mode: page directory at 0x%llx", m->DtPa.QuadPart);
    return STATUS_SUCCESS;
}

/* Device cleanup: every buffer is gone and paging is off. */
VOID
NpuMmuFree(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    NPU_MMU *m = &Ctx->Mmu;
    ULONG i;

    for (i = 0; i < NPU_PT_ENTRIES; i++) {
        if (m->Pt[i]) {
            MmFreeContiguousMemory(m->Pt[i]);
            m->Pt[i] = NULL;
        }
    }
    if (m->Dt) {
        MmFreeContiguousMemory(m->Dt);
        m->Dt = NULL;
    }
    if (m->ChunkBits) {
        ExFreePoolWithTag(m->ChunkBits, RKNPU_POOL_TAG);
        m->ChunkBits = NULL;
    }
    if (m->ChunkOwner) {
        ExFreePoolWithTag(m->ChunkOwner, RKNPU_POOL_TAG);
        m->ChunkOwner = NULL;
    }
    m->PtPages = 0;
}

/* The page table for Iova, made (and entered in the directory) if needed. */
static PULONG
PtGetLocked(
    _In_ NPU_MMU *M,
    _In_ ULONG Iova
    )
{
    ULONG d = Iova >> 22;

    if (!M->Pt[d]) {
        M->Pt[d] = (PULONG)TablePageAlloc(&M->PtPa[d]);
        if (!M->Pt[d])
            return NULL;
        M->PtPages++;
        M->Dt[d] = MkDteV2((ULONGLONG)M->PtPa[d].QuadPart);
    }
    return M->Pt[d];
}

/* ------------------------------------------------------------- zapping */

/* RK_MMU_ZAP_ONE_LINE for each page, on every instance with paging on. */
static VOID
ZapLines(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Iova,
    _In_ ULONG Pages
    )
{
    ULONG n, i, p;

    if (!NpuSafeToRead(Ctx))
        return;
    for (n = 0; n < RKNPU_NUM_CORES; n++) {
        for (i = 0; i < RKNPU_MMUS_PER_CORE; i++) {
            if (!(MmuRd(Ctx, n, i, RK_MMU_STATUS) & RK_MMU_STATUS_PAGING))
                continue;       /* turning it on zaps the whole TLB */
            for (p = 0; p < Pages; p++)
                MmuWr(Ctx, n, i, RK_MMU_ZAP_ONE_LINE, Iova + p * PAGE_SIZE);
        }
    }
}

/* ------------------------------------------------------------- mapping */

/*
 * Map the pages of Mdl (Size bytes, page multiple) at a fresh IOVA. Bo is
 * entered as the chunks' owner (NULL for the driver's own memory, which no
 * stream may name). The IOVA range is Size rounded up to whole chunks with
 * at least one page to spare: that page, and the rest of the last chunk,
 * stay unmapped. BoLock held.
 */
NTSTATUS
NpuMmuMapLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ PMDL Mdl,
    _In_ SIZE_T Size,
    _In_ BOOLEAN Writable,
    _In_opt_ PNPU_BO Bo,
    _Out_ ULONG *Iova
    )
{
    NPU_MMU *m = &Ctx->Mmu;
    PPFN_NUMBER pfn = MmGetMdlPfnArray(Mdl);
    ULONG pages = (ULONG)(Size >> PAGE_SHIFT), chunks, first, p, iova;

    *Iova = 0;
    if (!m->On || !m->Dt || pages == 0)
        return STATUS_INVALID_DEVICE_STATE;
    chunks = (ULONG)((Size + PAGE_SIZE + ((1u << NPU_IOVA_CHUNK_SHIFT) - 1)) >> NPU_IOVA_CHUNK_SHIFT);
    first = RtlFindClearBitsAndSet(&m->Chunks, chunks, m->Hint);
    if (first == 0xFFFFFFFF)
        return STATUS_INSUFFICIENT_RESOURCES;
    iova = first << NPU_IOVA_CHUNK_SHIFT;

    for (p = 0; p < pages; p++) {
        ULONG a = iova + p * PAGE_SIZE;
        PULONG pt = PtGetLocked(m, a);
        ULONGLONG pa = (ULONGLONG)pfn[p] << PAGE_SHIFT;

        if (!pt || pa > PHYS_40BIT) {
            /* undo what was written; nothing has used it yet */
            ULONG q;

            for (q = 0; q < p; q++)
                m->Pt[(iova + q * PAGE_SIZE) >> 22][((iova + q * PAGE_SIZE) >> 12) & 0x3FF] = 0;
            TableBarrier();
            RtlClearBits(&m->Chunks, first, chunks);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        pt[(a >> 12) & 0x3FF] = MkPteV2(pa, Writable);
    }
    TableBarrier();
    for (p = 0; p < chunks; p++)
        m->ChunkOwner[first + p] = Bo;
    m->ChunksUsed += chunks;
    m->Hint = first + chunks;
    if (m->Hint >= NPU_IOVA_CHUNKS - 1)
        m->Hint = NPU_IOVA_FIRST_CHUNK;

    /* rk_iommu_map_iova(): a stale line can only hold the first or last page */
    ZapLines(Ctx, iova, 1);
    if (pages > 1)
        ZapLines(Ctx, iova + (pages - 1) * PAGE_SIZE, 1);
    *Iova = iova;
    return STATUS_SUCCESS;
}

/* Undo NpuMmuMapLocked. Nothing may be using the range. BoLock held. */
VOID
NpuMmuUnmapLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Iova,
    _In_ SIZE_T Size
    )
{
    NPU_MMU *m = &Ctx->Mmu;
    ULONG pages = (ULONG)(Size >> PAGE_SHIFT), chunks, first, p;

    if (!m->On || !m->Dt || Iova == 0)
        return;
    chunks = (ULONG)((Size + PAGE_SIZE + ((1u << NPU_IOVA_CHUNK_SHIFT) - 1)) >> NPU_IOVA_CHUNK_SHIFT);
    first = Iova >> NPU_IOVA_CHUNK_SHIFT;
    for (p = 0; p < pages; p++) {
        ULONG a = Iova + p * PAGE_SIZE;

        if (m->Pt[a >> 22])
            m->Pt[a >> 22][(a >> 12) & 0x3FF] = 0;
    }
    TableBarrier();
    ZapLines(Ctx, Iova, pages);
    for (p = 0; p < chunks; p++)
        m->ChunkOwner[first + p] = NULL;
    RtlClearBits(&m->Chunks, first, chunks);
    m->ChunksUsed -= chunks;
}

/* The buffer whose chunk holds Iova, or NULL. BoLock held. */
PNPU_BO
NpuMmuOwnerLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Iova
    )
{
    if (!Ctx->Mmu.ChunkOwner)
        return NULL;
    return Ctx->Mmu.ChunkOwner[Iova >> NPU_IOVA_CHUNK_SHIFT];
}

/* ----------------------------------------------------- enable, disable */

typedef enum { MMU_ALL_PAGING, MMU_NO_PAGING, MMU_ALL_STALL, MMU_NO_STALL, MMU_ALL_RESET } MMU_WAIT_FOR;

static BOOLEAN
MmuCondition(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N,
    _In_ MMU_WAIT_FOR What
    )
{
    ULONG i, st;

    for (i = 0; i < RKNPU_MMUS_PER_CORE; i++) {
        if (What == MMU_ALL_RESET) {
            if (MmuRd(Ctx, N, i, RK_MMU_DTE_ADDR) != 0)
                return FALSE;
            continue;
        }
        st = MmuRd(Ctx, N, i, RK_MMU_STATUS);
        switch (What) {
        case MMU_ALL_PAGING: if (!(st & RK_MMU_STATUS_PAGING)) return FALSE; break;
        case MMU_NO_PAGING:  if (st & RK_MMU_STATUS_PAGING) return FALSE; break;
        case MMU_ALL_STALL:  if (!(st & RK_MMU_ST_STALL_ACTIVE)) return FALSE; break;
        case MMU_NO_STALL:   if (st & RK_MMU_ST_STALL_ACTIVE) return FALSE; break;
        default: break;
        }
    }
    return TRUE;
}

/* readx_poll_timeout(), at PASSIVE_LEVEL, in 10 us steps */
static BOOLEAN
MmuPoll(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N,
    _In_ MMU_WAIT_FOR What
    )
{
    ULONG t;

    for (t = 0; t <= MMU_POLL_US; t += 10) {
        if (MmuCondition(Ctx, N, What))
            return TRUE;
        KeStallExecutionProcessor(10);
    }
    return MmuCondition(Ctx, N, What);
}

static VOID
MmuCommand(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N,
    _In_ ULONG Cmd
    )
{
    ULONG i;

    for (i = 0; i < RKNPU_MMUS_PER_CORE; i++)
        MmuWr(Ctx, N, i, RK_MMU_COMMAND, Cmd);
}

/* rk_iommu_enable_stall(): only meaningful with paging on */
static BOOLEAN
MmuStall(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    ULONG i;

    if (MmuCondition(Ctx, N, MMU_ALL_STALL) || !MmuCondition(Ctx, N, MMU_ALL_PAGING))
        return TRUE;
    /* a bank left in PAGE_FAULT_ACTIVE ignores ENABLE_STALL */
    for (i = 0; i < RKNPU_MMUS_PER_CORE; i++)
        if (MmuRd(Ctx, N, i, RK_MMU_STATUS) & RK_MMU_ST_PAGE_FAULT_ACTIVE)
            MmuWr(Ctx, N, i, RK_MMU_COMMAND, RK_MMU_CMD_PAGE_FAULT_DONE);
    MmuCommand(Ctx, N, RK_MMU_CMD_ENABLE_STALL);
    return MmuPoll(Ctx, N, MMU_ALL_STALL);
}

static BOOLEAN
MmuUnstall(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    if (!MmuCondition(Ctx, N, MMU_ALL_STALL))
        return TRUE;
    MmuCommand(Ctx, N, RK_MMU_CMD_DISABLE_STALL);
    return MmuPoll(Ctx, N, MMU_NO_STALL);
}

static ULONG
MmuStatusPair(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    return (MmuRd(Ctx, N, 0, RK_MMU_STATUS) & 0xFFFF) << 16 | (MmuRd(Ctx, N, 1, RK_MMU_STATUS) & 0xFFFF);
}

/* rk_iommu_enable() for core N's two instances. JobLock and BoLock held. */
static NTSTATUS
MmuEnable(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    ULONG want = MkDteV2((ULONGLONG)Ctx->Mmu.DtPa.QuadPart), i;
    NTSTATUS status = STATUS_SUCCESS;

    if (!MmuStall(Ctx, N))
        RKNPU_LOG("core %u IOMMU: stall timed out, status 0x%08x", N, MmuStatusPair(Ctx, N));

    /* rk_iommu_force_reset(): DTE_ADDR must hold what is written to it */
    for (i = 0; i < RKNPU_MMUS_PER_CORE; i++) {
        MmuWr(Ctx, N, i, RK_MMU_DTE_ADDR, MMU_DTE_ADDR_DUMMY_V2);
        if (MmuRd(Ctx, N, i, RK_MMU_DTE_ADDR) != MMU_DTE_ADDR_DUMMY_V2) {
            RKNPU_LOG("core %u IOMMU %u: DTE_ADDR is not functioning (reads 0x%08x)",
                      N, i, MmuRd(Ctx, N, i, RK_MMU_DTE_ADDR));
            status = STATUS_DEVICE_HARDWARE_ERROR;
            goto out;
        }
    }
    MmuCommand(Ctx, N, RK_MMU_CMD_FORCE_RESET);
    if (!MmuPoll(Ctx, N, MMU_ALL_RESET)) {
        RKNPU_LOG("core %u IOMMU: FORCE_RESET timed out", N);
        status = STATUS_IO_TIMEOUT;
        goto out;
    }

    for (i = 0; i < RKNPU_MMUS_PER_CORE; i++) {
        MmuWr(Ctx, N, i, RK_MMU_DTE_ADDR, want);
        MmuWr(Ctx, N, i, RK_MMU_COMMAND, RK_MMU_CMD_ZAP_CACHE);
        MmuWr(Ctx, N, i, RK_MMU_INT_MASK, RK_MMU_IRQ_MASK);
        /* "Workaround for iommu blocked, BIT(31) default to 1" */
        MmuWr(Ctx, N, i, RK_MMU_AUTO_GATING,
              MmuRd(Ctx, N, i, RK_MMU_AUTO_GATING) | RK_MMU_DISABLE_FETCH_DTE_TIME_LIMIT);
    }
    MmuCommand(Ctx, N, RK_MMU_CMD_ENABLE_PAGING);
    if (!MmuPoll(Ctx, N, MMU_ALL_PAGING)) {
        RKNPU_LOG("core %u IOMMU: paging did not come on, status 0x%08x", N, MmuStatusPair(Ctx, N));
        status = STATUS_IO_TIMEOUT;
    }
out:
    if (!MmuUnstall(Ctx, N))
        RKNPU_LOG("core %u IOMMU: unstall timed out, status 0x%08x", N, MmuStatusPair(Ctx, N));
    InterlockedIncrement(&Ctx->Core[N].MmuEnables);
    RKNPU_LOG("core %u IOMMU: paging on with directory 0x%llx: 0x%08x, status 0x%08x",
              N, Ctx->Mmu.DtPa.QuadPart, status, MmuStatusPair(Ctx, N));
    return status;
}

/*
 * Before a job on core N: paging on, with this driver's directory, faults
 * unmasked. Six register reads when all is as it should be. JobLock and
 * BoLock held, PASSIVE_LEVEL, the NPU powered (SUBMIT checked).
 */
NTSTATUS
NpuMmuEnsureLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    ULONG want = MkDteV2((ULONGLONG)Ctx->Mmu.DtPa.QuadPart), i;
    BOOLEAN ok = TRUE;

    if (!Ctx->Mmu.On || !Ctx->Mmu.Dt)
        return STATUS_INVALID_DEVICE_STATE;
    for (i = 0; i < RKNPU_MMUS_PER_CORE; i++) {
        if (!(MmuRd(Ctx, N, i, RK_MMU_STATUS) & RK_MMU_STATUS_PAGING) ||
            ((MmuRd(Ctx, N, i, RK_MMU_DTE_ADDR) ^ want) & ~0xFu))
            ok = FALSE;
    }
    if (ok) {
        /* the ISR masks an instance that faults with no job to stop */
        for (i = 0; i < RKNPU_MMUS_PER_CORE; i++)
            if (MmuRd(Ctx, N, i, RK_MMU_INT_MASK) != RK_MMU_IRQ_MASK)
                MmuWr(Ctx, N, i, RK_MMU_INT_MASK, RK_MMU_IRQ_MASK);
        return STATUS_SUCCESS;
    }
    return MmuEnable(Ctx, N);
}

/* rk_iommu_disable(): paging off, DTE_ADDR 0, interrupts masked. BoLock held. */
VOID
NpuMmuDisableLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    ULONG i;

    if (!Ctx->Mmu.On || !NpuSafeToRead(Ctx))
        return;
    if (MmuCondition(Ctx, N, MMU_NO_PAGING) && MmuRd(Ctx, N, 0, RK_MMU_DTE_ADDR) == 0 &&
        MmuRd(Ctx, N, 1, RK_MMU_DTE_ADDR) == 0)
        return;
    (VOID)MmuStall(Ctx, N);
    if (!MmuCondition(Ctx, N, MMU_NO_PAGING)) {
        MmuCommand(Ctx, N, RK_MMU_CMD_DISABLE_PAGING);
        if (!MmuPoll(Ctx, N, MMU_NO_PAGING))
            RKNPU_LOG("core %u IOMMU: paging did not go off, status 0x%08x", N, MmuStatusPair(Ctx, N));
    }
    for (i = 0; i < RKNPU_MMUS_PER_CORE; i++) {
        MmuWr(Ctx, N, i, RK_MMU_INT_MASK, 0);
        MmuWr(Ctx, N, i, RK_MMU_DTE_ADDR, 0);
    }
    (VOID)MmuUnstall(Ctx, N);
}

/* ----------------------------------------------------------------- ISR */

/*
 * From NpuEvtIsr, at DIRQL with the core's interrupt lock held: ack both
 * instances (rk_iommu_irq()). TRUE if either had something. A page fault or
 * bus error stops the core's job; with no job to stop, the instance is
 * masked until the next SUBMIT, so a fault cannot become an interrupt storm.
 */
BOOLEAN
NpuMmuIsr(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG N
    )
{
    CORE_STATE *core = &Ctx->Core[N];
    BOOLEAN ours = FALSE;
    ULONG i, ms, iova, st;

    for (i = 0; i < RKNPU_MMUS_PER_CORE; i++) {
        ms = MmuRd(Ctx, N, i, RK_MMU_INT_STATUS);
        if (!ms)
            continue;
        ours = TRUE;
        iova = MmuRd(Ctx, N, i, RK_MMU_PAGE_FAULT_ADDR);
        st = MmuRd(Ctx, N, i, RK_MMU_STATUS);
        core->LastMmuIntStatus[i] = ms;
        core->LastMmuFault[i] = iova;
        if (ms & RK_MMU_IRQ_PAGE_FAULT) {
            MmuWr(Ctx, N, i, RK_MMU_COMMAND, RK_MMU_CMD_ZAP_CACHE);
            MmuWr(Ctx, N, i, RK_MMU_COMMAND, RK_MMU_CMD_PAGE_FAULT_DONE);
        }
        MmuWr(Ctx, N, i, RK_MMU_INT_CLEAR, ms);
        if (ms & RK_MMU_IRQ_MASK) {
            InterlockedIncrement(&core->MmuFaults);
            if (core->Job.InFlight)
                NpuM3JobFaultLocked(Ctx, N, iova, (st << 8) | (ms & 0xFF));
            else
                MmuWr(Ctx, N, i, RK_MMU_INT_MASK, 0);
        }
    }
    return ours;
}
