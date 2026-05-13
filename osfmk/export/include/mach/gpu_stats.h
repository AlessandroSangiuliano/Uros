/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * mach/gpu_stats.h — Wire format for gpu_query_stats (#203).
 *
 * Returned inline (fixed-size, no OOL) by gpu_query_stats.  The
 * MIG-generated server stub passes the buffer as a `char[]` of the
 * declared bound; callers cast it to `struct gpu_stats *`.  The
 * `reserved` tail keeps the struct forward-compatible: new counters
 * may consume reserved slots without breaking ABI as long as the
 * total size stays at GPU_STATS_SIZE.
 *
 * Cap requirement: GPU_CAP_DEV_ADMIN.  This is a diagnostic surface,
 * not for general clients.
 */

#ifndef _MACH_GPU_STATS_H_
#define _MACH_GPU_STATS_H_

#include <stdint.h>

#define GPU_STATS_VERSION	1u
#define GPU_STATS_SIZE		64u

struct gpu_stats {
	uint32_t	version;			/* GPU_STATS_VERSION */
	uint32_t	devices_attached;
	uint64_t	text_render_drops;
	uint64_t	text_render_chunks_processed;
	uint64_t	scroll_count;
	uint64_t	reserved[4];
};

#endif /* _MACH_GPU_STATS_H_ */
