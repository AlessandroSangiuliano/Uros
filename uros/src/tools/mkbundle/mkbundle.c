/*
 * MIT License
 *
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so.
 */

/*
 * mkbundle — host tool that builds a stage-1 multiboot bundle for the
 * Uros bootstrap server (Issue #186).
 *
 * Usage:
 *   mkbundle -o <bundle.bin> <name>:<file> [<name>:<file> ...]
 *
 * Each <name>:<file> argument adds <file>'s contents to the bundle
 * under the logical entry name <name>.  Names use forward slashes for
 * subdirectories (e.g. modules/blk/ahci.so) and must fit in
 * BOOT_BUNDLE_NAME_MAX bytes.
 *
 * The output is a flat little-endian file consumed by bundle.c in
 * the bootstrap server.
 */

#include "bundle.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct entry {
	char     name[BOOT_BUNDLE_NAME_MAX];
	char    *path;
	uint32_t size;
	uint32_t offset;
};

static void
usage(const char *argv0)
{
	fprintf(stderr,
	    "usage: %s -o <bundle.bin> <name>:<file> [...]\n",
	    argv0);
	exit(2);
}

static uint32_t
align_up(uint32_t v, uint32_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static uint32_t
file_size(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0) {
		fprintf(stderr, "mkbundle: stat(%s): %s\n",
		    path, strerror(errno));
		exit(1);
	}
	if (st.st_size > UINT32_MAX) {
		fprintf(stderr, "mkbundle: %s too large (%lld bytes)\n",
		    path, (long long)st.st_size);
		exit(1);
	}
	return (uint32_t)st.st_size;
}

static void
copy_payload(FILE *out, const char *path, uint32_t size)
{
	FILE *in = fopen(path, "rb");
	if (in == NULL) {
		fprintf(stderr, "mkbundle: open(%s): %s\n",
		    path, strerror(errno));
		exit(1);
	}

	char buf[8192];
	uint32_t left = size;
	while (left > 0) {
		size_t want = (left > sizeof(buf)) ? sizeof(buf) : left;
		size_t got = fread(buf, 1, want, in);
		if (got == 0) {
			fprintf(stderr,
			    "mkbundle: short read on %s (%u left)\n",
			    path, left);
			exit(1);
		}
		if (fwrite(buf, 1, got, out) != got) {
			fprintf(stderr, "mkbundle: write failed: %s\n",
			    strerror(errno));
			exit(1);
		}
		left -= (uint32_t)got;
	}
	fclose(in);
}

static void
pad_to(FILE *out, uint32_t target, uint32_t cur)
{
	static const char zeros[16] = {0};
	while (cur < target) {
		uint32_t step = target - cur;
		if (step > sizeof(zeros))
			step = sizeof(zeros);
		fwrite(zeros, 1, step, out);
		cur += step;
	}
}

int
main(int argc, char **argv)
{
	const char *out_path = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			out_path = argv[++i];
		} else if (argv[i][0] == '-') {
			usage(argv[0]);
		} else {
			break;
		}
	}
	if (out_path == NULL || i >= argc)
		usage(argv[0]);

	int n = argc - i;
	struct entry *ents = calloc((size_t)n, sizeof(*ents));
	if (ents == NULL) {
		perror("mkbundle: calloc");
		return 1;
	}

	for (int k = 0; k < n; k++) {
		const char *arg = argv[i + k];
		const char *colon = strchr(arg, ':');
		if (colon == NULL || colon == arg) {
			fprintf(stderr,
			    "mkbundle: bad spec '%s' (need name:path)\n",
			    arg);
			return 1;
		}
		size_t nlen = (size_t)(colon - arg);
		if (nlen >= BOOT_BUNDLE_NAME_MAX) {
			fprintf(stderr,
			    "mkbundle: name '%.*s' too long (max %d)\n",
			    (int)nlen, arg, BOOT_BUNDLE_NAME_MAX - 1);
			return 1;
		}
		memcpy(ents[k].name, arg, nlen);
		ents[k].path = strdup(colon + 1);
		ents[k].size = file_size(ents[k].path);
	}

	uint32_t header_bytes = (uint32_t)sizeof(struct boot_bundle_header);
	uint32_t table_bytes  = (uint32_t)(n * sizeof(struct boot_bundle_entry));
	uint32_t cursor = align_up(header_bytes + table_bytes,
				   BOOT_BUNDLE_ALIGN);

	for (int k = 0; k < n; k++) {
		ents[k].offset = cursor;
		cursor = align_up(cursor + ents[k].size, BOOT_BUNDLE_ALIGN);
	}

	FILE *out = fopen(out_path, "wb");
	if (out == NULL) {
		fprintf(stderr, "mkbundle: open(%s): %s\n",
		    out_path, strerror(errno));
		return 1;
	}

	struct boot_bundle_header hdr = {
		.magic0 = BOOT_BUNDLE_MAGIC0,
		.magic1 = BOOT_BUNDLE_MAGIC1,
		.version = BOOT_BUNDLE_VERSION,
		.n_entries = (uint32_t)n,
	};
	fwrite(&hdr, sizeof(hdr), 1, out);

	for (int k = 0; k < n; k++) {
		struct boot_bundle_entry e;
		memset(&e, 0, sizeof(e));
		memcpy(e.name, ents[k].name, BOOT_BUNDLE_NAME_MAX);
		e.offset = ents[k].offset;
		e.size   = ents[k].size;
		fwrite(&e, sizeof(e), 1, out);
	}

	uint32_t pos = header_bytes + table_bytes;
	for (int k = 0; k < n; k++) {
		pad_to(out, ents[k].offset, pos);
		pos = ents[k].offset;
		copy_payload(out, ents[k].path, ents[k].size);
		pos += ents[k].size;
	}
	pad_to(out, align_up(pos, BOOT_BUNDLE_ALIGN), pos);

	fclose(out);

	fprintf(stderr, "mkbundle: %s — %d entries, %u bytes total\n",
	    out_path, n, align_up(pos, BOOT_BUNDLE_ALIGN));
	for (int k = 0; k < n; k++)
		fprintf(stderr, "  %-40s %8u bytes @ 0x%x\n",
		    ents[k].name, ents[k].size, ents[k].offset);

	for (int k = 0; k < n; k++)
		free(ents[k].path);
	free(ents);
	return 0;
}
