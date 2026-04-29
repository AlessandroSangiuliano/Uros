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
 * bundle.h — stage-1 multiboot bundle format (Issue #186).
 *
 * The bundle is a flat name -> bytes archive passed as the second
 * multiboot module (mod[1]).  It carries the early-stage server
 * binaries and driver .so plug-ins that the bootstrap server needs
 * before block_device_server is up — replacing the in-kernel IDE
 * driver as the source of stage-1 binaries.
 *
 * Layout (little-endian, 32-bit fields):
 *
 *   offset 0       struct boot_bundle_header
 *   offset 16      n_entries * struct boot_bundle_entry
 *   offset ...     payloads, each at entry.offset (16-byte aligned)
 *
 * Entry names are paths relative to /mach_servers/ on the would-be
 * boot filesystem, e.g.:
 *
 *   "bootstrap.conf"
 *   "name_server"
 *   "modules/blk/ahci.so"
 *   "modules/hal/pci_scan.so"
 */

#ifndef _BOOTSTRAP_BUNDLE_H_
#define _BOOTSTRAP_BUNDLE_H_

#include <stdint.h>

#define BOOT_BUNDLE_MAGIC0	0x534f5255U	/* "UROS" */
#define BOOT_BUNDLE_MAGIC1	0x4c444e42U	/* "BNDL" */
#define BOOT_BUNDLE_VERSION	1
#define BOOT_BUNDLE_NAME_MAX	56
#define BOOT_BUNDLE_ALIGN	16

struct boot_bundle_header {
	uint32_t magic0;	/* BOOT_BUNDLE_MAGIC0 */
	uint32_t magic1;	/* BOOT_BUNDLE_MAGIC1 */
	uint32_t version;	/* BOOT_BUNDLE_VERSION */
	uint32_t n_entries;
};

struct boot_bundle_entry {
	char     name[BOOT_BUNDLE_NAME_MAX];	/* NUL-padded */
	uint32_t offset;	/* from start of bundle, 16-byte aligned */
	uint32_t size;		/* payload bytes */
};

/* In-bootstrap parsed view of the bundle (built once at startup). */
struct file;

extern int  bundle_init(uint32_t base_addr, uint32_t total_size);
extern int  bundle_open(const char *name, struct file *fp);
extern int  bundle_list(const char *prefix,
			char *out, uint32_t out_max, uint32_t *out_used,
			uint32_t *out_count);
extern int  bundle_active(void);

#endif /* _BOOTSTRAP_BUNDLE_H_ */
