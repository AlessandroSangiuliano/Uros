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
 * dl_api.c — POSIX dlopen/dlsym/dlclose/dlerror implementation for Uros.
 *
 * This is the public API of libdl.  It uses the I/O callback to read
 * ELF files, maps them with Mach VM, resolves symbols against the host
 * symbol table, and manages a linked list of loaded objects.
 *
 * Phase 1: eager relocation, no lazy PLT, no DT_NEEDED, no locking.
 */

#include <stdio.h>
#include <string.h>
#include "dl_internal.h"
#include "dlfcn.h"

/* ================================================================
 * Global state
 * ================================================================ */

struct dl_object	*dl_obj_list;
const struct dl_host_sym *dl_host_symtab;
const struct dl_io_ops	*dl_io;
char			dl_error_msg[DL_ERRMSG_SIZE];

/* Last error message returned by dlerror (cleared after read) */
static char		dl_last_error[DL_ERRMSG_SIZE];
static int		dl_error_set;

/* ================================================================
 * Configuration API
 * ================================================================ */

void
dl_set_io_ops(const struct dl_io_ops *ops)
{
	dl_io = ops;
}

void
dl_set_host_symbols(const struct dl_host_sym *symtab)
{
	dl_host_symtab = symtab;
}

/* ================================================================
 * Find an already-loaded object by path.
 * ================================================================ */

static struct dl_object *
dl_find_loaded(const char *path)
{
	struct dl_object *obj;

	for (obj = dl_obj_list; obj != NULL; obj = obj->next) {
		if (obj->path != NULL && strcmp(obj->path, path) == 0)
			return obj;
	}
	return NULL;
}

/* ================================================================
 * dlopen — Load a shared object.
 *
 * 1. Check if already loaded (bump refcount)
 * 2. Read file via I/O callback
 * 3. Map into VM (parse ELF, copy PT_LOAD segments)
 * 4. Parse .dynamic section
 * 5. Apply relocations (eager)
 * 6. Call DT_INIT
 * 7. Add to loaded list, return handle
 * ================================================================ */

void *
dlopen(const char *path, int mode)
{
	struct dl_object *obj;
	void *filebuf;
	unsigned int filesize;

	(void)mode;	/* phase 1: always eager */

	dl_error_msg[0] = '\0';

	if (path == NULL) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "dlopen: NULL path");
		dl_error_set = 1;
		memcpy(dl_last_error, dl_error_msg, DL_ERRMSG_SIZE);
		return NULL;
	}

	/* Check I/O ops */
	if (dl_io == NULL || dl_io->read_file == NULL) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "dlopen: no I/O ops configured (call dl_set_io_ops)");
		dl_error_set = 1;
		memcpy(dl_last_error, dl_error_msg, DL_ERRMSG_SIZE);
		return NULL;
	}

	/* Already loaded? */
	obj = dl_find_loaded(path);
	if (obj != NULL) {
		obj->ref_count++;
		return obj;
	}

	/* Read file */
	filebuf = NULL;
	filesize = 0;
	if (dl_io->read_file(path, &filebuf, &filesize) != 0 ||
	    filebuf == NULL) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "dlopen: cannot read \"%s\"", path);
		dl_error_set = 1;
		memcpy(dl_last_error, dl_error_msg, DL_ERRMSG_SIZE);
		return NULL;
	}

	/* Map object (parse ELF, allocate VM, copy segments, digest_dynamic) */
	obj = dl_map_object(filebuf, filesize, path);

	/* Free the file buffer — we've copied everything into VM */
	if (dl_io->free_buf != NULL)
		dl_io->free_buf(filebuf, filesize);

	if (obj == NULL) {
		dl_error_set = 1;
		memcpy(dl_last_error, dl_error_msg, DL_ERRMSG_SIZE);
		return NULL;
	}

	/* Apply relocations */
	if (dl_relocate(obj) != 0) {
		dl_error_set = 1;
		memcpy(dl_last_error, dl_error_msg, DL_ERRMSG_SIZE);
		dl_free_object(obj);
		return NULL;
	}

	/* Add to loaded list */
	obj->next = dl_obj_list;
	dl_obj_list = obj;

	/* Call DT_INIT */
	if (obj->init != NULL)
		obj->init();

	return obj;
}

/* ================================================================
 * dlsym — Look up a symbol in a loaded object.
 * ================================================================ */

void *
dlsym(void *handle, const char *name)
{
	struct dl_object *obj;
	unsigned long hash;
	const Elf32_Sym *sym;

	dl_error_msg[0] = '\0';

	if (handle == NULL || name == NULL) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "dlsym: invalid argument");
		dl_error_set = 1;
		memcpy(dl_last_error, dl_error_msg, DL_ERRMSG_SIZE);
		return NULL;
	}

	obj = (struct dl_object *)handle;
	hash = dl_elf_hash(name);
	sym = dl_symlook_obj(name, hash, obj);

	if (sym == NULL) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "dlsym: symbol \"%s\" not found in %s",
			 name, obj->path ? obj->path : "?");
		dl_error_set = 1;
		memcpy(dl_last_error, dl_error_msg, DL_ERRMSG_SIZE);
		return NULL;
	}

	return (void *)(obj->relocbase + sym->st_value);
}

/* ================================================================
 * dlclose — Unload a shared object.
 *
 * Decrements refcount.  When it reaches zero: call DT_FINI,
 * remove from list, unmap VM.
 * ================================================================ */

int
dlclose(void *handle)
{
	struct dl_object *obj, *prev;

	dl_error_msg[0] = '\0';

	if (handle == NULL) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "dlclose: NULL handle");
		dl_error_set = 1;
		memcpy(dl_last_error, dl_error_msg, DL_ERRMSG_SIZE);
		return -1;
	}

	obj = (struct dl_object *)handle;
	obj->ref_count--;

	if (obj->ref_count > 0)
		return 0;

	/* Call DT_FINI */
	if (obj->fini != NULL)
		obj->fini();

	/* Remove from linked list */
	if (dl_obj_list == obj) {
		dl_obj_list = obj->next;
	} else {
		for (prev = dl_obj_list; prev != NULL; prev = prev->next) {
			if (prev->next == obj) {
				prev->next = obj->next;
				break;
			}
		}
	}

	/* Unmap and free */
	dl_free_object(obj);
	return 0;
}

/* ================================================================
 * dlerror — Return last error message, then clear it.
 * ================================================================ */

const char *
dlerror(void)
{
	if (!dl_error_set)
		return NULL;

	dl_error_set = 0;
	return dl_last_error;
}
