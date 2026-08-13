/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Which filesystems this machine's boot path can read (#422).
 *
 * ⚠️ This file exists per machine, and it took a check to be sure it should.
 * Ten lines of table look machine-independent, and the obvious move was to
 * lift it out of AT386/ into the shared directory the way parse_args went.
 * The three existing copies say otherwise:
 *
 *	AT386     ufs, ext2fs, minixfs
 *	HP700     ufs, ext2fs, bext2fs
 *	POWERMAC  minixfs, ext2fs, bext2fs, hfs
 *
 * Different sets and different order -- HFS on a Macintosh, big-endian ext2 on
 * a PA-RISC.  What the table encodes is which media a machine is expected to
 * boot from, which is a decision about the machine and not about the code.
 *
 * So this is AT386's list, because this machine boots from the same media a
 * PC does, and the order is AT386's for the same reason: fs_switch is tried in
 * order, so it is a preference and not a set.
 */

#include "file_system.h"

extern struct fs_ops	ufs_ops;
extern struct fs_ops	ext2fs_ops;
extern struct fs_ops	minixfs_ops;

fs_ops_t fs_switch[] = {
	&ufs_ops,
	&ext2fs_ops,
	&minixfs_ops,
	0
};
