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

/*
 * 🔑 A module is its class and its file name (#973).  Two pools may each hold
 * a "twin.so", and the cache knew a module by its file name alone: a program
 * that fetched two classes got the first class's module whenever it asked for
 * the second's.  So every lookup asks for both, and the path dlopen() is given
 * is "<class>/<name>" -- libdl knows an object by the path it was opened with,
 * and would hand back the first twin as already loaded.
 */
struct cached_module {
	char		class[MODULE_NAME_MAX];
	char		name[MODULE_NAME_MAX];
	vm_offset_t	data;		/* vm_allocate'd by bootstrap */
	vm_size_t	size;		/* bytes in .so */
};

/* "<class>/<name>", the path a module is opened with: two names and a '/'. */
#define MODULE_PATH_MAX		(2 * MODULE_NAME_MAX)

static struct cached_module	cached_modules[MAX_CACHED_MODULES];
static int			n_cached_modules;
static const char	       *log_tag = "modload";

static struct cached_module *
find_cached_module(const char *class, const char *name)
{
	int i;
	for (i = 0; i < n_cached_modules; i++) {
		if (strcmp(cached_modules[i].class, class) == 0 &&
		    strcmp(cached_modules[i].name, name) == 0)
			return &cached_modules[i];
	}
	return NULL;
}

/* ================================================================
 * I/O callback: serve dlopen() reads from the cache
 * ================================================================ */

/*
 * The path is "<class>/<name>", as load_cached() makes it: one '/', with a
 * class and a name on either side.  Any other path is no module of a pool.
 */
static int
cache_read_file(const char *path, void **buf, unsigned int *size)
{
	char class[MODULE_NAME_MAX];
	const char *slash;
	const struct cached_module *cm;
	vm_offset_t copy;
	kern_return_t kr;

	slash = strchr(path, '/');
	if (slash == NULL || slash == path || slash[1] == '\0' ||
	    strchr(slash + 1, '/') != NULL ||
	    (size_t)(slash - path) >= sizeof(class)) {
		printf("%s: \"%s\" is not <class>/<name>\n", log_tag, path);
		return -1;
	}
	memcpy(class, path, (size_t)(slash - path));
	class[slash - path] = '\0';

	cm = find_cached_module(class, slash + 1);
	if (cm == NULL) {
		printf("%s: module \"%s\" not in cache\n", log_tag, path);
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

/*
 * Every module the pool of <class> lists, into the cache: the number of them
 * that are there afterwards, fetched now or before.  A class name that does
 * not fit a pool's is refused, not cut: a cut name is another class's name.
 */
static int
fetch_class(const char *class)
{
	kern_return_t kr;
	vm_offset_t names = 0;
	mach_msg_type_number_t names_count = 0;
	module_name_t class_buf;
	const char *p;
	int present = 0;

	if (strlen(class) >= sizeof(class_buf)) {
		printf("%s: class \"%s\": a name longer than %u characters "
		       "is no pool's\n", log_tag, class,
		       (unsigned)(sizeof(class_buf) - 1));
		return 0;
	}
	strcpy(class_buf, class);

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

		if (len >= MODULE_NAME_MAX) {
			printf("%s: module \"%s/%s\": a name longer than %u "
			       "characters is no pool's\n", log_tag, class, p,
			       (unsigned)(MODULE_NAME_MAX - 1));
			p += len + 1;
			continue;
		}
		if (find_cached_module(class, p) != NULL) {
			present++;
			p += len + 1;
			continue;
		}
		if (n_cached_modules >= MAX_CACHED_MODULES) {
			printf("%s: module cache full, dropping \"%s/%s\"\n",
			       log_tag, class, p);
			break;
		}

		{
			module_name_t name_buf;
			vm_offset_t data = 0;
			mach_msg_type_number_t data_count = 0;
			struct cached_module *cm;

			strcpy(name_buf, p);

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
			strcpy(cm->class, class);
			strcpy(cm->name, p);
			cm->data = data;
			cm->size = data_count;
			present++;
		}

		p += len + 1;
	}

	if (names != 0)
		vm_deallocate(mach_task_self(), names, names_count);

	return present;
}

/*
 * Fetch one module of a class into the cache (#963): its entry, there
 * already or now; NULL, said, otherwise.  A name that does not fit a pool's
 * name is refused, not cut: a cut name is another module's name.
 */
static struct cached_module *
fetch_one(const char *class, const char *name)
{
	kern_return_t kr;
	module_name_t class_buf, name_buf;
	vm_offset_t data = 0;
	mach_msg_type_number_t data_count = 0;
	struct cached_module *cm;

	if (strlen(class) >= sizeof(class_buf) ||
	    strlen(name) >= sizeof(name_buf)) {
		printf("%s: module \"%s/%s\": a name longer than %u "
		       "characters is no pool's\n", log_tag, class, name,
		       (unsigned)(sizeof(name_buf) - 1));
		return NULL;
	}
	cm = find_cached_module(class, name);
	if (cm != NULL)
		return cm;
	if (n_cached_modules >= MAX_CACHED_MODULES) {
		printf("%s: module cache full, \"%s/%s\" not fetched\n",
		       log_tag, class, name);
		return NULL;
	}

	strcpy(class_buf, class);
	strcpy(name_buf, name);
	kr = bootstrap_get_module(bootstrap_port, class_buf, name_buf,
				  &data, &data_count);
	if (kr != KERN_SUCCESS) {
		printf("%s: bootstrap_get_module(\"%s\", \"%s\") failed: "
		       "0x%x\n", log_tag, class, name, kr);
		return NULL;
	}

	cm = &cached_modules[n_cached_modules++];
	strcpy(cm->class, class);
	strcpy(cm->name, name);
	cm->data = data;
	cm->size = data_count;
	return cm;
}

/*
 * dlopen() a cached module as "<class>/<name>" and return the symbol
 * <basename><sym_suffix>:
 *   "ahci.so"       + "_module_ops"    → "ahci_module_ops"
 *   "virtio_blk.so" + "_module_ops"    → "virtio_blk_module_ops"
 *   "pci_scan.so"   + "_discovery_ops" → "pci_scan_discovery_ops"
 */
static void *
load_cached(const struct cached_module *cm, const char *sym_suffix)
{
	char path[MODULE_PATH_MAX];
	char sym_buf[96];
	size_t base_len = 0;
	size_t suf_len;
	const char *p;
	void *handle;
	void *ops;

	/* Both names are shorter than MODULE_NAME_MAX: the cache holds no other. */
	(void) snprintf(path, sizeof(path), "%s/%s", cm->class, cm->name);

	suf_len = strlen(sym_suffix);
	if (suf_len + 1 >= sizeof(sym_buf)) {
		printf("%s: sym_suffix too long\n", log_tag);
		return NULL;
	}

	handle = dlopen(path, RTLD_NOW);
	if (handle == NULL) {
		printf("%s: dlopen(\"%s\") failed: %s\n",
		       log_tag, path, dlerror());
		return NULL;
	}

	for (p = cm->name; *p != '\0' && *p != '.' &&
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
	       log_tag, path, sym_buf);
	return ops;
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

	/*
	 * Its own class's modules, the ones its pool lists, fetched now or
	 * before -- and no other.  This walked the whole cache from index 0
	 * (#973): a program's second class dlopened the first class's modules
	 * again, and looked each up with the second class's suffix.
	 */
	for (i = 0; i < n_cached_modules && loaded < max_ops; i++) {
		void *ops;

		if (strcmp(cached_modules[i].class, class) != 0)
			continue;
		ops = load_cached(&cached_modules[i], sym_suffix);
		if (ops != NULL)
			out_ops[loaded++] = ops;
	}

	return loaded;
}

void *
modload_load_named(const char *class, const char *name, const char *sym_suffix)
{
	const struct cached_module *cm = fetch_one(class, name);

	if (cm == NULL)
		return NULL;
	return load_cached(cm, sym_suffix);
}
