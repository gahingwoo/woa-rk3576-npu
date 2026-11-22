/*
 * fakedev.h - the host stand-in for rknpu.sys (fakedev.c), CI only
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RKNPU_FAKEDEV_H
#define RKNPU_FAKEDEV_H

int fake_ioctl(unsigned int code, const void *in, unsigned int in_len, void *out, unsigned int out_len);

#endif
