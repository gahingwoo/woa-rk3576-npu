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
 *   - every entry that writes one of the address registers below either
 *     writes 0 or an address inside one of the caller's buffers.
 *
 * The list is the address registers charsiu's int8 matmul stream writes on
 * RK3576, found by tools/m3gen by moving each buffer and seeing which
 * entries move with it (CI repeats that on every push). It is NOT every
 * address register the NPU has: RK3576 moved its registers away from the
 * RK3588 layout in drivers/accel/rocket/rocket_registers.h, and the rest of
 * them are not known well enough to list. A stream that programs an address
 * through a register not listed here passes the check unseen. This bounds
 * the mistakes rknputest and charsiu can make; it is not a sandbox, and the
 * device stays administrators-only.
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

/* Address registers, RK3576 offsets, as charsiu's int8 matmul stream uses them. */
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

#endif /* RKNPU_REGCMD_H */
