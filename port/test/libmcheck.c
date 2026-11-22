/*
 * libmcheck.c - port/libm against this host's glibc
 *
 * expf, sinf and cosf on every one of the 2^32 float inputs, powf on the
 * RoPE inputs of real models (base^(-2i/hd)) and 200 million random pairs.
 * Run on aarch64 Linux, whose glibc is what charsiu's Linux reference links;
 * the Windows build bundles port/libm built the same way, so that a greedy
 * token cannot turn on a last-bit difference between UCRT and glibc. The
 * port/libm functions are renamed aor_* (Makefile, objcopy).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

float aor_expf(float), aor_sinf(float), aor_cosf(float), aor_powf(float, float);

#define NTH 8

static uint32_t bits(float f)
{
	uint32_t x;

	memcpy(&x, &f, 4);
	return x;
}

static int same(float a, float b)
{
	return bits(a) == bits(b) || (isnan(a) && isnan(b));
}

struct part {
	int fn;                 /* 0 exp, 1 sin, 2 cos */
	uint64_t lo, hi;
	unsigned long long bad;
};

static void *run(void *arg)
{
	struct part *p = arg;
	uint64_t i;

	for (i = p->lo; i < p->hi; i++) {
		uint32_t a = (uint32_t)i;
		float x, g, o;

		memcpy(&x, &a, 4);
		switch (p->fn) {
		case 0: g = expf(x); o = aor_expf(x); break;
		case 1: g = sinf(x); o = aor_sinf(x); break;
		default: g = cosf(x); o = aor_cosf(x); break;
		}
		if (!same(g, o)) {
			if (p->bad < 3)
				printf("  %s(%a): glibc %a, port %a\n",
				       p->fn == 0 ? "expf" : p->fn == 1 ? "sinf" : "cosf", x, g, o);
			p->bad++;
		}
	}
	return NULL;
}

int main(void)
{
	static const char *name[3] = { "expf", "sinf", "cosf" };
	static const float bases[] = { 10000.f, 100000.f, 500000.f, 1000000.f, 5000000.f };
	unsigned long long bad = 0, n = 0, tot = 0;
	uint64_t s = 88172645463325252ull;
	pthread_t th[NTH];
	struct part pt[NTH];
	int fn, t, b, hd, i;
	long k;

	for (fn = 0; fn < 3; fn++) {
		for (t = 0; t < NTH; t++) {
			pt[t].fn = fn;
			pt[t].lo = (0x100000000ull / NTH) * (uint64_t)t;
			pt[t].hi = (0x100000000ull / NTH) * (uint64_t)(t + 1);
			pt[t].bad = 0;
			pthread_create(&th[t], NULL, run, &pt[t]);
		}
		for (t = 0, bad = 0; t < NTH; t++) {
			pthread_join(th[t], NULL);
			bad += pt[t].bad;
		}
		printf("%s: %llu of 4294967296 inputs differ from glibc\n", name[fn], bad);
		tot += bad;
	}
	bad = 0;
	for (b = 0; b < 5; b++)
		for (hd = 32; hd <= 256; hd *= 2)
			for (i = 0; i < hd / 2; i++) {
				float x = -2.0f * (float)i / (float)hd;

				n++;
				bad += !same(powf(bases[b], x), aor_powf(bases[b], x));
			}
	for (k = 0; k < 200000000; k++) {
		uint32_t a, c;
		float x, y;

		s ^= s << 13;
		s ^= s >> 7;
		s ^= s << 17;
		a = (uint32_t)s;
		c = (uint32_t)(s >> 32);
		memcpy(&x, &a, 4);
		memcpy(&y, &c, 4);
		n++;
		if (!same(powf(x, y), aor_powf(x, y))) {
			if (bad < 3)
				printf("  powf(%a, %a): glibc %a, port %a\n", x, y, powf(x, y), aor_powf(x, y));
			bad++;
		}
	}
	printf("powf: %llu of %llu inputs differ from glibc\n", bad, n);
	tot += bad;
	printf("%s\n", tot ? "LIBM CHECK FAILED" : "LIBM CHECK OK");
	return tot != 0;
}
