/*
 * sys/mman.h for charsiu on Windows: the one use is gguf.c (and whisper.c)
 * mapping a model file read only. compat_win.c has the code.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef CHARSIU_COMPAT_SYS_MMAN_H
#define CHARSIU_COMPAT_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE       0
#define PROT_READ       1
#define PROT_WRITE      2
#define MAP_SHARED      1
#define MAP_PRIVATE     2
#define MAP_FAILED      ((void *)-1)

/* A file mapping (fd from open()); anonymous mappings are not supported. */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off);
int munmap(void *addr, size_t len);

#endif
