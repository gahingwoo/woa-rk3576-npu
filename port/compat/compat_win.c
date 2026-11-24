/*
 * compat_win.c - the POSIX pieces charsiu needs, on Windows
 *
 * mmap of a model file, CPU affinity, the crash note, and binary stdio:
 * everything charsiu reads and prints is bytes, as on Linux, so its output
 * redirected to a file compares byte for byte with the Linux run.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

/* ------------------------------------------------------ binary stdio */

__attribute__((constructor)) static void compat_binary_stdio(void)
{
	_fmode = _O_BINARY;
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);
	/* the console shows charsiu's UTF-8 as UTF-8 (a file gets the bytes anyway) */
	SetConsoleOutputCP(CP_UTF8);
}

/* -------------------------------------------------------------- mmap */

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
	HANDLE fh, mh;
	DWORD protect, access;
	void *p;

	(void)addr;
	if (fd < 0 || len == 0) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	fh = (HANDLE)_get_osfhandle(fd);
	if (fh == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return MAP_FAILED;
	}
	if (prot & PROT_WRITE) {
		protect = (flags & MAP_PRIVATE) ? PAGE_WRITECOPY : PAGE_READWRITE;
		access = (flags & MAP_PRIVATE) ? FILE_MAP_COPY : FILE_MAP_WRITE;
	} else {
		protect = PAGE_READONLY;
		access = FILE_MAP_READ;
	}
	mh = CreateFileMappingA(fh, NULL, protect, 0, 0, NULL);
	if (!mh) {
		errno = EACCES;
		return MAP_FAILED;
	}
	p = MapViewOfFile(mh, access, (DWORD)((unsigned long long)off >> 32), (DWORD)off, len);
	CloseHandle(mh);        /* the view holds the section */
	if (!p) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return p;
}

int munmap(void *addr, size_t len)
{
	(void)len;
	return UnmapViewOfFile(addr) ? 0 : -1;
}

/* ---------------------------------------------------------- affinity */

int sched_setaffinity(int pid, size_t size, const cpu_set_t *set)
{
	(void)size;
	if (pid != 0 || !set->bits) {
		errno = EINVAL;
		return -1;
	}
	return SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)set->bits) ? 0 : -1;
}

int sched_getaffinity(int pid, size_t size, cpu_set_t *set)
{
	DWORD_PTR proc = 0, sys = 0;

	(void)size;
	if (pid != 0 || !GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) {
		errno = EINVAL;
		return -1;
	}
	set->bits = proc;
	return 0;
}

int sched_getcpu(void)
{
	return (int)GetCurrentProcessorNumber();
}

int pthread_setaffinity_np(pthread_t t, size_t size, const cpu_set_t *set)
{
	(void)size;
	if (!set->bits)
		return EINVAL;
	return SetThreadAffinityMask(pthread_gethandle(t), (DWORD_PTR)set->bits) ? 0 : EINVAL;
}

int pthread_getaffinity_np(pthread_t t, size_t size, cpu_set_t *set)
{
	DWORD_PTR proc = 0, sys = 0;

	(void)t;
	(void)size;
	if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys))
		return EINVAL;
	set->bits = proc;
	return 0;
}

/* -------------------------------------------------------- crash note */

/* device_rknpu.c: the last stage charsiu_note() recorded */
extern const char *volatile charsiu_note_what;
extern volatile unsigned long charsiu_note_a, charsiu_note_b;

static LONG WINAPI note_crash(EXCEPTION_POINTERS *ep)
{
	char buf[320];
	int n;
	DWORD w = 0;

	n = snprintf(buf, sizeof(buf), "\ncharsiu: died in %s (%lu, %lu) exception 0x%08lx at %p\n",
		     charsiu_note_what ? charsiu_note_what : "?", charsiu_note_a, charsiu_note_b,
		     (unsigned long)ep->ExceptionRecord->ExceptionCode,
		     ep->ExceptionRecord->ExceptionAddress);
	if (n > 0)
		WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, (DWORD)n, &w, NULL);
	return EXCEPTION_CONTINUE_SEARCH;       /* Windows Error Reporting still gets it */
}

void charsiu_port_crash_handler_install(void)
{
	SetUnhandledExceptionFilter(note_crash);
}

/* ----------------------------------------------------------- sysconf */

long sysconf(int name)
{
	SYSTEM_INFO si;

	switch (name) {
	case _SC_NPROCESSORS_CONF:
	case _SC_NPROCESSORS_ONLN:
		return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	case _SC_PAGESIZE:
		GetSystemInfo(&si);
		return (long)si.dwPageSize;
	default:
		errno = EINVAL;
		return -1;
	}
}
