/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * gpu_types.h — Public types shared between gpu_server, its back-end
 * modules, and gpu_server clients.
 *
 * See docs/gpu_server_design.md §3 (resource model) and §5 (capability
 * model).  This header is part of the stable export surface; bumping any
 * of the GPU_*_VERSION constants signals a non-backwards-compatible
 * change.
 */

#ifndef _GPU_GPU_TYPES_H_
#define _GPU_GPU_TYPES_H_

#include <stdint.h>

/* ============================================================
 * Versioning
 * ============================================================ */

#define GPU_TYPES_VERSION	1u

/* ============================================================
 * Opaque handles passed across the Mach control plane.
 *
 * A `gpu_handle_t` is server-local (per gpu_server task instance).
 * Clients keep them as opaque integers; revocation invalidates them.
 * ============================================================ */

typedef uint32_t	gpu_dev_id_t;	/* index into gpu_server device table */
typedef uint32_t	gpu_disp_id_t;	/* per-device display index */
typedef uint32_t	gpu_bo_id_t;	/* per-context buffer-object index */
typedef uint32_t	gpu_ctx_id_t;	/* per-task context index */
typedef uint64_t	gpu_fence_id_t;	/* monotonic per-context fence */

/* ============================================================
 * Display modes
 *
 * The 0.1.0 vga back-end exposes a single fixed text mode and (when
 * BGA/Bochs DISPI is wired up later) one linear framebuffer mode.
 * Future drivers will report a list of these.
 * ============================================================ */

#define GPU_MODE_TEXT		0u	/* width=cols, height=rows, bpp=0 */
#define GPU_MODE_LINEAR		1u	/* width/height in pixels, bpp matters */

typedef struct gpu_mode {
	uint32_t	kind;		/* GPU_MODE_TEXT | GPU_MODE_LINEAR */
	uint32_t	width;
	uint32_t	height;
	uint32_t	bpp;		/* 0 for text, 8/16/24/32 for linear */
	uint32_t	refresh_mhz;	/* 0 if unknown / N/A */
	uint32_t	flags;
} gpu_mode_t;

/* ============================================================
 * Buffer-object flags
 *
 * The 0.1.0 vga back-end ignores everything except SCANOUT (text mode
 * has a single implicit BO at 0xB8000).  Real drivers will care.
 * ============================================================ */

#define GPU_BO_FLAG_SCANOUT		(1u << 0)
#define GPU_BO_FLAG_LINEAR		(1u << 1)
#define GPU_BO_FLAG_TILED		(1u << 2)
#define GPU_BO_FLAG_CACHED		(1u << 3)	/* default: WC */
#define GPU_BO_FLAG_HOST_VISIBLE	(1u << 4)

/* ============================================================
 * Capability ops mask — see §5.
 *
 * Carried in the `ops` field of a uros_cap token (cap_server) and
 * checked by gpu_server on every Mach RPC that targets a resource.
 * Layout pinned now so the cap_server policy table can reference these
 * constants directly.
 * ============================================================ */

#define GPU_CAP_DEV_OPEN		(1ull << 0)
#define GPU_CAP_DEV_ADMIN		(1ull << 1)
#define GPU_CAP_DISPLAY_SCANOUT		(1ull << 2)
#define GPU_CAP_DISPLAY_MODESET		(1ull << 3)
#define GPU_CAP_BO_RW			(1ull << 4)
#define GPU_CAP_BO_SCANOUT		(1ull << 5)
#define GPU_CAP_BO_EXPORT		(1ull << 6)
#define GPU_CAP_CTX_SUBMIT		(1ull << 7)
#define GPU_CAP_CTX_DEBUG		(1ull << 8)
#define GPU_CAP_VBLANK_SUB		(1ull << 9)

#define GPU_CAP_RESOURCE_TYPE		0x47505500u	/* 'GPU\0' */

/* ============================================================
 * Device info (returned by gpu_query_displays / gpu_query_devices)
 *
 * POD layout, written back-to-back into an OOL buffer in MIG calls,
 * so clients can iterate without MIG typing help.
 * ============================================================ */

#define GPU_DEV_NAME_LEN		32

struct gpu_device_info {
	gpu_dev_id_t	id;
	uint32_t	pci_bdf;	/* (bus<<16)|(slot<<8)|func, 0 if non-PCI */
	uint32_t	vendor_device;	/* PCI 0x00, 0 if non-PCI */
	uint32_t	n_displays;
	char		module_name[GPU_DEV_NAME_LEN];	/* "vga", "intel_gen", ... */
};

#endif /* _GPU_GPU_TYPES_H_ */
