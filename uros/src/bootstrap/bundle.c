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
 * bundle.c — stage-1 multiboot bundle parser for the bootstrap server
 * (Issue #186).
 *
 * The kernel maps the second multiboot module (mod[1]) into the
 * bootstrap task's address space and passes its base address and size
 * via "--bundle=ADDR,SIZE" on argv.  We index the table once, then
 * answer bundle_open() / bundle_list() lookups with zero-copy
 * pseudo-files backed by an internal fs_ops vtable.
 */

#define _BOOTSTRAP_BUNDLE_INTERNAL
#include "bundle.h"
#include "bootstrap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach.h>

static const struct boot_bundle_entry *bundle_entries = NULL;
static uint32_t			       bundle_n_entries = 0;
static const uint8_t		      *bundle_base = NULL;
static uint32_t			       bundle_total = 0;

/*
 * Pseudo-fs_ops backed by a region of memory inside the bundle.
 * One per opened entry; keeps a pointer + remaining size.
 */
struct bundle_priv {
	const uint8_t *data;
	uint32_t       size;
};

static int
bundle_fs_open(struct device *dev, const char *path, fs_private_t *out)
{
	(void)dev;
	(void)path;
	(void)out;
	return FS_INVALID_PARAMETER;	/* never reached: we open directly */
}

static void
bundle_fs_close(fs_private_t fp)
{
	if (fp != NULL)
		free(fp);
}

static int
bundle_fs_read(fs_private_t fp,
	       vm_offset_t off,
	       vm_offset_t addr,
	       vm_size_t   len)
{
	struct bundle_priv *p = (struct bundle_priv *)fp;

	if (p == NULL || off > p->size)
		return FS_NOT_IN_FILE;
	if (off + len > p->size)
		return FS_NOT_IN_FILE;
	memcpy((void *)addr, p->data + off, len);
	return 0;
}

static size_t
bundle_fs_size(fs_private_t fp)
{
	struct bundle_priv *p = (struct bundle_priv *)fp;
	return (p == NULL) ? 0 : (size_t)p->size;
}

static boolean_t
bundle_fs_is_dir(fs_private_t fp)
{
	(void)fp;
	return FALSE;
}

static boolean_t
bundle_fs_is_exec(fs_private_t fp)
{
	(void)fp;
	return TRUE;
}

static struct fs_ops bundle_ops = {
	.open_file		= bundle_fs_open,
	.close_file		= bundle_fs_close,
	.read_file		= bundle_fs_read,
	.file_size		= bundle_fs_size,
	.file_is_directory	= bundle_fs_is_dir,
	.file_is_executable	= bundle_fs_is_exec,
	.readdir		= NULL,
	.mount_size		= 0,
};

int
bundle_init(uint32_t base_addr, uint32_t total_size)
{
	const struct boot_bundle_header *hdr;

	if (base_addr == 0 || total_size < sizeof(*hdr))
		return -1;

	hdr = (const struct boot_bundle_header *)(uintptr_t)base_addr;
	if (hdr->magic0 != BOOT_BUNDLE_MAGIC0 ||
	    hdr->magic1 != BOOT_BUNDLE_MAGIC1) {
		printf("bundle: bad magic 0x%x 0x%x\n",
		       hdr->magic0, hdr->magic1);
		return -1;
	}
	if (hdr->version != BOOT_BUNDLE_VERSION) {
		printf("bundle: unsupported version %u\n", hdr->version);
		return -1;
	}

	uint32_t table_bytes = hdr->n_entries *
			       (uint32_t)sizeof(struct boot_bundle_entry);
	if (sizeof(*hdr) + table_bytes > total_size) {
		printf("bundle: truncated table\n");
		return -1;
	}

	bundle_base       = (const uint8_t *)hdr;
	bundle_total      = total_size;
	bundle_entries    = (const struct boot_bundle_entry *)(hdr + 1);
	bundle_n_entries  = hdr->n_entries;

	printf("bundle: %u entries at 0x%x (%u bytes)\n",
	       bundle_n_entries, base_addr, total_size);
	for (uint32_t i = 0; i < bundle_n_entries; i++)
		printf("bundle:   %-40s %8u bytes\n",
		       bundle_entries[i].name,
		       bundle_entries[i].size);
	return 0;
}

int
bundle_active(void)
{
	return bundle_entries != NULL;
}

static size_t
bundle_namelen(const char *en)
{
	size_t n = 0;
	while (n < BOOT_BUNDLE_NAME_MAX && en[n] != '\0')
		n++;
	return n;
}

static const struct boot_bundle_entry *
bundle_find(const char *name)
{
	size_t want = strlen(name);
	for (uint32_t i = 0; i < bundle_n_entries; i++) {
		const char *en = bundle_entries[i].name;
		size_t n = bundle_namelen(en);
		if (n == want && strncmp(en, name, n) == 0)
			return &bundle_entries[i];
	}
	return NULL;
}

int
bundle_open(const char *name, struct file *fp)
{
	const struct boot_bundle_entry *e;
	struct bundle_priv *p;

	if (!bundle_active() || name == NULL || fp == NULL)
		return FS_NO_ENTRY;

	e = bundle_find(name);
	if (e == NULL)
		return FS_NO_ENTRY;
	if ((vm_offset_t)e->offset + e->size > bundle_total)
		return FS_INVALID_FS;

	p = (struct bundle_priv *)malloc(sizeof(*p));
	if (p == NULL)
		return FS_INVALID_FS;
	p->data = bundle_base + e->offset;
	p->size = e->size;

	memset(fp, 0, sizeof(*fp));
	fp->f_ops = &bundle_ops;
	fp->f_private = p;
	return 0;
}

/*
 * List entries whose name begins with "<prefix>/" and whose remaining
 * tail contains no further slash.  Pack the tails as NUL-terminated
 * strings into out[0..out_max) and report the byte count and entry
 * count.  Used by do_bootstrap_list_modules to enumerate
 * modules/<class>/.
 */
int
bundle_list(const char *prefix,
	    char *out, uint32_t out_max,
	    uint32_t *out_used, uint32_t *out_count)
{
	uint32_t count = 0, used = 0;
	size_t plen;

	if (out_used != NULL)
		*out_used = 0;
	if (out_count != NULL)
		*out_count = 0;
	if (!bundle_active() || prefix == NULL)
		return -1;

	plen = strlen(prefix);
	for (uint32_t i = 0; i < bundle_n_entries; i++) {
		const char *en = bundle_entries[i].name;
		size_t n = bundle_namelen(en);
		if (n <= plen + 1)
			continue;
		if (strncmp(en, prefix, plen) != 0 || en[plen] != '/')
			continue;
		const char *tail = en + plen + 1;
		boolean_t has_slash = FALSE;
		for (const char *q = tail; *q != '\0'; q++)
			if (*q == '/') { has_slash = TRUE; break; }
		if (has_slash)
			continue;
		size_t tlen = strlen(tail) + 1;
		if (used + tlen > out_max)
			return -1;	/* caller's buffer too small */
		memcpy(out + used, tail, tlen);
		used += (uint32_t)tlen;
		count++;
	}

	if (out_used != NULL)
		*out_used = used;
	if (out_count != NULL)
		*out_count = count;
	return 0;
}
