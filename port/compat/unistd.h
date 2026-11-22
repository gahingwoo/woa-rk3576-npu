/*
 * unistd.h for charsiu on Windows: mingw's, plus sysconf for the CPU count
 * (llama.c sizes its thread pool from it).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CHARSIU_COMPAT_UNISTD_H
#define CHARSIU_COMPAT_UNISTD_H

#include_next <unistd.h>

#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_CONF    83
#define _SC_NPROCESSORS_ONLN    84
#define _SC_PAGESIZE            30
#endif

long sysconf(int name);

#endif
