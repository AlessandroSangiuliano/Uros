/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * gpu_module_abi.h — Back-end module ABI for gpu_server.
 *
 * A back-end module is a userspace shared object that implements one
 * GPU driver.  It is loaded into the gpu_server process via libdl
 * (#162) and called through the function-pointer table below.
 *
 * Each module defines exactly one exported symbol:
 *
 *     extern const gpu_module_ops_t gpu_module_entry;
 *
 * gpu_server walks installed modules in priority order and calls
 * `probe()` on each HAL-discovered display device; the first non-NULL
 * return wins.  See docs/gpu_server_design.md §4.
 *
 * Bumping GPU_MODULE_ABI_VERSION is a hard ABI break.  Adding new
 * operations is done by appending to gpu_module_ops_t and bumping the
 * version; old modules using the smaller layout will be rejected by
 * the loader (size check).
 */

#ifndef _GPU_GPU_MODULE_ABI_H_
#define _GPU_GPU_MODULE_ABI_H_

#include <stdint.h>
#include <stddef.h>
#include <gpu/gpu_types.h>

#define GPU_MODULE_ABI_VERSION		4u	/* +text_scroll scrollback (#364) */

/* ============================================================
 * Forward-declared opaque types.
 *
 * Defined privately in gpu_server core.  Modules treat them as
 * opaque pointers and call back into core via the entry-point table
 * the core supplies at attach time (see core_ops_t below).
 * ============================================================ */

struct hal_device_info;		/* hal_server.h */
struct gpu_display;
struct gpu_bo;
struct gpu_context;
struct gpu_fence;

typedef struct gpu_display	gpu_display_t;
typedef struct gpu_bo		gpu_bo_t;
typedef struct gpu_context	gpu_context_t;
typedef struct gpu_fence	gpu_fence_t;

/* ============================================================
 * Core → module entry points.
 *
 * Filled in by the module author.  Function-pointer fields that the
 * module does not implement may be NULL; gpu_server core treats a NULL
 * op as "KERN_NOT_SUPPORTED for this resource class".
 * ============================================================ */

typedef struct gpu_module_ops {
	/* Identification ------------------------------------------------- */
	const char	*name;		/* "vga", "intel_gen", ... */
	uint32_t	abi_version;	/* must == GPU_MODULE_ABI_VERSION */
	uint32_t	priority;	/* higher = tried first; 0 default */

	/* Probe — HAL hands us a display-class device.  Return non-NULL
	 * on claim (the pointer becomes the module's `priv` for this
	 * device), NULL on skip. */
	void *		(*probe)(const struct hal_device_info *dev);

	/* Lifecycle ------------------------------------------------------ */
	int		(*attach)(void *priv);
	void		(*detach)(void *priv);

	/* Display ops --------------------------------------------------- */
	uint32_t	(*display_count)(void *priv);
	gpu_display_t *	(*display_get)(void *priv, uint32_t idx);
	int		(*display_set_mode)(gpu_display_t *, const gpu_mode_t *);
	int		(*display_scanout)(gpu_display_t *, gpu_bo_t *);

	/* Buffer-object ops --------------------------------------------- */
	gpu_bo_t *	(*bo_alloc)(void *priv, size_t size, uint32_t flags);
	void		(*bo_free)(gpu_bo_t *);
	int		(*bo_map)(gpu_bo_t *, void **out_kva);

	/* Submission ---------------------------------------------------- */
	int		(*submit)(void *priv, gpu_context_t *,
				  const void *cmdbuf, size_t len,
				  gpu_fence_id_t *out_fence);

	/* Text fast-path (0.1.0+).  `surface` selects one of the module's
	 * virtual text surfaces (#204/#364): surface 0 = the system
	 * console; higher ids = additional on-screen terminals (VTs).  Each
	 * surface has its own off-screen cell grid + cursor; only the
	 * active one is scanned out.  A single-surface module ignores a
	 * non-zero `surface` (output still lands, on surface 0). */
	int		(*text_puts)(void *priv, uint32_t surface,
				     const char *buf, size_t len);

	/* Diagnostic counters (ABI 2, #203).  Optional — return 0 / NULL
	 * if the module doesn't have anything meaningful.  Summed across
	 * all attached devices by gpu_query_stats. */
	uint64_t	(*get_scroll_count)(void *priv);

	/* Virtual text surfaces (ABI 3, #204/#364). ---------------------- */

	/* Number of text surfaces the module keeps (>= 1).  NULL ⇒ 1. */
	uint32_t	(*text_surface_count)(void *priv);

	/* Make `surface` the one scanned out to the physical display — the
	 * VT switch.  Repaints hardware from that surface's cell grid.
	 * NULL ⇒ single-surface module, switching is a no-op. */
	int		(*text_set_active)(void *priv, uint32_t surface);

	/* Scroll the on-screen surface through its scrollback by `delta`
	 * rows (> 0 = up into history, < 0 = down toward live output).  New
	 * output snaps the view back to the bottom.  NULL ⇒ no scrollback
	 * (ABI 4, #364). */
	int		(*text_scroll)(void *priv, int delta);
} gpu_module_ops_t;

/* ============================================================
 * Module → core callbacks.
 *
 * Supplied by core to the module at attach time via the `core` argument
 * the module receives in attach().  The module stashes the pointer in
 * its priv state.  This split keeps the module side a pure consumer of
 * a fixed v-table — no symbol lookups, no late binding, no surprises
 * if/when modules move into separate tasks (see design doc §4
 * "Module isolation in 0.2+").
 *
 * 0.1.0 only declares the shape; implementations land with #195/#196.
 * ============================================================ */

typedef struct gpu_core_ops {
	uint32_t	abi_version;

	/* Async event delivery (module → core).  Called from the IRQ
	 * worker thread the module spawned via core_irq_register. */
	void		(*irq_done)(gpu_context_t *, gpu_fence_id_t);
	void		(*vsync)(gpu_display_t *);
	void		(*hotunplug)(void *priv);

	/* Logging — modules MUST go through here so prints get tagged
	 * with the module name and routed off the user console. */
	void		(*log)(const char *module, const char *fmt, ...);
} gpu_core_ops_t;

/* ============================================================
 * The single exported symbol every module must define.
 * ============================================================ */

#define GPU_MODULE_ENTRY_SYMBOL		"gpu_module_entry"

#endif /* _GPU_GPU_MODULE_ABI_H_ */
