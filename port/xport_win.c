/*
 * xport_win.c - device_rknpu.c's transport on Windows: \\.\RKNPU
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2         /* GetProcessMemoryInfo is kernel32's K32 export */
#include <windows.h>
#include <psapi.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rknpu_ioctl.h"
#include "rknpu_xport.h"

struct rkx {
	HANDLE h;
};

static int errno_of(DWORD e)
{
	switch (e) {
	case ERROR_BUSY:                    return -EBUSY;   /* STATUS_DEVICE_BUSY */
	case ERROR_NOACCESS:                return -EACCES;  /* STATUS_ACCESS_VIOLATION */
	case ERROR_INVALID_USER_BUFFER:     return -E2BIG;   /* STATUS_INVALID_BUFFER_SIZE */
	case ERROR_NO_SYSTEM_RESOURCES:
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:             return -ENOMEM;  /* STATUS_INSUFFICIENT_RESOURCES */
	case ERROR_NOT_ENOUGH_QUOTA:        return -ENOSPC;  /* STATUS_QUOTA_EXCEEDED */
	case ERROR_INVALID_PARAMETER:       return -EINVAL;
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:          return -ENOENT;
	case ERROR_ACCESS_DENIED:           return -EPERM;
	default:                            return -EIO;
	}
}

struct rkx *rkx_open(const char *path, int *err)
{
	struct rkx *x;
	HANDLE h;

	if (path && *path)
		h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	else
		h = CreateFileW(RKNPU_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		DWORD e = GetLastError();
		static int said;

		*err = errno_of(e);
		/* once: three devices are opened, and a run without the driver is normal */
		if (!said++)
			fprintf(stderr, "charsiu: cannot open %s (Windows error %lu)%s\n",
				path && *path ? path : "\\\\.\\RKNPU", (unsigned long)e,
				e == ERROR_ACCESS_DENIED ? ": run as administrator" :
				e == ERROR_FILE_NOT_FOUND ? ": is rknpu.sys installed and the NPU "
				"enabled in the firmware setup?" : "");
		return NULL;
	}
	x = calloc(1, sizeof(*x));
	if (!x) {
		CloseHandle(h);
		*err = -ENOMEM;
		return NULL;
	}
	x->h = h;
	return x;
}

void rkx_close(struct rkx *x)
{
	if (!x)
		return;
	CloseHandle(x->h);
	free(x);
}

int rkx_ioctl(struct rkx *x, unsigned int code, const void *in, unsigned int inlen,
	      void *out, unsigned int outlen, unsigned int *got)
{
	DWORD n = 0;

	*got = 0;
	if (!DeviceIoControl(x->h, code, (LPVOID)in, inlen, out, outlen, &n, NULL))
		return errno_of(GetLastError());
	*got = n;
	return 0;
}

void rkx_pause(unsigned int us)
{
	if (us < 1000)
		SwitchToThread();
	else
		Sleep(us / 1000);
}

void rkx_relax(void)
{
	__asm__ __volatile__("yield" ::: "memory");
}

unsigned long long rkx_now_us(void)
{
	static LARGE_INTEGER f;
	LARGE_INTEGER t;

	if (!f.QuadPart)
		QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (unsigned long long)(t.QuadPart / f.QuadPart) * 1000000ull +
	       (unsigned long long)(t.QuadPart % f.QuadPart) * 1000000ull / (unsigned long long)f.QuadPart;
}

void rkx_wmb(void)
{
	/* DSB SY, not MemoryBarrier()'s DMB: the write-combine buffer is drained
	 * to memory, which the NPU reads without snooping, before the SUBMIT */
	__asm__ __volatile__("dsb sy" ::: "memory");
}

/*
 * M6: the M4 board run was 4x slower than the CPU arm in decode with only
 * 7% of the wall in the NPU path; paging the model file back in from the
 * USB stick under memory pressure is one candidate this line can confirm
 * or rule out (PageFaultCount counts soft faults too, so compare arms).
 */
const char *rkx_mem_report(void)
{
	static char buf[96];
	PROCESS_MEMORY_COUNTERS pmc;

	memset(&pmc, 0, sizeof(pmc));
	pmc.cb = sizeof(pmc);
	if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
		return "no memory counters";
	snprintf(buf, sizeof(buf), "%lu page faults, peak working set %.0f MiB",
		 (unsigned long)pmc.PageFaultCount, pmc.PeakWorkingSetSize / 1048576.0);
	return buf;
}

/* ------------------------------------------------ M7: user-mode DC by VA */

/*
 * DC CVAC and DC CIVAC are allowed at EL0 only when the kernel sets
 * SCTLR_EL1.UCI; otherwise they trap and Windows raises an exception. So
 * the first use runs each once, on a line of this stack, with a vectored
 * handler that recognises exactly these two instructions, notes the fault
 * and steps over them. The line size is the A72's and the A53's, 64 bytes:
 * CTR_EL0 is not read, because reading it from EL0 can trap as well
 * (SCTLR_EL1.UCT), and a smaller line than the real one would only clean
 * some lines twice.
 */
#define RKX_DC_LINE 64u

void rkx_dc_probe_cvac(void *p);
void rkx_dc_probe_civac(void *p);
__asm__(".text\n"
	".p2align 2\n"
	".globl rkx_dc_probe_cvac\n"
	"rkx_dc_probe_cvac:\n"
	"    dc cvac, x0\n"
	"    dsb sy\n"
	"    ret\n"
	".p2align 2\n"
	".globl rkx_dc_probe_civac\n"
	"rkx_dc_probe_civac:\n"
	"    dc civac, x0\n"
	"    dsb sy\n"
	"    ret\n");

static volatile LONG dc_faults;

static LONG CALLBACK dc_probe_handler(EXCEPTION_POINTERS *ep)
{
	PVOID at = ep->ExceptionRecord->ExceptionAddress;

	if (at != (PVOID)rkx_dc_probe_cvac && at != (PVOID)rkx_dc_probe_civac)
		return EXCEPTION_CONTINUE_SEARCH;
	InterlockedIncrement(&dc_faults);
	ep->ContextRecord->Pc += 4;     /* step over the DC */
	return EXCEPTION_CONTINUE_EXECUTION;
}

int rkx_dc_usable(void)
{
	static volatile LONG state;     /* 0 unknown, 1 usable, 2 not */
	volatile unsigned char line[2 * RKX_DC_LINE];
	PVOID h;

	if (state)
		return state == 1;
	h = AddVectoredExceptionHandler(1, dc_probe_handler);
	if (!h) {
		state = 2;
		return 0;
	}
	line[0] = 1;
	rkx_dc_probe_cvac((void *)line);
	rkx_dc_probe_civac((void *)line);
	RemoveVectoredExceptionHandler(h);
	state = dc_faults ? 2 : 1;
	fprintf(stderr, "charsiu rknpu: user-mode DC CVAC / DC CIVAC %s\n",
		state == 1 ? "work (CHARSIU_RKNPU_UCACHE can do the cache maintenance here)"
			   : "trap (SCTLR_EL1.UCI is 0); the driver keeps doing it");
	return state == 1;
}

void rkx_dc_clean(const void *p, size_t n)
{
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)(RKX_DC_LINE - 1), end = (uintptr_t)p + n;

	for (; a < end; a += RKX_DC_LINE)
		__asm__ __volatile__("dc cvac, %0" :: "r"(a) : "memory");
	__asm__ __volatile__("dsb sy" ::: "memory");
}

void rkx_dc_clean_inval(const void *p, size_t n)
{
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)(RKX_DC_LINE - 1), end = (uintptr_t)p + n;

	for (; a < end; a += RKX_DC_LINE)
		__asm__ __volatile__("dc civac, %0" :: "r"(a) : "memory");
	__asm__ __volatile__("dsb sy" ::: "memory");
}
