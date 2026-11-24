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
 * M6: thread safe, with a handle per thread (fake_open). A job stays running
 * for FAKE_JOB_US after its SUBMIT, until a WAIT has let that time pass, and
 * the driver's busy rules apply: a core with a job is busy, and while a
 * handle's job without RKNPU_SUBMIT_COPY runs, that handle's WRITE_BO and
 * MAP_BO are refused (STATUS_DEVICE_BUSY in the driver). Two threads driving
 * the two cores therefore overlap the way they do on the board, which is
 * where the M3/M4 rule ("any handle's job") refused the second handle.
 * FAKE_GLOBAL_BUSY=1 brings that old rule back, to see the test catch it.
 *
 * And the shared CBUF, which the second M6 board run met: the two cores
 * share it, so two jobs that run at once with the same CBUF window (CNA
 * 0x1040, rknpu_regcmd.h) both come out wrong on the hardware; here each
 * gets every output word changed. The driver makes such a job wait for the
 * other (m3.c), and so does this; FAKE_NO_CBUF_GUARD=1 drops the driver's
 * wait, to see the corruption the test must catch.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rknpu_ioctl.h"
#include "rknpu_regcmd.h"
#include "fakedev.h"

#define FAKE_MAX_BO 512

#define FAKE_JOB_US 2000

static struct {
    unsigned int handle, addr, size, owner;
    unsigned char *mem;
} bo[FAKE_MAX_BO];
static unsigned int next_addr = 0x40100000u, next_handle;
static unsigned long long seq;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static unsigned int next_owner;
static _Thread_local unsigned int cur;          /* this thread's handle; 0 = the first */

static struct {
    unsigned long long seq;
    unsigned int owner, copied, active, key, nout;
    double end_us;
    unsigned int out_addr[RKNPU_MAX_TASKS], out_n[RKNPU_MAX_TASKS];
} job[RKNPU_NUM_CORES];
static unsigned int cbuf_waits[RKNPU_NUM_CORES];

static double fake_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

void fake_open(void)
{
    pthread_mutex_lock(&lk);
    cur = ++next_owner;
    pthread_mutex_unlock(&lk);
}

/* the hardware still has the job: SUBMIT was less than FAKE_JOB_US ago */
static int running(int c)
{
    return job[c].active && fake_now_us() < job[c].end_us;
}

/* the driver's OwnerUncopiedJobActive(), or M3/M4's rule with FAKE_GLOBAL_BUSY */
static int writes_frozen(void)
{
    int c, global = getenv("FAKE_GLOBAL_BUSY") != NULL;

    for (c = 0; c < RKNPU_NUM_CORES; c++)
        if (running(c) && !job[c].copied && (global || job[c].owner == cur))
            return 1;
    return 0;
}

static unsigned char *at(unsigned int addr, unsigned int len)
{
    int i;

    for (i = 0; i < FAKE_MAX_BO; i++)
        if (bo[i].handle && bo[i].owner == cur && addr >= bo[i].addr && (unsigned long long)addr + len <= (unsigned long long)bo[i].addr + bo[i].size)
            return bo[i].mem + (addr - bo[i].addr);
    return NULL;
}

static int find(unsigned int handle)
{
    int i;

    for (i = 0; i < FAKE_MAX_BO; i++)
        if (handle && bo[i].handle == handle && bo[i].owner == cur)
            return i;
    return -1;
}

static unsigned int last_out_addr, last_out_n;

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
    last_out_addr = out;
    last_out_n = n;
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

static int fake_ioctl_locked(unsigned int code, const void *inb, unsigned int in_len, void *outb,
                             unsigned int out_len);

int fake_ioctl(unsigned int code, const void *inb, unsigned int in_len, void *outb, unsigned int out_len)
{
    int r;

    pthread_mutex_lock(&lk);
    r = fake_ioctl_locked(code, inb, in_len, outb, out_len);
    pthread_mutex_unlock(&lk);
    return r;
}

static int fake_ioctl_locked(unsigned int code, const void *inb, unsigned int in_len, void *outb,
                             unsigned int out_len)
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
        /* M6: answers as IOMMU mode, so `m6` runs; the addresses are made up
         * either way, and the buffers are not cached (no PREP/FINI needed) */
        o->m6_version = RKNPU_M6_VERSION;
        o->mem_mode = RKNPU_MEM_IOMMU;
        for (i = 0; i < RKNPU_NUM_CORES; i++)
            o->core[i].cbuf_waits = cbuf_waits[i];
        for (i = 0; i < FAKE_MAX_BO; i++)
            if (bo[i].handle) {
                o->bo_count++;
                o->bo_kib += bo[i].size >> 10;
            }
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
        bo[i].owner = cur;
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
        if (code == IOCTL_RKNPU_WRITE_BO && writes_frozen()) {
            fprintf(stderr, "fakedev: WRITE_BO refused, busy (a job's streams are frozen)\n");
            return -1;
        }
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
        if (writes_frozen()) {
            fprintf(stderr, "fakedev: MAP_BO refused, busy\n");
            return -1;
        }
        m->user_va = (unsigned long long)(size_t)bo[i].mem;
        return 0;
    }
    case IOCTL_RKNPU_PREP_BO:
    case IOCTL_RKNPU_FINI_BO:
        return find(((const rknpu_bo_sync *)inb)->handle) < 0 ? -1 : 0;
    case IOCTL_RKNPU_UNMAP_BO:
        return find(((const rknpu_bo_handle *)inb)->handle) < 0 ? -1 : 0;
    case IOCTL_RKNPU_SUBMIT: {
        const rknpu_submit_in *s = (const rknpu_submit_in *)inb;
        rknpu_submit_out o;
        unsigned int t;

        if (s->core >= RKNPU_NUM_CORES || s->task_count == 0 || s->task_count > RKNPU_MAX_TASKS)
            return -1;
        unsigned int key = RKNPU_RC_CBUF_ANY, other = s->core ^ 1u, collided = 0;

        if (running((int)s->core)) {
            fprintf(stderr, "fakedev: SUBMIT refused, core %u busy\n", s->core);
            return -1;
        }
        for (t = 0; t < s->task_count; t++) {
            const unsigned char *st = at(s->tasks[t].regcmd, s->tasks[t].regcmd_count * 8u);

            if (!st)
                return -1;
            key = rknpu_rc_cbuf_merge(key, rknpu_rc_cbuf_key((const unsigned long long *)st,
                                                             s->tasks[t].regcmd_count), t == 0);
        }
        /* the shared CBUF: the driver's wait, or the hardware's corruption */
        if (running((int)other) && rknpu_rc_cbuf_collide(key, job[other].key)) {
            if (getenv("FAKE_NO_CBUF_GUARD")) {
                collided = 1;
            } else {
                cbuf_waits[s->core]++;
                while (running((int)other)) {
                    struct timespec ts = { 0, 200000 };

                    pthread_mutex_unlock(&lk);
                    nanosleep(&ts, NULL);
                    pthread_mutex_lock(&lk);
                }
                if (running((int)s->core))
                    return -1;
            }
        }
        job[s->core].nout = 0;
        for (t = 0; t < s->task_count; t++) {
            if (run_task(&s->tasks[t]))
                return -1;
            job[s->core].out_addr[t] = last_out_addr;
            job[s->core].out_n[t] = last_out_n;
            job[s->core].nout = t + 1;
        }
        if (collided) {
            unsigned int q, w, owner = cur;

            /* both jobs' outputs, as the board's 10039 and 8350 wrong words */
            for (q = 0; q < 2; q++) {
                unsigned int cj = q ? other : s->core;

                cur = job[cj].owner;
                if (q == 0)
                    cur = owner;
                for (t = 0; t < (q ? job[cj].nout : s->task_count); t++) {
                    unsigned int oa = q ? job[cj].out_addr[t] : job[s->core].out_addr[t];
                    unsigned int on = q ? job[cj].out_n[t] : job[s->core].out_n[t];
                    unsigned char *o = at(oa, on * 4);

                    for (w = 0; o && w < on; w++)
                        o[w * 4] ^= 0x5A;
                }
            }
            cur = owner;
        }
        memset(&o, 0, sizeof(o));
        o.seqno = ++seq;
        o.core = s->core;
        job[s->core].seq = o.seqno;
        job[s->core].owner = cur;
        job[s->core].copied = (s->flags & RKNPU_SUBMIT_COPY) != 0;
        job[s->core].active = 1;
        job[s->core].key = key;
        job[s->core].end_us = fake_now_us() + FAKE_JOB_US;
        memcpy(outb, &o, sizeof(o));
        return 0;
    }
    case IOCTL_RKNPU_WAIT: {
        rknpu_wait_out o;

        int c, found = 0;

        memset(&o, 0, sizeof(o));
        o.seqno = ((const rknpu_wait_in *)inb)->seqno;
        for (c = 0; c < RKNPU_NUM_CORES; c++) {
            if (job[c].seq != o.seqno || job[c].owner != cur)
                continue;
            found = 1;
            /* the job "runs" until its time is up; the lock is dropped meanwhile,
             * as the M6 driver's WAIT holds no lock */
            while (job[c].active && fake_now_us() < job[c].end_us) {
                struct timespec ts = { 0, 200000 };

                pthread_mutex_unlock(&lk);
                nanosleep(&ts, NULL);
                pthread_mutex_lock(&lk);
            }
            if (job[c].seq == o.seqno)
                job[c].active = 0;
            o.core = (unsigned int)c;
        }
        o.result = found ? RKNPU_JOB_DONE : RKNPU_JOB_UNKNOWN;
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
