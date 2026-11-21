// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Jiaxing Hu <gahing@gahingwoo.com>
/*
 * m0_peek: read 32-bit registers through /dev/mem, and nothing else.
 *
 *   m0_peek NAME=ADDRESS [NAME=ADDRESS ...]
 *
 * Prints one "NAME ADDRESS VALUE" line per register. It cannot write: the
 * device is opened O_RDONLY and every page is mapped PROT_READ, so a store
 * through the mapping would fault in this process before reaching the bus.
 *
 * It does not know which addresses are safe to read. A read of a block whose
 * power domain is off, or whose clock is gated, is an asynchronous SError on
 * the RK3576 and takes the kernel down. m0_snapshot.sh checks the domains and
 * clocks before it calls this; call it by hand only after the same checks.
 * Never point it at a QoS register: those hang the SoC from userspace even
 * with the domain on (their clocks are only enabled inside the PM driver).
 *
 * Build: gcc -O2 -Wall -static -o m0_peek m0_peek.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	long pg = sysconf(_SC_PAGESIZE);
	int fd, i, rc = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: %s NAME=ADDRESS ...\n", argv[0]);
		return 2;
	}
	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "m0_peek: /dev/mem: %s\n", strerror(errno));
		return 1;
	}
	for (i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		uint64_t addr, base;
		volatile uint32_t *p;
		void *map;

		if (!eq) {
			fprintf(stderr, "m0_peek: %s is not NAME=ADDRESS\n", argv[i]);
			rc = 2;
			continue;
		}
		*eq = '\0';
		addr = strtoull(eq + 1, NULL, 0);
		if (addr & 3) {
			fprintf(stderr, "m0_peek: %s 0x%" PRIx64 " is not 4-byte aligned\n", argv[i], addr);
			rc = 2;
			continue;
		}
		base = addr & ~(uint64_t)(pg - 1);
		map = mmap(NULL, pg, PROT_READ, MAP_SHARED, fd, (off_t)base);
		if (map == MAP_FAILED) {
			/*
			 * EPERM here is CONFIG_IO_STRICT_DEVMEM refusing a range a
			 * driver has claimed (rocket claims the NPU's). Boot with
			 * iomem=relaxed for this session.
			 */
			printf("%-28s 0x%08" PRIx64 " mmap-failed:%s\n", argv[i], addr, strerror(errno));
			rc = 1;
			continue;
		}
		p = (volatile uint32_t *)((char *)map + (addr - base));
		printf("%-28s 0x%08" PRIx64 " 0x%08" PRIx32 "\n", argv[i], addr, *p);
		munmap(map, pg);
	}
	close(fd);
	return rc;
}
