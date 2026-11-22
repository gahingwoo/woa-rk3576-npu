/*
 * m3_gemm.h - the int8 matmul rknputest m3 runs, without charsiu
 *
 * One task computes one row of C = A * B^T, M = 1: A is 1 x K, B is N x K,
 * both uint8 with zero point 128, and C is N raw int32 accumulators,
 *
 *     C[c] = sum_i (A[i] - 128) * (B[c][i] - 128)
 *
 * which is exactly what charsiu's tools/npu_gemm_test.c asks of the NPU
 * (its run() and reference()) for its M = 1 control. A GEMM with M rows is
 * M such tasks in one job: the same weights, the same coefficients, one
 * input row and one output row each.
 *
 * WHY M = 1 PER TASK. charsiu's lab notebook (docs/lab-notebook.md) records
 * the raw int32 accumulator path (acc_out) as exact at one row and never
 * correct above it; the M > 1 output layout is the open question its
 * npu_gemm_test exists for. M3 is about the driver, so it uses only the
 * shape charsiu has seen right on the board under Linux.
 *
 * WHERE EACH PIECE COMES FROM. The register stream is charsiu's own
 * charsiu_emit_job() output (src/job.c), captured by tools/m3gen into
 * m3_streams.h with the buffer addresses taken out, and put back here. The
 * packing and the coefficients below are plain formulas; tools/m3gen
 * compares every byte of them with charsiu_pack_weights(),
 * charsiu_pack_input() and charsiu_build_coefs() (src/regcmd.c, src/job.c)
 * for every shape it lists, and CI runs that comparison on each push, so
 * this file cannot drift from charsiu silently. charsiu is GPL-2.0, a
 * submodule at external/charsiu; nothing of it is compiled into rknputest.
 *
 *   weights   [N/32][K/32][N%32][K%32], each tile cut short at the edges,
 *             K padded to 16, every byte stored as b - 128 (b ^ 0x80):
 *             charsiu.h charsiu_weight_ngroup/kgroup (32, 32 for int8).
 *   input     K bytes in order, each a - 128, zero after (M = 1 collapses
 *             charsiu's [K/atom][M][atom] to that).
 *   coefs     per 8 output channels 64 bytes: int32 128 (charsiu's lift,
 *             128 / (in_scale * w_scale / out_scale)), 16 bytes of 0, int16
 *             16 (CHARSIU_COEF_C's default); then N fp16 1.0; then the word
 *             0x00001004; the whole buffer charsiu_coef_bytes() long.
 *   output    N int32 in order: charsiu's [n/atom][m][n%atom] at m = 1.
 *
 * Only K and N that are multiples of 16 are used (rknputest refuses
 * others): that keeps every formula above free of the odd-N and K-padding
 * edge cases charsiu handles and M3 does not need.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef M3_GEMM_H
#define M3_GEMM_H

#include <stddef.h>
#include <string.h>

#include "rknpu_regcmd.h"

enum { M3_BUF_INPUT, M3_BUF_WEIGHT, M3_BUF_OUTPUT, M3_BUF_COEF, M3_NBUF };

/* One relocation: entry `index` carries buffer `buf`'s address + `offset`. */
struct m3_reloc {
    unsigned int index;
    unsigned int buf;
    unsigned int offset;
};

/* One captured stream (m3_streams.h): its entries with every address at 0. */
struct m3_shape {
    unsigned int k, n;
    unsigned int count;
    const unsigned long long *entries;
    unsigned int nrel;
    const struct m3_reloc *rel;
};

#define M3_ALIGN(x, a)          (((x) + (a) - 1) / (a) * (a))
#define M3_COEF_ELEMS           65536u      /* charsiu_coef_bytes(): CHARSIU_COEF_ELEMS default */

/* tools/m3gen builds once without the streams, to generate them */
#ifndef M3_NO_STREAMS
#include "m3_streams.h"

RKNPU_RC_INLINE const struct m3_shape *m3_find_shape(unsigned int k, unsigned int n)
{
    unsigned int i;

    for (i = 0; i < sizeof(m3_shapes) / sizeof(m3_shapes[0]); i++)
        if (m3_shapes[i].k == k && m3_shapes[i].n == n)
            return &m3_shapes[i];
    return NULL;
}
#endif

RKNPU_RC_INLINE size_t m3_weight_bytes(unsigned int k, unsigned int n)
{
    return (size_t)M3_ALIGN(k, 16) * M3_ALIGN(n, 2);
}

RKNPU_RC_INLINE size_t m3_coef_bytes(unsigned int n)
{
    return (size_t)M3_ALIGN(n, 8) / 8 * 64 + (size_t)M3_COEF_ELEMS * 4 + 0x100;
}

/* charsiu_entries_per_row() * 64 at int8: the CBUF entries one row reads. */
RKNPU_RC_INLINE size_t m3_input_row_bytes(unsigned int k)
{
    return (size_t)M3_ALIGN(k, 64);
}

/* dst holds m3_weight_bytes(k, n) bytes; B is N rows of K. */
RKNPU_RC_INLINE void m3_pack_weights(unsigned int k, unsigned int n, const unsigned char *B, unsigned char *dst)
{
    unsigned int ke = M3_ALIGN(k, 16);
    unsigned int c, i;

    memset(dst, 0, m3_weight_bytes(k, n));
    for (c = 0; c < n; c++) {
        unsigned int c0 = c / 32 * 32;
        unsigned int tn = n - c0 < 32 ? n - c0 : 32;         /* channels in this tile row */
        for (i = 0; i < k; i++) {
            unsigned int i0 = i / 32 * 32;
            unsigned int tk = ke - i0 < 32 ? ke - i0 : 32;   /* k in this tile */
            size_t at = (size_t)c0 * ke + (size_t)i0 * tn + (size_t)(c - c0) * tk + (i - i0);
            dst[at] = (unsigned char)(B[(size_t)c * k + i] ^ 0x80);
        }
    }
}

/* dst holds `len` bytes, len >= k; one row of A. */
RKNPU_RC_INLINE void m3_pack_input(unsigned int k, const unsigned char *a, unsigned char *dst, size_t len)
{
    unsigned int i;

    memset(dst, 0, len);
    for (i = 0; i < k; i++)
        dst[i] = (unsigned char)(a[i] ^ 0x80);
}

RKNPU_RC_INLINE void m3_put32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* dst holds m3_coef_bytes(n) bytes; n a multiple of 8. */
RKNPU_RC_INLINE void m3_build_coefs(unsigned int n, unsigned char *dst)
{
    size_t table = (size_t)n / 8 * 64;
    unsigned int oc;

    memset(dst, 0, m3_coef_bytes(n));
    for (oc = 0; oc < n; oc++) {
        unsigned char *g = dst + (size_t)(oc / 8) * 64;
        unsigned int i = oc % 8;

        m3_put32(g + i * 4, 128);                   /* int32 lift */
        g[48 + i * 2] = 16;                         /* int16 coef_c; byte 1 stays 0 */
        dst[table + (size_t)oc * 2]     = 0x00;     /* fp16 1.0 = 0x3c00 */
        dst[table + (size_t)oc * 2 + 1] = 0x3c;
    }
    m3_put32(dst + table + (size_t)n * 2, 0x00001004u);
}

/* The shape's stream with the four buffer addresses put in. */
RKNPU_RC_INLINE void m3_build_stream(const struct m3_shape *s, const unsigned int base[M3_NBUF], unsigned long long *out)
{
    unsigned int i;

    memcpy(out, s->entries, (size_t)s->count * sizeof(*out));
    for (i = 0; i < s->nrel; i++) {
        unsigned long long e = out[s->rel[i].index];
        unsigned int v = base[s->rel[i].buf] + s->rel[i].offset;

        out[s->rel[i].index] = RKNPU_RC_ENTRY(RKNPU_RC_TARGET(e), RKNPU_RC_REG(e), v);
    }
}

/* charsiu npu_gemm_test.c mix(): a byte near 128 depending on both indices. */
RKNPU_RC_INLINE unsigned char m3_mix(unsigned int a, unsigned int b, unsigned int span)
{
    unsigned int h = a * 2654435761u ^ (b + 0x9e3779b9u) * 40503u;

    h ^= h >> 13;
    return (unsigned char)(128 + (int)(h % span) - (int)(span / 2));
}

/* charsiu npu_gemm_test.c reference(), one row. */
RKNPU_RC_INLINE void m3_reference_row(unsigned int k, unsigned int n, const unsigned char *a, const unsigned char *B, int *out)
{
    unsigned int c, i;

    for (c = 0; c < n; c++) {
        int acc = 0;
        for (i = 0; i < k; i++)
            acc += ((int)a[i] - 128) * ((int)B[(size_t)c * k + i] - 128);
        out[c] = acc;
    }
}

#endif /* M3_GEMM_H */
