/*
 * stdlib.h for charsiu on Windows: the CRT's, plus setenv and unsetenv,
 * which charsiu uses to pass switches between its own files. The CRT's
 * environment is the one getenv reads, so _putenv_s is the whole of it.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CHARSIU_COMPAT_STDLIB_H
#define CHARSIU_COMPAT_STDLIB_H

#include_next <stdlib.h>

static inline int setenv(const char *name, const char *value, int overwrite)
{
	if (!overwrite && getenv(name))
		return 0;
	return _putenv_s(name, value) ? -1 : 0;
}

/* _putenv_s with "" removes the variable */
static inline int unsetenv(const char *name)
{
	return _putenv_s(name, "") ? -1 : 0;
}

#endif
