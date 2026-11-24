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

/*
 * M7: the mutexes and condition variables charsiu uses (its thread pool's
 * barrier, the device locks) as Windows SRW locks and condition variables,
 * with a short spin before a waiter sleeps. winpthreads' own condition
 * variable is built from semaphores, and the board's silu * up (16 pool
 * barriers a token over 131k elements, 1.4 ms of work on one thread at
 * charsiu's 10.4 ns an element) took 3.06 ms a token: about 170 us a
 * barrier against the 50 us charsiu's pool sizing assumes. That is a model
 * from two numbers; the M7 bench's short-pthread arm is the measurement.
 * The storage is winpthreads' own
 * (pthread_mutex_t, pthread_cond_t, their static initialisers), holding a
 * pointer to the Windows object once used.
 *
 *   CHARSIU_WIN_SYNC=pthread   winpthreads, as M6
 *   CHARSIU_WIN_SPIN_US        how long a waiter spins first (default 50, 0 none)
 *
 * Every charsiu signal and broadcast is made with the mutex held, which is
 * what makes the spin safe: see compat_win.c.
 */
#ifndef CHARSIU_COMPAT_NO_SYNC
int charsiu_mx_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int charsiu_mx_destroy(pthread_mutex_t *m);
int charsiu_mx_lock(pthread_mutex_t *m);
int charsiu_mx_trylock(pthread_mutex_t *m);
int charsiu_mx_unlock(pthread_mutex_t *m);
int charsiu_cv_init(pthread_cond_t *c, const pthread_condattr_t *a);
int charsiu_cv_destroy(pthread_cond_t *c);
int charsiu_cv_wait(pthread_cond_t *c, pthread_mutex_t *m);
int charsiu_cv_signal(pthread_cond_t *c);
int charsiu_cv_broadcast(pthread_cond_t *c);
#define pthread_mutex_init(m, a)    charsiu_mx_init(m, a)
#define pthread_mutex_destroy(m)    charsiu_mx_destroy(m)
#define pthread_mutex_lock(m)       charsiu_mx_lock(m)
#define pthread_mutex_trylock(m)    charsiu_mx_trylock(m)
#define pthread_mutex_unlock(m)     charsiu_mx_unlock(m)
#define pthread_cond_init(c, a)     charsiu_cv_init(c, a)
#define pthread_cond_destroy(c)     charsiu_cv_destroy(c)
#define pthread_cond_wait(c, m)     charsiu_cv_wait(c, m)
#define pthread_cond_signal(c)      charsiu_cv_signal(c)
#define pthread_cond_broadcast(c)   charsiu_cv_broadcast(c)
#endif

#endif
