/*
 * fcntl.h for charsiu on Windows: mingw's, plus O_CLOEXEC. (Every file is
 * opened in binary mode: compat_win.c sets _fmode before main, since text
 * mode would turn CR LF into LF inside a model file.)
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CHARSIU_COMPAT_FCNTL_H
#define CHARSIU_COMPAT_FCNTL_H

#include_next <fcntl.h>
#include <io.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif

#endif
