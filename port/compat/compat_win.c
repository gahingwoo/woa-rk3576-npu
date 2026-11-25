/*
 * compat_win.c - the POSIX pieces charsiu needs, on Windows
 *
 * mmap of a model file, CPU affinity, the crash note, and binary stdio:
 * everything charsiu reads and prints is bytes, as on Linux, so its output
 * redirected to a file compares byte for byte with the Linux run.
 *
 * M7: the CPU topology charsiu reads from sysfs, the hold it takes through
 * /dev/cpu_dma_latency, and faster mutexes and condition variables (see
 * stdio.h, fcntl.h and pthread.h here for what each one replaces).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00    /* Windows 10: CPU sets, process power throttling */
#endif
/* this file is where the M7 wrappers live, so it sees the real calls */
#define CHARSIU_COMPAT_NO_SYNC
#define CHARSIU_COMPAT_NO_FOPEN
#include <windows.h>
#include <powerbase.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
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
	/* and a conversation typed into it reaches the model as UTF-8, not in
	 * the console's own code page (936 on a Chinese Windows) */
	SetConsoleCP(CP_UTF8);
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

/* ============================================================== M7 */

static long env_num(const char *name, long dflt)
{
	const char *e = getenv(name);

	return e && *e ? atol(e) : dflt;
}

/* ------------------------------------------- the CPUs, as sysfs says them */

/*
 * Per logical processor: a "clock" in kHz that charsiu only compares, to
 * find the fast cluster (it prints scaling_cur_freq, never this). The
 * order is decided by, first to differ between processors:
 *
 *   1. the scheduler's EfficiencyClass, if the firmware set one (MADT GICC;
 *      edk2-rk3576 leaves it 0 on every core today);
 *   2. the core type from MIDR_EL1 (the registry's "CP 4000"): a big Arm
 *      core over a little one. An A72 at 1608 MHz is faster than an A53 at
 *      2016, so this outranks the clock;
 *   3. Windows' own maximum clock (CallNtPowerInformation, else ~MHz).
 *
 * The rank is added as a large offset, so it wins any comparison.
 */
#define CPU_MAX 64

typedef struct {
	ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
} rk_ppi;                               /* PROCESSOR_POWER_INFORMATION */

static struct {
	int done, n;
	long khz[CPU_MAX];
	ULONG cur_mhz[CPU_MAX];
	ULONG max_idle;                 /* the deepest idle state Windows has, 0 = one state */
	unsigned midr[CPU_MAX];
} cpus;

static int midr_rank(unsigned midr)
{
	static const unsigned short big[] = { 0xD07, 0xD08, 0xD09, 0xD0A, 0xD0B, 0xD0C, 0xD0D,
					      0xD0E, 0xD40, 0xD41, 0xD44, 0xD47, 0xD48, 0xD4B,
					      0xD4D, 0xD4E, 0xD81, 0xD82, 0xD87 };
	unsigned part = (midr >> 4) & 0xFFF, i;

	if (((midr >> 24) & 0xFF) != 0x41)      /* not an Arm Ltd core: no opinion */
		return 0;
	for (i = 0; i < sizeof(big) / sizeof(big[0]); i++)
		if (part == big[i])
			return 1;
	return 0;
}

static void cpus_read(void)
{
	rk_ppi ppi[CPU_MAX];
	BYTE eff[CPU_MAX];
	int i, n, effdiff = 0, rankdiff = 0;
	ULONG len = 0;

	if (cpus.done)
		return;
	cpus.done = 1;
	n = (int)GetActiveProcessorCount(0);
	if (n > CPU_MAX)
		n = CPU_MAX;
	cpus.n = n;
	memset(ppi, 0, sizeof(ppi));
	memset(eff, 0, sizeof(eff));
	(void)CallNtPowerInformation(ProcessorInformation, NULL, 0, ppi, (ULONG)(n * sizeof(ppi[0])));

	GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
	if (len) {
		BYTE *buf = malloc(len);

		if (buf && GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len,
						      GetCurrentProcess(), 0)) {
			ULONG at = 0;

			while (at < len) {
				PSYSTEM_CPU_SET_INFORMATION c = (PSYSTEM_CPU_SET_INFORMATION)(buf + at);

				if (c->Type == CpuSetInformation && c->CpuSet.Group == 0 &&
				    c->CpuSet.LogicalProcessorIndex < CPU_MAX)
					eff[c->CpuSet.LogicalProcessorIndex] = c->CpuSet.EfficiencyClass;
				at += c->Size;
			}
		}
		free(buf);
	}
	for (i = 0; i < n; i++) {
		char key[96];
		HKEY h;
		DWORD mhz = 0, sz = sizeof(mhz);
		ULONGLONG midr = 0;
		DWORD qsz = sizeof(midr);

		snprintf(key, sizeof(key), "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%d", i);
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) == ERROR_SUCCESS) {
			RegQueryValueExA(h, "~MHz", NULL, NULL, (BYTE *)&mhz, &sz);
			RegQueryValueExA(h, "CP 4000", NULL, NULL, (BYTE *)&midr, &qsz);
			RegCloseKey(h);
		}
		cpus.midr[i] = (unsigned)midr;
		cpus.khz[i] = (long)(ppi[i].MaxMhz ? ppi[i].MaxMhz : mhz ? mhz : 1000) * 1000;
		cpus.cur_mhz[i] = ppi[i].CurrentMhz ? ppi[i].CurrentMhz : mhz;
		if (ppi[i].MaxIdleState > cpus.max_idle)
			cpus.max_idle = ppi[i].MaxIdleState;
		if (i && midr_rank(cpus.midr[i]) != midr_rank(cpus.midr[0]))
			rankdiff = 1;
		if (i && eff[i] != eff[0])
			effdiff = 1;
	}
	for (i = 0; i < n; i++)
		if (cpus.khz[i] && (effdiff || rankdiff))
			cpus.khz[i] += (effdiff ? (long)eff[i] : (long)midr_rank(cpus.midr[i])) * 100000000L;
}

/* a FILE* reading `text`, through a pipe: the CRT has no fmemopen */
static FILE *text_file(const char *text)
{
	int fd[2];
	size_t len = strlen(text);
	FILE *f;

	if (_pipe(fd, 4096, _O_BINARY))
		return NULL;
	if (_write(fd[1], text, (unsigned)len) != (int)len) {
		_close(fd[0]);
		_close(fd[1]);
		return NULL;
	}
	_close(fd[1]);
	f = _fdopen(fd[0], "rb");
	if (!f)
		_close(fd[0]);
	return f;
}

FILE *charsiu_compat_fopen(const char *path, const char *mode)
{
	static const char pre[] = "/sys/devices/system/cpu/cpu";
	char what[48], buf[64];
	int cpu;

	if (strncmp(path, pre, sizeof(pre) - 1))
		return fopen(path, mode);
	if (!env_num("CHARSIU_WIN_SYSFS", 1) ||
	    sscanf(path + sizeof(pre) - 1, "%d/cpufreq/%47s", &cpu, what) != 2) {
		errno = ENOENT;
		return NULL;
	}
	cpus_read();
	if (cpu < 0 || cpu >= cpus.n || !cpus.khz[cpu]) {
		errno = ENOENT;
		return NULL;
	}
	if (!strcmp(what, "cpuinfo_max_freq"))
		snprintf(buf, sizeof(buf), "%ld\n", cpus.khz[cpu]);
	else if (!strcmp(what, "scaling_cur_freq"))
		snprintf(buf, sizeof(buf), "%lu\n", (unsigned long)cpus.cur_mhz[cpu] * 1000ul);
	else if (!strcmp(what, "scaling_governor"))
		snprintf(buf, sizeof(buf), "windows\n");
	else {
		errno = ENOENT;
		return NULL;
	}
	return text_file(buf);
}

/* ------------------------------------------------ /dev/cpu_dma_latency */

/*
 * What charsiu's hold becomes here, for as long as the process lives:
 *
 *   - power throttling off for this process (SetProcessInformation,
 *     PROCESS_POWER_THROTTLING_EXECUTION_SPEED with the state 0): Windows
 *     does not run its threads as EcoQoS, which on a heterogeneous machine
 *     means the little cores at their lowest clock;
 *   - the system timer at 1 ms (timeBeginPeriod), which bounds how long a
 *     CPU stays in one idle stretch and how late a sleeping thread wakes.
 *
 * What it cannot become is Linux's PM QoS bound: Windows has no per-process
 * idle-state latency limit. On this firmware it does not need one, because
 * without the "CPU Idle States" setup option there is no _LPI and Windows
 * idles in WFI only, which is what the 100 us bound leaves Linux; the line
 * below says how many idle states Windows reports, and m7-bench's
 * short-idle arm (the processor idle-disable power setting) is the control.
 *
 * CHARSIU_RKNPU_QOS=0 skips both (the open still succeeds, so charsiu's
 * message is the same); CHARSIU_NPU_IDLE=1 keeps charsiu from asking.
 */
typedef UINT (WINAPI *time_period_fn)(UINT);

static void qos_hold(void)
{
	static int held;
	PROCESS_POWER_THROTTLING_STATE ps;
	int thr, tim = 0;
	HMODULE wm;

	if (held++)
		return;
	if (!env_num("CHARSIU_RKNPU_QOS", 1)) {
		fprintf(stderr, "charsiu (Windows): CHARSIU_RKNPU_QOS=0, the CPU hold does nothing here\n");
		return;
	}
	memset(&ps, 0, sizeof(ps));
	ps.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	ps.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
	ps.StateMask = 0;
	thr = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &ps, sizeof(ps)) != 0;
	wm = LoadLibraryA("winmm.dll");
	if (wm) {
		time_period_fn tbp = (time_period_fn)(void (*)(void))GetProcAddress(wm, "timeBeginPeriod");

		tim = tbp && tbp(1) == 0;      /* TIMERR_NOERROR; ended by the process's exit */
	}
	cpus_read();
	fprintf(stderr, "charsiu (Windows): the CPU hold is power throttling %s and a %s timer; "
		"Windows reports %lu idle state%s past the first%s\n",
		thr ? "off" : "unchanged (refused)", tim ? "1 ms" : "default",
		(unsigned long)cpus.max_idle, cpus.max_idle == 1 ? "" : "s",
		cpus.max_idle ? " (the firmware's _LPI: m7-bench's short-idle arm is the control)"
			      : " (WFI only, which is what Linux's 100 us bound allows)");
}

int charsiu_compat_open(const char *path, int flags, ...)
{
	int mode = 0;

	if (flags & _O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	if (path && !strcmp(path, "/dev/cpu_dma_latency")) {
		int fd = _open("NUL", _O_RDWR | _O_BINARY | _O_NOINHERIT);

		if (fd >= 0)
			qos_hold();
		return fd;
	}
	return _open(path, flags, mode);
}

/* ------------------------------------- mutexes and condition variables */

/*
 * mode 1: SRW locks and condition variables, with a spin (pthread.h);
 * mode 2: winpthreads. Chosen once, before any object is used, so every
 * object lives in one world.
 */
static volatile LONG sync_mode;
static long spin_us = -1;
static LONGLONG qpc_per_us;

static int sync_srw(void)
{
	LONG m = sync_mode;

	if (!m) {
		const char *e = getenv("CHARSIU_WIN_SYNC");
		LARGE_INTEGER f;

		m = e && !strcmp(e, "pthread") ? 2 : 1;
		spin_us = env_num("CHARSIU_WIN_SPIN_US", 0);   /* board-measured, docs/dev/performance.md */
		QueryPerformanceFrequency(&f);
		qpc_per_us = f.QuadPart / 1000000 ? f.QuadPart / 1000000 : 1;
		InterlockedCompareExchange(&sync_mode, m, 0);
		m = sync_mode;
	}
	return m == 1;
}

typedef struct {
	SRWLOCK lock;
} cmx;

typedef struct {
	CONDITION_VARIABLE cv;
	volatile LONG seq;              /* bumped by every signal, under the mutex */
} ccv;

/* the object behind a pthread_mutex_t / pthread_cond_t, made on first use
 * (0 and winpthreads' static initialisers, all negative, mean "not yet") */
static void *obj_get(intptr_t *slot, size_t size)
{
	intptr_t v = *slot;
	void *o;

	if (v > 0)
		return (void *)v;
	o = calloc(1, size);
	if (!o)
		abort();
	if (size == sizeof(cmx))
		InitializeSRWLock(&((cmx *)o)->lock);
	else
		InitializeConditionVariable(&((ccv *)o)->cv);
	if ((intptr_t)InterlockedCompareExchangePointer((PVOID *)slot, o, (PVOID)v) != v)
		free(o);                /* another thread made it first */
	return (void *)*slot;
}

#define MX(m) ((cmx *)obj_get((intptr_t *)(m), sizeof(cmx)))
#define CV(c) ((ccv *)obj_get((intptr_t *)(c), sizeof(ccv)))

int charsiu_mx_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
	if (!sync_srw())
		return pthread_mutex_init(m, a);
	*m = 0;
	(void)MX(m);
	return 0;
}

int charsiu_mx_destroy(pthread_mutex_t *m)
{
	if (!sync_srw())
		return pthread_mutex_destroy(m);
	if (*m > 0)
		free((void *)*m);
	*m = 0;
	return 0;
}

int charsiu_mx_lock(pthread_mutex_t *m)
{
	if (!sync_srw())
		return pthread_mutex_lock(m);
	AcquireSRWLockExclusive(&MX(m)->lock);
	return 0;
}

int charsiu_mx_trylock(pthread_mutex_t *m)
{
	if (!sync_srw())
		return pthread_mutex_trylock(m);
	return TryAcquireSRWLockExclusive(&MX(m)->lock) ? 0 : EBUSY;
}

int charsiu_mx_unlock(pthread_mutex_t *m)
{
	if (!sync_srw())
		return pthread_mutex_unlock(m);
	ReleaseSRWLockExclusive(&MX(m)->lock);
	return 0;
}

int charsiu_cv_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
	if (!sync_srw())
		return pthread_cond_init(c, a);
	*c = 0;
	(void)CV(c);
	return 0;
}

int charsiu_cv_destroy(pthread_cond_t *c)
{
	if (!sync_srw())
		return pthread_cond_destroy(c);
	if (*c > 0)
		free((void *)*c);
	*c = 0;
	return 0;
}

/*
 * The spin. With the mutex held the waiter reads the signal count, lets the
 * mutex go and watches the count for up to CHARSIU_WIN_SPIN_US; a signal in
 * that window is a wake-up without a trip through the scheduler. Otherwise
 * it takes the mutex back and, if the count has still not moved, sleeps.
 * No signal can be lost between that check and the sleep, because a signal
 * needs the mutex to move the count (every caller in charsiu holds it) and
 * SleepConditionVariableSRW lets the mutex go only once the waiter is
 * queued. Returning after a signal that was meant for another waiter is a
 * spurious wake-up, which every pthread_cond_wait caller already allows.
 */
int charsiu_cv_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
	ccv *v;
	cmx *x;
	LONG s;

	if (!sync_srw())
		return pthread_cond_wait(c, m);
	v = CV(c);
	x = MX(m);
	s = v->seq;
	if (spin_us > 0) {
		LARGE_INTEGER t0, t;
		LONGLONG limit = spin_us * qpc_per_us;
		unsigned k = 0;

		ReleaseSRWLockExclusive(&x->lock);
		QueryPerformanceCounter(&t0);
		for (;;) {
			if (v->seq != s)
				break;
			__asm__ __volatile__("yield" ::: "memory");
			if ((++k & 63) == 0) {
				QueryPerformanceCounter(&t);
				if (t.QuadPart - t0.QuadPart > limit)
					break;
			}
		}
		AcquireSRWLockExclusive(&x->lock);
		if (v->seq != s)
			return 0;
	}
	SleepConditionVariableSRW(&v->cv, &x->lock, INFINITE, 0);
	return 0;
}

int charsiu_cv_signal(pthread_cond_t *c)
{
	ccv *v;

	if (!sync_srw())
		return pthread_cond_signal(c);
	v = CV(c);
	InterlockedIncrement(&v->seq);
	WakeConditionVariable(&v->cv);
	return 0;
}

int charsiu_cv_broadcast(pthread_cond_t *c)
{
	ccv *v;

	if (!sync_srw())
		return pthread_cond_broadcast(c);
	v = CV(c);
	InterlockedIncrement(&v->seq);
	WakeAllConditionVariable(&v->cv);
	return 0;
}
