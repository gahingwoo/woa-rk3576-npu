/*
 * linux/futex.h for charsiu on Windows: the two operations llama.c uses
 * (see sys/syscall.h). The values are Linux's.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CHARSIU_COMPAT_LINUX_FUTEX_H
#define CHARSIU_COMPAT_LINUX_FUTEX_H

#define FUTEX_WAIT              0
#define FUTEX_WAKE              1
#define FUTEX_PRIVATE_FLAG      128
#define FUTEX_WAIT_PRIVATE      (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE      (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

#endif
