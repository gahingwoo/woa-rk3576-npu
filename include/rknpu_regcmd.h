/*
 * rknpu_regcmd.h - the register command stream, as far as rknpu.sys checks it
 *
 * Shared by the driver (SUBMIT checks every stream against it), rknputest
 * and tools/m3gen (which checks that charsiu's int8 streams satisfy it).
 *
 * An entry is 64 bits: target block in bits 63..48, the value in 47..16 and
 * the register offset in 15..0. This is the encoding of charsiu's emitter
 * (external/charsiu src/regcmd.c, emit()) and of Mesa's rocket driver.
 *
 * WHY THE DRIVER LOOKS INSIDE A STREAM AT ALL. The NPU IOMMUs are left with
 * paging off (M3), so every address in a stream is a physical address and
 * the NPU will read and write wherever it says. A wrong address in a stream
 * is then a write into memory Windows owns, which surfaces later as an
 * unrelated bugcheck (MEMORY_MANAGEMENT, a pool corruption) far from its
 * cause. SUBMIT therefore refuses a stream unless:
 *
 *   - no entry writes the PC block (register offset below 0x1000): a stream
 *     that writes PC_BASE_ADDRESS or PC_REGISTER_AMOUNTS chains to another
 *     stream the driver never saw;
 *   - (M4) every entry writes a (block, register) on the list in
 *     rknpu_regcmd_allow.h, which is every register charsiu_emit_job()
 *     writes and nothing else. tools/m3gen generates it from charsiu over a
 *     grid of jobs and CI checks it on every push; a Llama-3.2-1B run through
 *     the host model of the driver (port/test) writes exactly the same 147;
 *   - every entry that writes one of the address registers below either
 *     writes 0 or an address inside one of the caller's buffers.
 *
 * The address registers are the ones whose value moves when a buffer moves,
 * found by tools/m3gen by moving each buffer alone, over the same grid (CI
 * repeats that too), and confirmed on the whole Llama run by placing every
 * buffer at two different bases. charsiu writes exactly five. M3 could only
 * say that a stream programming an address through some other register
 * passed unseen; with the list, no other register can be written at all, so
 * every address a stream can program is one of these five, and each is
 * checked. What is still not checked is how far past its start the NPU
 * reads or writes (the sizes are in other registers whose meaning differs by
 * operation), so this bounds the mistakes charsiu can make; it is still not
 * a sandbox, and the device stays administrators-only.
 *
 * Device Parameters\M4AllowUnlisted = 1 (read when the device starts) lets
 * registers off the list through, logged; the address check still applies.
 * It is the back-out for a charsiu path the list missed, not a mode to run in.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RKNPU_REGCMD_H
#define RKNPU_REGCMD_H

#define RKNPU_RC_TARGET(e)      ((unsigned int)((e) >> 48) & 0xFFFFu)
#define RKNPU_RC_VALUE(e)       ((unsigned int)((e) >> 16))
#define RKNPU_RC_REG(e)         ((unsigned int)(e) & 0xFFFFu)
#define RKNPU_RC_ENTRY(t, r, v) (((unsigned long long)(t) << 48) | ((unsigned long long)(v) << 16) | (r))

/* charsiu src/regcmd.c */
#define RKNPU_RC_CNA            0x0201u
#define RKNPU_RC_CORE           0x0801u
#define RKNPU_RC_DPU            0x1001u
#define RKNPU_RC_RDMA           0x2001u

#define RKNPU_RC_PC_LIMIT       0x1000u /* register offsets below this are the PC block */

/* Address registers, RK3576 offsets: every one charsiu's streams write (int8, w4a16 and fp16 alike). */
#define RKNPU_RC_CNA_FEATURE_ADDR   0x1088u     /* input (charsiu job->input_addr) */
#define RKNPU_RC_CNA_WEIGHT_ADDR    0x1110u     /* weights (job->weight_addr) */
#define RKNPU_RC_DPU_DST_ADDR       0x4018u     /* output (job->output_addr) */
#define RKNPU_RC_RDMA_BS_ADDR       0x5020u     /* coefficients (job->coef_addr) */
#define RKNPU_RC_RDMA_TABLE_ADDR    0x5024u     /* coefficients, the scale table */

#ifdef _MSC_VER
#define RKNPU_RC_INLINE static __inline
#else
#define RKNPU_RC_INLINE static inline
#endif

RKNPU_RC_INLINE int rknpu_rc_is_addr_reg(unsigned int reg)
{
    return reg == RKNPU_RC_CNA_FEATURE_ADDR || reg == RKNPU_RC_CNA_WEIGHT_ADDR ||
           reg == RKNPU_RC_DPU_DST_ADDR || reg == RKNPU_RC_RDMA_BS_ADDR ||
           reg == RKNPU_RC_RDMA_TABLE_ADDR;
}

RKNPU_RC_INLINE int rknpu_rc_is_pc_reg(unsigned int reg)
{
    return reg < RKNPU_RC_PC_LIMIT;
}

#ifndef RKNPU_RC_NO_ALLOWLIST    /* only m3gen's generator, which writes the list, builds without it */
#include "rknpu_regcmd_allow.h"

/* Whether (block, register) is on the list: a binary search of 147 sorted keys. */
RKNPU_RC_INLINE int rknpu_rc_is_listed(unsigned int target, unsigned int reg)
{
    unsigned int key = target << 16 | reg, lo = 0, hi = RKNPU_RC_NALLOWED;

    while (lo < hi) {
        unsigned int mid = (lo + hi) / 2;

        if (rknpu_rc_allowed[mid] == key)
            return 1;
        if (rknpu_rc_allowed[mid] < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}
#endif

/* rknpu_rc_check_stream() results */
#define RKNPU_RC_OK             0
#define RKNPU_RC_BAD_PC         1       /* an entry writes the PC block */
#define RKNPU_RC_BAD_ADDR       2       /* an address register outside the caller's buffers */
#define RKNPU_RC_UNLISTED       3       /* a register off the list (M4) */

/* rknpu_rc_check_stream() flags */
#define RKNPU_RC_ALLOW_UNLISTED 0x1u    /* Device Parameters\M4AllowUnlisted */

RKNPU_RC_INLINE const char *rknpu_rc_why(int why)
{
    return why == RKNPU_RC_BAD_PC ? "writes the PC block" :
           why == RKNPU_RC_BAD_ADDR ? "address outside the caller's buffers" :
           why == RKNPU_RC_UNLISTED ? "a register charsiu never writes" : "ok";
}

/*
 * The rules above over `n` entries (every entry the PC fetches). An all-zero
 * entry is padding and passes. in_bo(ctx, addr) says whether addr lies in
 * one of the caller's buffers. *bad is the first refused entry; with
 * RKNPU_RC_ALLOW_UNLISTED, *unlisted (if not NULL) counts the entries off the
 * list that were let through. The driver (m3.c), the host model of it
 * (port/test/xport_fake.c) and m3gen all call this one function.
 */
typedef int (*rknpu_rc_in_bo_fn)(void *ctx, unsigned int addr);

RKNPU_RC_INLINE int rknpu_rc_check_stream_ex(const unsigned long long *ent, unsigned int n,
                                             unsigned int flags, rknpu_rc_in_bo_fn in_bo, void *ctx,
                                             unsigned int *bad, unsigned int *unlisted)
{
    unsigned int i;

    *bad = 0;
    (void)flags;
    (void)unlisted;
    for (i = 0; i < n; i++) {
        unsigned long long e = ent[i];
        unsigned int reg = RKNPU_RC_REG(e), v = RKNPU_RC_VALUE(e);

        if (e == 0)
            continue;
        if (rknpu_rc_is_pc_reg(reg)) {
            *bad = i;
            return RKNPU_RC_BAD_PC;
        }
#ifndef RKNPU_RC_NO_ALLOWLIST
        if (!rknpu_rc_is_listed(RKNPU_RC_TARGET(e), reg)) {
            if (!(flags & RKNPU_RC_ALLOW_UNLISTED)) {
                *bad = i;
                return RKNPU_RC_UNLISTED;
            }
            if (unlisted)
                (*unlisted)++;
        }
#endif
        if (rknpu_rc_is_addr_reg(reg) && v != 0 && !in_bo(ctx, v)) {
            *bad = i;
            return RKNPU_RC_BAD_ADDR;
        }
    }
    return RKNPU_RC_OK;
}

RKNPU_RC_INLINE int rknpu_rc_check_stream(const unsigned long long *ent, unsigned int n, unsigned int flags,
                                          rknpu_rc_in_bo_fn in_bo, void *ctx, unsigned int *bad)
{
    return rknpu_rc_check_stream_ex(ent, n, flags, in_bo, ctx, bad, 0);
}

/*
 * M6: THE SHARED CBUF. The two NPU cores share one CBUF, and two jobs that run
 * at the same time corrupt each other when their streams use the same part
 * of it and are exact when they use different parts (charsiu src/job.c,
 * rounds 362 and 363; charsiu_job.cbuf_window). Which part a stream uses is
 * CNA 0x1040: 0x10000000 for window 0, 0x2c001c00 for window 1 (the vendor's
 * two copies of every op differ in six registers, and this one carries both
 * the data and the weight start). The driver never lets two jobs with the
 * same key run together (m3.c), and treats a stream with no 0x1040, or a job
 * whose tasks disagree, as colliding with everything.
 */
#define RKNPU_RC_CNA_CBUF_CON1      0x1040u
#define RKNPU_RC_CBUF_ANY           0xFFFFFFFFu     /* unknown: collides with every job */

/* the stream's last CNA 0x1040 value, or RKNPU_RC_CBUF_ANY */
RKNPU_RC_INLINE unsigned int rknpu_rc_cbuf_key(const unsigned long long *ent, unsigned int n)
{
    unsigned int i, key = RKNPU_RC_CBUF_ANY;

    for (i = 0; i < n; i++)
        if (RKNPU_RC_TARGET(ent[i]) == RKNPU_RC_CNA && RKNPU_RC_REG(ent[i]) == RKNPU_RC_CNA_CBUF_CON1)
            key = RKNPU_RC_VALUE(ent[i]);
    return key;
}

/* a job's key from its tasks' keys: first is 1 for the first task */
RKNPU_RC_INLINE unsigned int rknpu_rc_cbuf_merge(unsigned int acc, unsigned int key, int first)
{
    return first ? key : (acc == key ? acc : RKNPU_RC_CBUF_ANY);
}

RKNPU_RC_INLINE int rknpu_rc_cbuf_collide(unsigned int a, unsigned int b)
{
    return a == RKNPU_RC_CBUF_ANY || b == RKNPU_RC_CBUF_ANY || a == b;
}

#endif /* RKNPU_REGCMD_H */
