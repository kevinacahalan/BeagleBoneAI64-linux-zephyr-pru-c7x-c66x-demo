/* SPDX-License-Identifier: MIT */
/*
 * ai64 trace <core>
 *   Follows a core's remoteproc trace log, such as c66x0 or 4d80800000.dsp.
 *   It reads the buffer through /dev/mem, so it needs no root and no
 *   debugfs.
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ai64.h"

/*
 * The SoC address of device address da on core c. The trace buffer address
 * in a resource table is a device address: the core's own view of memory.
 * A DSP's L2 SRAM maps to a SoC window. An ICSSG core sees its own slice's
 * data RAM at 0x0000, the other slice's at 0x2000, and its ICSSG's shared
 * RAM at 0x10000; in the SoC, data RAM 0 (slice 0's) is at the ICSSG's
 * base, data RAM 1 at base + 0x2000, and shared RAM at base + 0x10000.
 * Anything else (DDR) is the same address on both sides.
 */
static uint64_t core_da_to_pa(const struct ai64_core *c, uint32_t da)
{
	if (c->l2_len && da >= c->l2_da && da - c->l2_da < c->l2_len) {
		return c->l2_pa + (da - c->l2_da);
	}
	if (c->icssg) {
		if (da < 0x2000) {
			return c->icssg + 0x2000u * c->slice + da;
		}
		if (da < 0x4000) {
			return c->icssg + 0x2000u * (1u - c->slice) + (da - 0x2000u);
		}
		if (da >= 0x10000 && da < 0x20000) {
			return c->icssg + da;
		}
	}
	return da;
}

/*
 * Find the first trace entry in the .resource_table section of an ELF
 * file. Handles 32-bit (C66x, R5F, PRU) and 64-bit (C7x) little-endian ELF.
 */
static int elf_trace(const char *file, uint32_t *da, uint32_t *len)
{
	FILE *f = fopen(file, "rb");
	unsigned char *img = NULL;
	long size;
	int ret = -1;
	int is64;
	uint64_t shoff;
	unsigned shentsize, shnum, shstrndx, i;

	if (!f) {
		perror(file);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	img = malloc((size_t)size);
	if (!img || fread(img, 1, (size_t)size, f) != (size_t)size ||
	    size < 64 || memcmp(img, "\177ELF", 4)) {
		goto out;
	}
	is64 = img[4] == 2;
#define U16(o) ((uint32_t)img[o] | (uint32_t)img[(o) + 1] << 8)
#define U32(o) (U16(o) | U16((o) + 2) << 16)
#define U64(o) ((uint64_t)U32(o) | (uint64_t)U32((o) + 4) << 32)
	shoff = is64 ? U64(0x28) : U32(0x20);
	shentsize = is64 ? U16(0x3a) : U16(0x2e);
	shnum = is64 ? U16(0x3c) : U16(0x30);
	shstrndx = is64 ? U16(0x3e) : U16(0x32);
	if (shoff + (uint64_t)shnum * shentsize > (uint64_t)size || shstrndx >= shnum) {
		goto out;
	}
	{
		uint64_t s = shoff + (uint64_t)shstrndx * shentsize;
		uint64_t stroff = is64 ? U64(s + 0x18) : U32(s + 0x10);

		for (i = 0; i < shnum; i++) {
			uint64_t h = shoff + (uint64_t)i * shentsize;
			uint64_t off = is64 ? U64(h + 0x18) : U32(h + 0x10);
			uint64_t sz = is64 ? U64(h + 0x20) : U32(h + 0x14);
			uint32_t num, k;

			if (stroff + U32(h) >= (uint64_t)size ||
			    strcmp((const char *)img + stroff + U32(h), ".resource_table") ||
			    off + sz > (uint64_t)size || sz < 16) {
				continue;
			}
			num = U32(off + 4);
			for (k = 0; k < num && 16 + 4 * (k + 1) <= sz; k++) {
				uint64_t e = off + U32(off + 16 + 4 * k);

				if (e + 16 <= off + sz && U32(e) == 2) { /* RSC_TRACE */
					*da = U32(e + 4);
					*len = U32(e + 8);
					ret = 0;
					goto out;
				}
			}
		}
	}
#undef U64
#undef U32
#undef U16
out:
	if (ret) {
		fprintf(stderr, "%s has no trace in its resource table\n", file);
	}
	free(img);
	fclose(f);
	return ret;
}

int trace_main(int argc, char **argv)
{
	const struct ai64_core *c;
	const char *name;
	char fw[128], path[256];
	uint32_t da, len, shown = 0;
	char state[32];
	uint64_t pa;
	volatile char *buf;
	long page = sysconf(_SC_PAGESIZE);
	char *mapped;
	int fd;

	if (argc != 3) {
		fprintf(stderr, "usage: %s trace <core>, such as c66x0 or 4d80800000.dsp\n", argv[0]);
		return 2;
	}
	c = core_find(argv[2]);
	if (!c) {
		fprintf(stderr, "%s is not a demo core; ai64 cores lists them\n", argv[2]);
		return 2;
	}
	name = c->rproc;
	if (rproc_attr(name, "firmware", fw, sizeof(fw))) {
		fprintf(stderr, "no remoteproc called %s\n", name);
		return 1;
	}
	/* A powered-off DSP's RAM hangs the SoC when read. */
	if (!rproc_running(name, state, sizeof(state))) {
		fprintf(stderr, "%s is %s; start it first (ai64-demo does)\n", name, state);
		return 1;
	}
	snprintf(path, sizeof(path), "/lib/firmware/%s", fw);
	if (elf_trace(path, &da, &len)) {
		return 1;
	}
	pa = core_da_to_pa(c, da);
	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	mapped = mmap(NULL, len + (pa % page), PROT_READ, MAP_SHARED, fd, (off_t)(pa - pa % page));
	close(fd);
	if (mapped == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	buf = (volatile char *)(mapped + pa % page);
	printf("%s trace (%s), %u bytes at 0x%llx\n", name, fw, len, (unsigned long long)pa);
	fflush(stdout);
	for (;;) {
		uint32_t n = 0;

		while (n < len && buf[n]) {
			n++;
		}
		/* The firmware starts the log again from the top when it is full. */
		if (n < shown) {
			shown = 0;
		}
		for (; shown < n; shown++) {
			putchar(buf[shown]);
		}
		fflush(stdout);
		usleep(200000);
	}
}
