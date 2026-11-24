/*
 * fakedev.h - the host stand-in for rknpu.sys (fakedev.c), CI only
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RKNPU_FAKEDEV_H
#define RKNPU_FAKEDEV_H

/* M6: a handle per thread; dev_open() calls this. Buffers and jobs belong
 * to the calling thread's handle, as they belong to a file object in the
 * driver. */
void fake_open(void);
int fake_ioctl(unsigned int code, const void *in, unsigned int in_len, void *out, unsigned int out_len);

#endif
