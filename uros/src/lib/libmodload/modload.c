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
 * modload.c — runtime module loader for class servers.
 *
 * The server is linked as a PIE executable with --export-dynamic and
 * --hash-style=sysv so every symbol it defines (MIG stubs, libc, Mach
 * VM/port/IPC, stack-protector, etc.) shows up in its own .dynsym.
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
#include "modload.h"
#include "dlfcn.h"
#include "dl_internal.h"

/* ================================================================
 * Per-instance state (one class server = one modload context)
 * ================================================================ */

#define MAX_CACHED_MODULES	16

struct cached_module {
	char		name[MODULE_NAME_MAX];
	vm_offset_t	data;		/* vm_allocate'd by bootstrap */
	vm_size_t	size;		/* bytes in .so */
};

static struct cached_module	cached_modules[MAX_CACHED_MODULES];
static int			n_cached_modules;
static const char	       *log_tag = "modload";

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
		printf("%s: module \"%s\" not in cache\n", log_tag, basename);
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
 * Fetch the module pool for a class from the bootstrap
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
		printf("%s: bootstrap_list_modules(\"%s\") failed: 0x%x\n",
		       log_tag, class, kr);
		return 0;
	}

	for (p = (const char *)names; p < (const char *)names + names_count; ) {
		size_t len = strlen(p);
		if (len == 0)
			break;

		if (n_cached_modules >= MAX_CACHED_MODULES) {
			printf("%s: module cache full, dropping \"%s\"\n",
			       log_tag, p);
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
				printf("%s: bootstrap_get_module"
				       "(\"%s\", \"%s\") failed: 0x%x\n",
				       log_tag, class, p, kr);
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
modload_init(const char *tag)
{
	if (tag != NULL)
		log_tag = tag;

	dl_set_io_ops(&cache_io_ops);

	if (dl_bootstrap_self() != 0) {
		printf("%s: dl_bootstrap_self failed: %s\n",
		       log_tag, dlerror());
		printf("%s: module symbol resolution will fail\n", log_tag);
		return;
	}

	printf("%s: dynamic module loader initialised "
	       "(self-bootstrap OK)\n", log_tag);
}

int
modload_load_class(const char *class, const char *sym_suffix,
		   void **out_ops, int max_ops)
{
	int fetched, loaded = 0, i;

	fetched = fetch_class(class);
	if (fetched == 0) {
		printf("%s: no modules in class \"%s\"\n", log_tag, class);
		return 0;
	}

	for (i = 0; i < n_cached_modules && loaded < max_ops; i++) {
		void *ops = modload_load_module(cached_modules[i].name,
						sym_suffix);
		if (ops != NULL)
			out_ops[loaded++] = ops;
	}

	return loaded;
}

void *
modload_load_module(const char *name, const char *sym_suffix)
{
	void *handle;
	void *ops;
	const char *basename;
	char sym_buf[96];
	size_t base_len = 0;
	size_t suf_len;
	const char *p;

	handle = dlopen(name, RTLD_NOW);
	if (handle == NULL) {
		printf("%s: dlopen(\"%s\") failed: %s\n",
		       log_tag, name, dlerror());
		return NULL;
	}

	/*
	 * Derive the ops symbol name from the module filename:
	 *   "ahci.so"       + "_module_ops"  → "ahci_module_ops"
	 *   "virtio_blk.so" + "_module_ops"  → "virtio_blk_module_ops"
	 *   "pci_scan.so"   + "_discovery_ops" → "pci_scan_discovery_ops"
	 */
	basename = name;
	for (p = name; *p != '\0'; p++) {
		if (*p == '/')
			basename = p + 1;
	}

	suf_len = strlen(sym_suffix);
	if (suf_len + 1 >= sizeof(sym_buf)) {
		printf("%s: sym_suffix too long\n", log_tag);
		dlclose(handle);
		return NULL;
	}

	for (p = basename; *p != '\0' && *p != '.' &&
			   base_len < sizeof(sym_buf) - suf_len - 1; p++) {
		sym_buf[base_len++] = *p;
	}
	memcpy(sym_buf + base_len, sym_suffix, suf_len);
	sym_buf[base_len + suf_len] = '\0';

	ops = dlsym(handle, sym_buf);
	if (ops == NULL) {
		printf("%s: dlsym(\"%s\") failed: %s\n",
		       log_tag, sym_buf, dlerror());
		dlclose(handle);
		return NULL;
	}

	printf("%s: loaded module \"%s\" (sym %s)\n",
	       log_tag, name, sym_buf);
	return ops;
}
