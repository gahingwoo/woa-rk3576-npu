/*
 * fakedev.c - a stand-in for rknpu.sys, for `rknputest m3` on a Linux host
 *
 * Built only into the CI host binary (-DRKNPU_FAKE_DEVICE). It keeps the
 * buffers in memory, and SUBMIT does the matmul the stream describes in C:
 * it reads the input, weight and output addresses and K and N out of the
 * stream (CNA 0x1088, 0x1110, 0x107c; DPU 0x4018; CORE 0x3020) and the data
 * in the packed layouts m3_gemm.h documents. That exercises the tool's
 * buffer plumbing (slots, relocations, canaries, readback) and checks the
 * packing against an independent reading of it; it says nothing about the
 * NPU itself.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rknpu_ioctl.h"
#include "rknpu_regcmd.h"
#include "fakedev.h"

#define FAKE_MAX_BO 32

static struct {
    unsigned int handle, addr, size;
    unsigned char *mem;
} bo[FAKE_MAX_BO];
static unsigned int next_addr = 0x40100000u, next_handle;
static unsigned long long seq;

static unsigned char *at(unsigned int addr, unsigned int len)
{
    int i;

    for (i = 0; i < FAKE_MAX_BO; i++)
        if (bo[i].handle && addr >= bo[i].addr && (unsigned long long)addr + len <= (unsigned long long)bo[i].addr + bo[i].size)
            return bo[i].mem + (addr - bo[i].addr);
    return NULL;
}

static int find(unsigned int handle)
{
    int i;

    for (i = 0; i < FAKE_MAX_BO; i++)
        if (handle && bo[i].handle == handle)
            return i;
    return -1;
}

static int run_task(const rknpu_task *t)
{
    const unsigned char *s = at(t->regcmd, t->regcmd_count * 8u);
    unsigned int i, c, in = 0, w = 0, out = 0, ke = 0, n = 0;
    const unsigned char *A, *W;
    unsigned char *O;

    if (!s)
        return -1;
    for (i = 0; i < t->regcmd_count; i++) {
        unsigned long long e;

        memcpy(&e, s + (size_t)i * 8, 8);
        switch (RKNPU_RC_REG(e)) {
        case RKNPU_RC_CNA_FEATURE_ADDR: in = RKNPU_RC_VALUE(e); break;
        case RKNPU_RC_CNA_WEIGHT_ADDR:  w = RKNPU_RC_VALUE(e); break;
        case RKNPU_RC_DPU_DST_ADDR:     out = RKNPU_RC_VALUE(e); break;
        case 0x107c:                    ke = RKNPU_RC_VALUE(e) + 1; break;
        case 0x3020:                    n = RKNPU_RC_VALUE(e) + 1; break;
        default: break;
        }
    }
    A = at(in, ke);
    W = at(w, ke * n);
    O = at(out, n * 4);
    if (!A || !W || !O || !ke || !n) {
        fprintf(stderr, "fakedev: stream addresses or shape not found\n");
        return -1;
    }
    for (c = 0; c < n; c++) {
        int acc = 0;
        unsigned int c0 = c / 32 * 32, tn = n - c0 < 32 ? n - c0 : 32;

        for (i = 0; i < ke; i++) {
            unsigned int i0 = i / 32 * 32, tk = ke - i0 < 32 ? ke - i0 : 32;
            size_t wi = (size_t)c0 * ke + (size_t)i0 * tn + (size_t)(c - c0) * tk + (i - i0);

            acc += (int)(signed char)A[i] * (int)(signed char)W[wi];
        }
        memcpy(O + (size_t)c * 4, &acc, 4);
    }
    return 0;
}

int fake_ioctl(unsigned int code, const void *inb, unsigned int in_len, void *outb, unsigned int out_len)
{
    int i;

    (void)in_len;
    switch (code) {
    case IOCTL_RKNPU_GET_INFO: {
        rknpu_info *o = (rknpu_info *)outb;

        if (out_len < sizeof(*o))
            return -1;
        memset(o, 0, sizeof(*o));
        o->interface_version = RKNPU_INTERFACE_VERSION;
        o->struct_size = sizeof(*o);
        o->flags = RKNPU_INFO_INTERRUPTS_CONNECTED | RKNPU_INFO_DOMAINS_ON;
        o->core_count = RKNPU_NUM_CORES;
        o->m3_version = RKNPU_M3_VERSION;
        return 0;
    }
    case IOCTL_RKNPU_CREATE_BO: {
        rknpu_create_bo c;

        memcpy(&c, inb, sizeof(c));
        for (i = 0; i < FAKE_MAX_BO && bo[i].handle; i++)
            ;
        if (i == FAKE_MAX_BO || c.size == 0 || c.size > RKNPU_BO_MAX_SIZE)
            return -1;
        c.size = (c.size + 4095u) & ~4095u;
        bo[i].mem = (unsigned char *)calloc(1, c.size);
        if (!bo[i].mem)
            return -1;
        bo[i].handle = ++next_handle;
        bo[i].addr = next_addr;
        bo[i].size = c.size;
        next_addr += c.size + 0x10000u;
        c.handle = bo[i].handle;
        c.dma_address = bo[i].addr;
        memcpy(outb, &c, sizeof(c));
        return 0;
    }
    case IOCTL_RKNPU_FREE_BO: {
        i = find(((const rknpu_bo_handle *)inb)->handle);
        if (i < 0)
            return -1;
        free(bo[i].mem);
        memset(&bo[i], 0, sizeof(bo[i]));
        return 0;
    }
    case IOCTL_RKNPU_WRITE_BO:
    case IOCTL_RKNPU_READ_BO: {
        const rknpu_bo_io *io = (const rknpu_bo_io *)inb;

        i = find(io->handle);
        if (i < 0 || (unsigned long long)io->offset + io->length > bo[i].size || out_len < io->length)
            return -1;
        if (code == IOCTL_RKNPU_WRITE_BO)
            memcpy(bo[i].mem + io->offset, outb, io->length);
        else
            memcpy(outb, bo[i].mem + io->offset, io->length);
        return 0;
    }
    case IOCTL_RKNPU_MAP_BO: {
        rknpu_map_bo *m = (rknpu_map_bo *)outb;

        i = find(((const rknpu_map_bo *)inb)->handle);
        if (i < 0)
            return -1;
        m->user_va = (unsigned long long)(size_t)bo[i].mem;
        return 0;
    }
    case IOCTL_RKNPU_UNMAP_BO:
        return find(((const rknpu_bo_handle *)inb)->handle) < 0 ? -1 : 0;
    case IOCTL_RKNPU_SUBMIT: {
        const rknpu_submit_in *s = (const rknpu_submit_in *)inb;
        rknpu_submit_out o;
        unsigned int t;

        if (s->core >= RKNPU_NUM_CORES || s->task_count == 0 || s->task_count > RKNPU_MAX_TASKS)
            return -1;
        for (t = 0; t < s->task_count; t++)
            if (run_task(&s->tasks[t]))
                return -1;
        memset(&o, 0, sizeof(o));
        o.seqno = ++seq;
        o.core = s->core;
        memcpy(outb, &o, sizeof(o));
        return 0;
    }
    case IOCTL_RKNPU_WAIT: {
        rknpu_wait_out o;

        memset(&o, 0, sizeof(o));
        o.seqno = ((const rknpu_wait_in *)inb)->seqno;
        o.result = o.seqno == seq ? RKNPU_JOB_DONE : RKNPU_JOB_UNKNOWN;
        o.hw_us = 1;
        memcpy(outb, &o, sizeof(o));
        return 0;
    }
    case IOCTL_RKNPU_RESET: {
        rknpu_reset_out o;

        memset(&o, 0, sizeof(o));
        o.version_after = 0x46495245u;
        memcpy(outb, &o, sizeof(o));
        return 0;
    }
    default:
        return -1;
    }
}
