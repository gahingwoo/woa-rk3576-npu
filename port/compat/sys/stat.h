/*
 * sys/stat.h for charsiu on Windows: mingw's, with POSIX's two-argument
 * mkdir (npupool.c's weight cache directory).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CHARSIU_COMPAT_SYS_STAT_H
#define CHARSIU_COMPAT_SYS_STAT_H

#include_next <sys/stat.h>
#include <io.h>
#include <direct.h>

#define mkdir(path, mode) _mkdir(path)

#endif
