/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * dl_loader.c — Dynamic module loading for block_device_server.
 *
 * The BDS is linked as a PIE executable with --export-dynamic and
 * --hash-style=sysv so every symbol it defines (MIG stubs, libc,
 * Mach VM/port/IPC, stack-protector, etc.) shows up in its own .dynsym.
 * dl_bootstrap_self() hooks the running image into libdl's loaded-
 * object list; dlopen'd modules then resolve their undefined symbols
 * against it automatically — no hand-maintained host symbol array.
 *
 * Phase 1 I/O: modules are embedded in the executable as binary blobs
 * via objcopy (symbols _binary_<name>_so_start / _binary_<name>_so_size).
 * Future: read from ext2 filesystem after mount.
 */

#include <mach.h>
#include <stdio.h>
#include <string.h>
#include "block_server.h"
#include "dl_loader.h"
#include "dlfcn.h"
#include "dl_internal.h"

/* ================================================================
 * Embedded module blobs (from objcopy)
 *
 * For each .so, the linker provides:
 *   _binary_ahci_so_start, _binary_ahci_so_end, _binary_ahci_so_size
 *   _binary_virtio_blk_so_start, ...
 * ================================================================ */

extern const char _binary_ahci_so_start[];
extern const char _binary_ahci_so_end[];
extern const char _binary_virtio_blk_so_start[];
extern const char _binary_virtio_blk_so_end[];

struct embedded_module {
	const char	*path;
	const char	*data;
	const char	*data_end;
};

static const struct embedded_module embedded_modules[] = {
	{ "ahci.so",
	  _binary_ahci_so_start,
	  _binary_ahci_so_end },
	{ "virtio_blk.so",
	  _binary_virtio_blk_so_start,
	  _binary_virtio_blk_so_end },
	{ NULL, NULL, NULL }
};

/* ================================================================
 * I/O callback: read from embedded blobs
 * ================================================================ */

static int
embedded_read_file(const char *path, void **buf, unsigned int *size)
{
	const struct embedded_module *em;
	const char *basename;
	vm_offset_t copy;
	vm_size_t blobsize;
	kern_return_t kr;

	/* Extract basename from path (e.g., "/modules/ahci.so" → "ahci.so") */
	basename = path;
	{
		const char *p = path;
		while (*p != '\0') {
			if (*p == '/')
				basename = p + 1;
			p++;
		}
	}

	for (em = embedded_modules; em->path != NULL; em++) {
		if (strcmp(basename, em->path) == 0) {
			blobsize = (vm_size_t)(em->data_end - em->data);

			/* Allocate a copy — libdl needs a writable buffer */
			copy = 0;
			kr = vm_allocate(mach_task_self(), &copy,
					 blobsize, TRUE);
			if (kr != KERN_SUCCESS)
				return -1;

			memcpy((void *)copy, em->data, blobsize);
			*buf = (void *)copy;
			*size = (unsigned int)blobsize;
			return 0;
		}
	}

	printf("blk: module \"%s\" not found in embedded blobs\n", path);
	return -1;
}

static void
embedded_free_buf(void *buf, unsigned int size)
{
	vm_deallocate(mach_task_self(), (vm_offset_t)buf, size);
}

static const struct dl_io_ops embedded_io_ops = {
	.read_file = embedded_read_file,
	.free_buf  = embedded_free_buf,
};

/* ================================================================
 * Public API
 * ================================================================ */

void
blk_dl_init(void)
{
	dl_set_io_ops(&embedded_io_ops);

	if (dl_bootstrap_self() != 0) {
		printf("blk: dl_bootstrap_self failed: %s\n", dlerror());
		printf("blk: module symbol resolution will fail\n");
		return;
	}

	printf("blk: dynamic module loader initialised "
	       "(%d embedded modules, self-bootstrap OK)\n",
	       (int)(sizeof(embedded_modules) / sizeof(embedded_modules[0])) - 1);
}

const struct block_driver_ops *
blk_dl_load_module(const char *path)
{
	void *handle;
	const struct block_driver_ops *ops;
	const char *sym_name;
	const char *basename;

	handle = dlopen(path, RTLD_NOW);
	if (handle == NULL) {
		printf("blk: dlopen(\"%s\") failed: %s\n",
		       path, dlerror());
		return NULL;
	}

	/*
	 * Derive the ops symbol name from the module filename.
	 * "ahci.so" → "ahci_module_ops"
	 * "virtio_blk.so" → "virtio_blk_module_ops"
	 *
	 * We build the symbol name in a small buffer.
	 */
	basename = path;
	{
		const char *p = path;
		while (*p != '\0') {
			if (*p == '/')
				basename = p + 1;
			p++;
		}
	}

	{
		char sym_buf[64];
		unsigned int len = 0;
		const char *p = basename;

		while (*p != '\0' && *p != '.' && len < sizeof(sym_buf) - 13) {
			sym_buf[len++] = *p++;
		}
		memcpy(sym_buf + len, "_module_ops", 12); /* includes NUL */
		sym_name = sym_buf;

		ops = (const struct block_driver_ops *)dlsym(handle, sym_name);
	}

	if (ops == NULL) {
		printf("blk: dlsym(\"%s\") failed: %s\n",
		       sym_name, dlerror());
		dlclose(handle);
		return NULL;
	}

	printf("blk: loaded module \"%s\" (%s)\n", path, ops->name);
	return ops;
}

void
blk_dl_unload_module(const char *path)
{
	/* For future use — currently modules are never unloaded at boot */
	(void)path;
}
