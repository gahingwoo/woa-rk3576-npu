/*
 * m3gen - capture charsiu's int8 matmul streams for rknputest m3, and check
 *         rknputest's packing against charsiu's
 *
 *   m3gen header    print tools/rknputest/m3_streams.h
 *   m3gen regs      print include/rknpu_regcmd_allow.h (M4): every register
 *                   charsiu_emit_job() writes, over the grid of job shapes
 *                   and types below
 *   m3gen check     compare tools/rknputest/m3_gemm.h with charsiu, byte for
 *                   byte, and check the streams against include/rknpu_regcmd.h;
 *                   then, over the grid, that every entry is on the list,
 *                   that the entries that move with a buffer are exactly the
 *                   address registers rknpu_regcmd.h names, and that every
 *                   stream passes rknpu_rc_check_stream() as SUBMIT runs it
 *
 * Built on Linux with gcc against external/charsiu src/job.c and
 * src/regcmd.c (see Makefile). Run with no CHARSIU_* variable in the
 * environment: charsiu reads dozens of them as experiment switches, and a
 * stream captured under one would not be the default stream. m3gen refuses
 * to run if any is set.
 *
 * The stream is the one charsiu's tools/npu_gemm_test.c run() submits for
 * its M = 1 control: int8 weights and activations, zero points 128, scales
 * 1, acc_out = 1 (the raw int32 accumulator), the default CBUF window.
 *
 * Addresses are found, not assumed: each of the four buffers is moved on
 * its own and the entries whose value moves by the same amount are that
 * buffer's relocations. The capture is then re-checked against charsiu at
 * random addresses.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "charsiu.h"
#include "m3_gemm.h"

extern char **environ;

#ifndef CHARSIU_COMMIT
#define CHARSIU_COMMIT "unknown"
#endif

/* The shapes m3_streams.h carries; rknputest m3 runs from these. */
static const unsigned int gen_shapes[][2] = {
    { 64, 64 },         /* charsiu lab notebook: M=1 K=64 N=64, exact on the board */
    { 256, 64 },        /* npu_gemm_test's default K and N */
    { 256, 256 },
    { 1024, 1024 },
};
#define NGEN (sizeof(gen_shapes) / sizeof(gen_shapes[0]))

#define MAXREG 4096

static void job_init(struct charsiu_job *job, unsigned int k, unsigned int n, const unsigned int base[M3_NBUF])
{
    memset(job, 0, sizeof(*job));
    job->cbuf_window = (unsigned)charsiu_cbuf_window();
    job->mm.m = 1;
    job->mm.k = k;
    job->mm.n = n;
    job->mm.wdtype = CHARSIU_INT8;
    job->mm.adtype = CHARSIU_INT8;
    job->input_zero_point = 128;
    job->weight_zero_point = 128;
    job->output_zero_point = 0;
    job->input_scale = 1.0f;
    job->weight_scale = 1.0f;
    job->output_scale = 1.0f;
    job->acc_out = 1;
    job->input_addr = base[M3_BUF_INPUT];
    job->weight_addr = base[M3_BUF_WEIGHT];
    job->output_addr = base[M3_BUF_OUTPUT];
    job->coef_addr = base[M3_BUF_COEF];
}

static size_t emit(unsigned int k, unsigned int n, const unsigned int base[M3_NBUF], uint64_t *out)
{
    struct charsiu_job job;

    job_init(&job, k, n, base);
    return charsiu_emit_job(&job, out, MAXREG);
}

struct capture {
    unsigned int count;
    uint64_t e[MAXREG];
    unsigned int nrel;
    struct m3_reloc rel[16];
};

static int capture(unsigned int k, unsigned int n, struct capture *c)
{
    static const unsigned int base0[M3_NBUF] = { 0x41000000u, 0x52000000u, 0x63000000u, 0x74000000u };
    static uint64_t moved[MAXREG];
    unsigned int b, i, j;
    size_t cnt;

    memset(c, 0, sizeof(*c));
    cnt = emit(k, n, base0, c->e);
    if (cnt == 0 || cnt >= MAXREG) {
        fprintf(stderr, "K=%u N=%u: charsiu emitted no stream\n", k, n);
        return -1;
    }
    c->count = (unsigned int)cnt;
    for (b = 0; b < M3_NBUF; b++) {
        unsigned int base1[M3_NBUF];
        const unsigned int delta = 0x00350000u + b * 0x1000u;

        memcpy(base1, base0, sizeof(base1));
        base1[b] += delta;
        if (emit(k, n, base1, moved) != cnt) {
            fprintf(stderr, "K=%u N=%u: the entry count depends on an address\n", k, n);
            return -1;
        }
        for (i = 0; i < cnt; i++) {
            if (moved[i] == c->e[i])
                continue;
            if (RKNPU_RC_TARGET(moved[i]) != RKNPU_RC_TARGET(c->e[i]) ||
                RKNPU_RC_REG(moved[i]) != RKNPU_RC_REG(c->e[i]) ||
                RKNPU_RC_VALUE(moved[i]) - RKNPU_RC_VALUE(c->e[i]) != delta) {
                fprintf(stderr, "K=%u N=%u: entry %u moves with buffer %u but not as an address\n", k, n, i, b);
                return -1;
            }
            for (j = 0; j < c->nrel; j++)
                if (c->rel[j].index == i) {
                    fprintf(stderr, "K=%u N=%u: entry %u moves with two buffers\n", k, n, i);
                    return -1;
                }
            if (c->nrel == 16)
                return -1;
            c->rel[c->nrel].index = i;
            c->rel[c->nrel].buf = b;
            c->rel[c->nrel].offset = RKNPU_RC_VALUE(c->e[i]) - base0[b];
            c->nrel++;
        }
    }
    /* the template carries 0 + offset in every relocated entry */
    for (j = 0; j < c->nrel; j++) {
        uint64_t e = c->e[c->rel[j].index];
        c->e[c->rel[j].index] = RKNPU_RC_ENTRY(RKNPU_RC_TARGET(e), RKNPU_RC_REG(e), c->rel[j].offset);
    }
    return 0;
}

static unsigned int rnd(void)
{
    static unsigned int s = 0x12345678u;

    s = s * 1103515245u + 12345u;
    return s;
}

/* The rules SUBMIT enforces, and the relocations exactly the address registers. */
static int check_rules(unsigned int k, unsigned int n, const struct capture *c)
{
    unsigned int i, j;
    int bad = 0;

    for (i = 0; i < c->count; i++) {
        unsigned int reg = RKNPU_RC_REG(c->e[i]);
        int isrel = 0;

        for (j = 0; j < c->nrel; j++)
            if (c->rel[j].index == i)
                isrel = 1;
        if (c->e[i] != 0 && rknpu_rc_is_pc_reg(reg)) {
            fprintf(stderr, "K=%u N=%u: entry %u writes the PC block (0x%04x)\n", k, n, i, reg);
            bad = 1;
        }
        if (isrel != rknpu_rc_is_addr_reg(reg) && !(rknpu_rc_is_addr_reg(reg) && RKNPU_RC_VALUE(c->e[i]) == 0)) {
            fprintf(stderr, "K=%u N=%u: entry %u (reg 0x%04x) is %s an address register in rknpu_regcmd.h "
                    "but %s with a buffer\n", k, n, i, reg, rknpu_rc_is_addr_reg(reg) ? "" : "not",
                    isrel ? "moves" : "does not move");
            bad = 1;
        }
    }
    return bad;
}

#ifndef M3_NO_STREAMS
static int check_stream(unsigned int k, unsigned int n)
{
    const struct m3_shape *s = m3_find_shape(k, n);
    static uint64_t want[MAXREG];
    static unsigned long long got[MAXREG];
    unsigned int t, base[M3_NBUF], b;
    size_t cnt;

    if (!s) {
        fprintf(stderr, "K=%u N=%u: not in m3_streams.h (run m3gen header)\n", k, n);
        return 1;
    }
    for (t = 0; t < 64; t++) {
        for (b = 0; b < M3_NBUF; b++)
            base[b] = 0x40000000u + (rnd() & 0x7FFFF000u) % 0xB0000000u;
        cnt = emit(k, n, base, want);
        if (cnt != s->count) {
            fprintf(stderr, "K=%u N=%u: charsiu emits %zu entries, m3_streams.h has %u\n", k, n, cnt, s->count);
            return 1;
        }
        m3_build_stream(s, base, got);
        if (memcmp(want, got, cnt * 8)) {
            fprintf(stderr, "K=%u N=%u: the rebuilt stream differs from charsiu's\n", k, n);
            return 1;
        }
    }
    return 0;
}
#endif

static int check_pack(unsigned int k, unsigned int n)
{
    struct charsiu_matmul mm;
    struct charsiu_job job;
    static const unsigned int zero_base[M3_NBUF];
    size_t wb, cb, insz;
    unsigned char *B, *A, *w1, *w2, *i1, *i2, *c1, *c2;
    int32_t *z;
    unsigned int i;
    int bad = 0;

    memset(&mm, 0, sizeof(mm));
    mm.m = 1;
    mm.k = k;
    mm.n = n;
    mm.wdtype = CHARSIU_INT8;
    mm.adtype = CHARSIU_INT8;
    wb = charsiu_weight_bytes(&mm);
    job_init(&job, k, n, zero_base);
    cb = charsiu_coef_bytes(&job.mm);
    insz = (size_t)charsiu_entries_per_row(&mm) * 64 + 4096;

    if (wb != m3_weight_bytes(k, n) || cb != m3_coef_bytes(n) ||
        (size_t)charsiu_entries_per_row(&mm) * 64 != m3_input_row_bytes(k)) {
        fprintf(stderr, "K=%u N=%u: sizes differ: weights %zu/%zu coefs %zu/%zu input row %u/%zu\n", k, n,
                wb, m3_weight_bytes(k, n), cb, m3_coef_bytes(n), charsiu_entries_per_row(&mm) * 64,
                m3_input_row_bytes(k));
        return 1;
    }
    B = malloc((size_t)n * k);
    A = malloc(k);
    w1 = malloc(wb); w2 = malloc(wb);
    i1 = calloc(1, insz); i2 = malloc(insz);
    c1 = malloc(cb); c2 = malloc(cb);
    z = calloc(n, sizeof(*z));
    if (!B || !A || !w1 || !w2 || !i1 || !i2 || !c1 || !c2 || !z)
        return 1;
    for (i = 0; i < n * k; i++)
        B[i] = (unsigned char)rnd();
    for (i = 0; i < k; i++)
        A[i] = (unsigned char)(rnd() >> 8);

    charsiu_pack_weights(&mm, B, w1);
    m3_pack_weights(k, n, B, w2);
    if (memcmp(w1, w2, wb)) {
        fprintf(stderr, "K=%u N=%u: weight packing differs\n", k, n);
        bad = 1;
    }
    charsiu_pack_input(&mm, A, i1, insz, 128);
    m3_pack_input(k, A, i2, insz);
    if (memcmp(i1, i2, insz)) {
        fprintf(stderr, "K=%u N=%u: input packing differs\n", k, n);
        bad = 1;
    }
    charsiu_build_coefs(&job, z, z, c1);
    m3_build_coefs(n, c2);
    if (memcmp(c1, c2, cb)) {
        fprintf(stderr, "K=%u N=%u: coefficients differ\n", k, n);
        bad = 1;
    }
    free(B); free(A); free(w1); free(w2); free(i1); free(i2); free(c1); free(c2); free(z);
    return bad;
}

/* ---- M4: the register list, over every kind of job charsiu emits ------- */

/*
 * The grid. Weight and activation types in every combination the emitter
 * accepts (int8/int8 is the M3 matmul, int4/fp16 the w4a16 default, fp16/fp16
 * the attention), rows 1 (decode) to 80 (a prefill chunk), and both CBUF
 * windows, which the two devices take. A Llama-3.2-1B run through the host
 * model of the driver (port/test) writes 147 registers; this grid must write
 * the same 147, and CI says so if charsiu ever writes one more.
 */
static const enum charsiu_dtype grid_w[] = { CHARSIU_INT8, CHARSIU_INT4, CHARSIU_FP16 };
static const enum charsiu_dtype grid_a[] = { CHARSIU_INT8, CHARSIU_FP16 };
static const unsigned int grid_m[] = { 1, 2, 16, 80 };
static const unsigned int grid_kn[][2] = {
    { 64, 64 }, { 2048, 512 }, { 1024, 2048 }, { 1024, 8192 }, { 8192, 2048 }, { 64, 1024 },
};

#define MAXKEYS 1024

struct gridjob {
    struct charsiu_job job;
    unsigned int base[M3_NBUF];
};

static void grid_set(struct charsiu_job *job, const unsigned int base[M3_NBUF])
{
    job->input_addr = base[M3_BUF_INPUT];
    job->weight_addr = base[M3_BUF_WEIGHT];
    job->output_addr = base[M3_BUF_OUTPUT];
    job->coef_addr = base[M3_BUF_COEF];
}

/* the next job of the grid into *g; 0 when done */
static int grid_next(unsigned int *at, struct charsiu_job *job)
{
    unsigned int nw = sizeof(grid_w) / sizeof(grid_w[0]), na = sizeof(grid_a) / sizeof(grid_a[0]);
    unsigned int nm = sizeof(grid_m) / sizeof(grid_m[0]), nkn = sizeof(grid_kn) / sizeof(grid_kn[0]);
    unsigned int i = *at, w, a, m, kn, win, acc;

    if (i >= nw * na * nm * nkn * 2 * 2)
        return 0;
    (*at)++;
    acc = i % 2; i /= 2;
    win = i % 2; i /= 2;
    kn = i % nkn; i /= nkn;
    m = i % nm; i /= nm;
    a = i % na; i /= na;
    w = i % nw;
    memset(job, 0, sizeof(*job));
    job->cbuf_window = win;
    job->mm.m = grid_m[m];
    job->mm.k = grid_kn[kn][0];
    job->mm.n = grid_kn[kn][1];
    job->mm.wdtype = grid_w[w];
    job->mm.adtype = grid_a[a];
    job->input_zero_point = grid_a[a] == CHARSIU_INT8 ? 128 : 0;
    job->weight_zero_point = grid_w[w] == CHARSIU_FP16 ? 0 : 128;
    job->output_zero_point = 0;
    job->input_scale = 1.0f;
    job->weight_scale = 1.0f;
    job->output_scale = 1.0f;
    job->acc_out = (int)acc;
    return 1;
}

static int key_cmp(const void *a, const void *b)
{
    unsigned int x = *(const unsigned int *)a, y = *(const unsigned int *)b;

    return x < y ? -1 : x > y;
}

static void key_add(unsigned int *keys, unsigned int *n, unsigned int key)
{
    unsigned int i;

    for (i = 0; i < *n; i++)
        if (keys[i] == key)
            return;
    if (*n < MAXKEYS)
        keys[(*n)++] = key;
}

static const unsigned int grid_base0[M3_NBUF] = { 0x41000000u, 0x52000000u, 0x63000000u, 0x74000000u };

/* every (block, register) the grid writes, sorted; the count, or 0 */
static unsigned int grid_keys(unsigned int *keys, unsigned int *jobs)
{
    static uint64_t e[MAXREG];
    struct charsiu_job job;
    unsigned int at = 0, n = 0, i;
    size_t cnt;

    *jobs = 0;
    while (grid_next(&at, &job)) {
        grid_set(&job, grid_base0);
        cnt = charsiu_emit_job(&job, e, MAXREG);
        if (cnt == 0 || cnt >= MAXREG)
            continue;           /* a combination the emitter does not make */
        (*jobs)++;
        for (i = 0; i < cnt; i++)
            if (e[i])
                key_add(keys, &n, RKNPU_RC_TARGET(e[i]) << 16 | RKNPU_RC_REG(e[i]));
    }
    qsort(keys, n, sizeof(keys[0]), key_cmp);
    return n;
}

static int cmd_regs(void)
{
    static unsigned int keys[MAXKEYS];
    unsigned int n, jobs, i;

    n = grid_keys(keys, &jobs);
    if (!n)
        return 1;
    printf("/*\n * rknpu_regcmd_allow.h - GENERATED by tools/m3gen (`m3gen regs`); do not edit.\n *\n"
           " * Every (block, register) charsiu_emit_job() writes, from charsiu %s\n"
           " * (external/charsiu, GPL-2.0), over the %u jobs of m3gen's grid. Each\n"
           " * entry is the block (RKNPU_RC_TARGET) << 16 | the register offset.\n"
           " * SUBMIT refuses any other register (rknpu_regcmd.h). Sorted.\n"
           " * SPDX-License-Identifier: GPL-2.0-only\n */\n\n", CHARSIU_COMMIT, jobs);
    printf("#ifndef RKNPU_REGCMD_ALLOW_H\n#define RKNPU_REGCMD_ALLOW_H\n\n");
    printf("#define RKNPU_RC_NALLOWED %u\n\n", n);
    printf("static const unsigned int rknpu_rc_allowed[RKNPU_RC_NALLOWED] = {");
    for (i = 0; i < n; i++)
        printf("%s0x%08xu,", i % 6 ? " " : "\n    ", keys[i]);
    printf("\n};\n\n#endif /* RKNPU_REGCMD_ALLOW_H */\n");
    return 0;
}

#ifndef M3_NO_STREAMS
static const unsigned int *grid_ok_base;

static int grid_in_bo(void *ctx, unsigned int addr)
{
    unsigned int b;

    (void)ctx;
    for (b = 0; b < M3_NBUF; b++)
        if (addr >= grid_ok_base[b] && addr < grid_ok_base[b] + 0x00800000u)
            return 1;
    return 0;
}

/*
 * Over the grid: the list in the header is the list the grid writes; each
 * buffer moved alone moves exactly the address registers; every stream
 * passes the check SUBMIT runs, and fails it once its output is moved past
 * every buffer.
 */
static int check_grid(void)
{
    static unsigned int keys[MAXKEYS];
    static uint64_t e0[MAXREG], e1[MAXREG];
    struct charsiu_job job;
    unsigned int n, jobs, i, at = 0, b, bad = 0, why;
    size_t cnt;

    n = grid_keys(keys, &jobs);
    if (n != RKNPU_RC_NALLOWED || memcmp(keys, rknpu_rc_allowed, n * sizeof(keys[0]))) {
        fprintf(stderr, "grid: charsiu writes %u registers, rknpu_regcmd_allow.h lists %u "
                "(run m3gen regs)\n", n, RKNPU_RC_NALLOWED);
        return 1;
    }
    while (grid_next(&at, &job)) {
        grid_set(&job, grid_base0);
        cnt = charsiu_emit_job(&job, e0, MAXREG);
        if (cnt == 0 || cnt >= MAXREG)
            continue;
        grid_ok_base = grid_base0;
        if ((why = (unsigned)rknpu_rc_check_stream((const unsigned long long *)e0, (unsigned)cnt, 0,
                                                   grid_in_bo, NULL, &i)) != RKNPU_RC_OK) {
            fprintf(stderr, "grid: m%u k%u n%u w%d a%d: entry %u (0x%016llx) refused: %s\n",
                    job.mm.m, job.mm.k, job.mm.n, job.mm.wdtype, job.mm.adtype, i,
                    (unsigned long long)e0[i], rknpu_rc_why((int)why));
            bad = 1;
            continue;
        }
        for (b = 0; b < M3_NBUF; b++) {
            unsigned int base1[M3_NBUF];
            const unsigned int delta = 0x00350000u + b * 0x1000u;

            memcpy(base1, grid_base0, sizeof(base1));
            base1[b] += delta;
            grid_set(&job, base1);
            if (charsiu_emit_job(&job, e1, MAXREG) != cnt) {
                fprintf(stderr, "grid: the entry count depends on an address\n");
                return 1;
            }
            for (i = 0; i < cnt; i++) {
                int moved = e1[i] != e0[i];
                unsigned int reg = RKNPU_RC_REG(e0[i]);

                if (moved && (RKNPU_RC_VALUE(e1[i]) - RKNPU_RC_VALUE(e0[i]) != delta ||
                              !rknpu_rc_is_addr_reg(reg))) {
                    fprintf(stderr, "grid: m%u k%u n%u w%d a%d: entry %u (reg 0x%04x) moves with buffer %u "
                            "but is not an address register in rknpu_regcmd.h\n", job.mm.m, job.mm.k,
                            job.mm.n, job.mm.wdtype, job.mm.adtype, i, reg, b);
                    bad = 1;
                }
            }
            /* the output moved past every buffer must be refused */
            if (b == M3_BUF_OUTPUT) {
                unsigned int far[M3_NBUF];

                memcpy(far, grid_base0, sizeof(far));
                far[M3_BUF_OUTPUT] = 0xF0000000u;
                grid_set(&job, far);
                charsiu_emit_job(&job, e1, MAXREG);
                if (rknpu_rc_check_stream((const unsigned long long *)e1, (unsigned)cnt, 0,
                                          grid_in_bo, NULL, &i) != RKNPU_RC_BAD_ADDR) {
                    fprintf(stderr, "grid: an output outside every buffer was not refused\n");
                    bad = 1;
                }
            }
        }
    }
    printf("grid: %u jobs, %u registers, all on the list, the address registers exactly %s\n",
           jobs, n, bad ? "NOT" : "rknpu_regcmd.h's");
    return (int)bad;
}
#endif

static int env_clean(void)
{
    char **e;

    for (e = environ; *e; e++)
        if (!strncmp(*e, "CHARSIU_", 8)) {
            fprintf(stderr, "m3gen: %s is set; run with no CHARSIU_* variable\n", *e);
            return 0;
        }
    return 1;
}

static int cmd_header(void)
{
    static struct capture c;
    unsigned int s, i;

    printf("/*\n * m3_streams.h - GENERATED by tools/m3gen (`m3gen header`); do not edit.\n *\n"
           " * charsiu_emit_job() int8 matmul streams, M = 1, acc_out, from charsiu %s\n"
           " * (external/charsiu, GPL-2.0). Every address is 0 + the offset in its\n"
           " * relocation; m3_build_stream() adds the buffer's NPU address.\n"
           " * SPDX-License-Identifier: GPL-2.0-only\n */\n\n", CHARSIU_COMMIT);
    printf("#ifndef M3_STREAMS_H\n#define M3_STREAMS_H\n\n");
    for (s = 0; s < NGEN; s++) {
        unsigned int k = gen_shapes[s][0], n = gen_shapes[s][1];

        if (capture(k, n, &c) || check_rules(k, n, &c))
            return 1;
        printf("static const unsigned long long m3_k%u_n%u_entries[%u] = {", k, n, c.count);
        for (i = 0; i < c.count; i++)
            printf("%s0x%016llxull,", i % 3 ? " " : "\n    ", (unsigned long long)c.e[i]);
        printf("\n};\n\nstatic const struct m3_reloc m3_k%u_n%u_rel[%u] = {\n", k, n, c.nrel);
        for (i = 0; i < c.nrel; i++)
            printf("    { %u, %u, 0x%x },\n", c.rel[i].index, c.rel[i].buf, c.rel[i].offset);
        printf("};\n\n");
    }
    printf("static const struct m3_shape m3_shapes[%u] = {\n", (unsigned)NGEN);
    for (s = 0; s < NGEN; s++) {
        unsigned int k = gen_shapes[s][0], n = gen_shapes[s][1];

        capture(k, n, &c);
        printf("    { %u, %u, %u, m3_k%u_n%u_entries, %u, m3_k%u_n%u_rel },\n", k, n, c.count, k, n, c.nrel, k, n);
    }
    printf("};\n\n#endif /* M3_STREAMS_H */\n");
    return 0;
}

static int cmd_check(void)
{
    /* the generated shapes, plus others the formulas must also hold for */
    static const unsigned int extra[][2] = {
        { 16, 16 }, { 32, 48 }, { 48, 32 }, { 80, 112 }, { 112, 80 }, { 512, 1024 }, { 1024, 256 },
        { 2048, 64 }, { 160, 96 },
    };
    unsigned int s;
    int bad = 0;

    for (s = 0; s < NGEN; s++) {
        static struct capture c;

        if (capture(gen_shapes[s][0], gen_shapes[s][1], &c) || check_rules(gen_shapes[s][0], gen_shapes[s][1], &c))
            bad = 1;
#ifndef M3_NO_STREAMS
        bad |= check_stream(gen_shapes[s][0], gen_shapes[s][1]);
#endif
        bad |= check_pack(gen_shapes[s][0], gen_shapes[s][1]);
    }
    for (s = 0; s < sizeof(extra) / sizeof(extra[0]); s++)
        bad |= check_pack(extra[s][0], extra[s][1]);
#ifndef M3_NO_STREAMS
    bad |= check_grid();
#endif
    printf("%s\n", bad ? "M3GEN CHECK FAILED" : "M3GEN CHECK OK");
    return bad;
}

int main(int argc, char **argv)
{
    if (!env_clean())
        return 2;
    if (argc == 2 && !strcmp(argv[1], "header"))
        return cmd_header();
    if (argc == 2 && !strcmp(argv[1], "regs"))
        return cmd_regs();
    if (argc == 2 && !strcmp(argv[1], "check"))
        return cmd_check();
    fprintf(stderr, "usage: m3gen header | regs | check\n");
    return 2;
}
