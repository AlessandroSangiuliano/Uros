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
 * Module source: the bootstrap server publishes plug-in .so files under
 *   /mach_servers/modules/<class>/
 * on the boot filesystem.  At init time we call bootstrap_list_modules
 * and bootstrap_get_module over MIG to pull the bytes into a small
 * in-memory cache, which libdl's read_file callback then serves to
 * dlopen().
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/bootstrap.h>
#include <mach/module_pool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "block_server.h"
#include "dl_loader.h"
#include "dlfcn.h"
#include "dl_internal.h"

/* ================================================================
 * Cache of modules fetched from the bootstrap over MIG
 * ================================================================ */

#define MAX_CACHED_MODULES	16

struct cached_module {
	char		name[MODULE_NAME_MAX];
	vm_offset_t	data;		/* vm_allocate'd by bootstrap */
	vm_size_t	size;		/* bytes in .so */
};

static struct cached_module	cached_modules[MAX_CACHED_MODULES];
static int			n_cached_modules;

static struct cached_module *
find_cached_module(const char *name)
{
	int i;
	for (i = 0; i < n_cached_modules; i++) {
		if (strcmp(cached_modules[i].name, name) == 0)
			return &cached_modules[i];
	}
	return NULL;
}

/* ================================================================
 * I/O callback: serve dlopen() reads from the cache
 * ================================================================ */

static int
cache_read_file(const char *path, void **buf, unsigned int *size)
{
	const char *basename;
	const struct cached_module *cm;
	vm_offset_t copy;
	kern_return_t kr;

	basename = path;
	{
		const char *p = path;
		while (*p != '\0') {
			if (*p == '/')
				basename = p + 1;
			p++;
		}
	}

	cm = find_cached_module(basename);
	if (cm == NULL) {
		printf("blk: module \"%s\" not in cache\n", basename);
		return -1;
	}

	copy = 0;
	kr = vm_allocate(mach_task_self(), &copy, cm->size, TRUE);
	if (kr != KERN_SUCCESS)
		return -1;

	memcpy((void *)copy, (const void *)cm->data, cm->size);
	*buf = (void *)copy;
	*size = (unsigned int)cm->size;
	return 0;
}

static void
cache_free_buf(void *buf, unsigned int size)
{
	vm_deallocate(mach_task_self(), (vm_offset_t)buf, size);
}

static const struct dl_io_ops cache_io_ops = {
	.read_file = cache_read_file,
	.free_buf  = cache_free_buf,
};

/* ================================================================
 * Fetch the module pool for our class from the bootstrap
 * ================================================================ */

static int
fetch_class(const char *class)
{
	kern_return_t kr;
	vm_offset_t names = 0;
	mach_msg_type_number_t names_count = 0;
	module_name_t class_buf;
	const char *p;
	int added = 0;

	strncpy(class_buf, class, sizeof(class_buf) - 1);
	class_buf[sizeof(class_buf) - 1] = '\0';

	kr = bootstrap_list_modules(bootstrap_port, class_buf,
				    &names, &names_count);
	if (kr != KERN_SUCCESS) {
		printf("blk: bootstrap_list_modules(\"%s\") failed: 0x%x\n",
		       class, kr);
		return 0;
	}

	for (p = (const char *)names; p < (const char *)names + names_count; ) {
		size_t len = strlen(p);
		if (len == 0)
			break;

		if (n_cached_modules >= MAX_CACHED_MODULES) {
			printf("blk: module cache full, dropping \"%s\"\n", p);
			break;
		}

		{
			module_name_t name_buf;
			vm_offset_t data = 0;
			mach_msg_type_number_t data_count = 0;
			struct cached_module *cm;

			strncpy(name_buf, p, sizeof(name_buf) - 1);
			name_buf[sizeof(name_buf) - 1] = '\0';

			kr = bootstrap_get_module(bootstrap_port, class_buf,
						  name_buf, &data, &data_count);
			if (kr != KERN_SUCCESS) {
				printf("blk: bootstrap_get_module"
				       "(\"%s\", \"%s\") failed: 0x%x\n",
				       class, p, kr);
				p += len + 1;
				continue;
			}

			cm = &cached_modules[n_cached_modules++];
			strncpy(cm->name, p, sizeof(cm->name) - 1);
			cm->name[sizeof(cm->name) - 1] = '\0';
			cm->data = data;
			cm->size = data_count;
			added++;
		}

		p += len + 1;
	}

	if (names != 0)
		vm_deallocate(mach_task_self(), names, names_count);

	return added;
}

/* ================================================================
 * Public API
 * ================================================================ */

void
blk_dl_init(void)
{
	dl_set_io_ops(&cache_io_ops);

	if (dl_bootstrap_self() != 0) {
		printf("blk: dl_bootstrap_self failed: %s\n", dlerror());
		printf("blk: module symbol resolution will fail\n");
		return;
	}

	printf("blk: dynamic module loader initialised "
	       "(self-bootstrap OK)\n");
}

int
blk_dl_load_class(const char *class,
		  const struct block_driver_ops **out_ops, int max_ops)
{
	int fetched, loaded = 0, i;

	fetched = fetch_class(class);
	if (fetched == 0) {
		printf("blk: no modules in class \"%s\"\n", class);
		return 0;
	}

	for (i = 0; i < n_cached_modules && loaded < max_ops; i++) {
		const struct block_driver_ops *ops;
		ops = blk_dl_load_module(cached_modules[i].name);
		if (ops != NULL)
			out_ops[loaded++] = ops;
	}

	return loaded;
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
	(void)path;
}
