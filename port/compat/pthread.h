/*
 * pthread.h for charsiu on Windows: winpthreads', plus the affinity call.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CHARSIU_COMPAT_PTHREAD_H
#define CHARSIU_COMPAT_PTHREAD_H

#include_next <pthread.h>
#include <sched.h>

int pthread_setaffinity_np(pthread_t t, size_t size, const cpu_set_t *set);
int pthread_getaffinity_np(pthread_t t, size_t size, cpu_set_t *set);

#endif
