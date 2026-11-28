/*
 * sys/syscall.h for charsiu on Windows: the one system call charsiu 0.2
 * makes directly is futex, from llama.c's fast pool (CHARSIU_POOL_FAST),
 * where the workers park on the dispatch generation and the caller on the
 * fan-in count. compat_win.c answers it with WaitOnAddress and
 * WakeByAddressSingle / WakeByAddressAll, which are the same operation:
 * sleep while a 32-bit word still holds a value, wake who sleeps on it.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CHARSIU_COMPAT_SYS_SYSCALL_H
#define CHARSIU_COMPAT_SYS_SYSCALL_H

#define SYS_futex 98            /* the arm64 Linux number; only a tag here */

long syscall(long number, ...);

#endif
