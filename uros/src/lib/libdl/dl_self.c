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
 * dl_self.c — Bootstrap the running PIE executable into libdl's
 * loaded-object list so that dlopen'd modules can resolve their
 * undefined references against it via DT_HASH.
 *
 * Load bias discovery:
 *   ld does *not* emit R_386_RELATIVE entries for the d_ptr fields
 *   inside .dynamic (DT_SYMTAB/STRTAB/HASH), so after the bootstrap
 *   loader runs those still hold link-time VAs (offsets from image
 *   base, since our first PT_LOAD has p_vaddr == 0).  A dynamic
 *   linker is normally expected to fix them up itself.
 *
 *   To do that we need load_bias.  We obtain it from the ld-provided
 *   symbol __ehdr_start, which points at the ELF header.  With
 *   `visibility("hidden")` the compiler emits a PC-relative LEA and
 *   no relocation is needed — `&__ehdr_start` is the runtime VA of
 *   the image base, which equals load_bias for a PIE linked with
 *   p_vaddr == 0 in its first PT_LOAD.
 *
 * Once load_bias is known we walk _DYNAMIC, adding load_bias to every
 * d_ptr, and build a struct dl_object pointing at the resulting DT_HASH
 * / DT_SYMTAB / DT_STRTAB so dl_symlook_obj() can resolve symbols out
 * of the main exe's .dynsym.
 *
 * The descriptor is statically allocated (no malloc at bootstrap time)
 * and prepended to dl_obj_list.  Subsequent dlopen()s push .so objects
 * in front of it, so module symbols take priority — standard search
 * order — and exe-provided symbols (printf, MIG stubs, Mach VM) remain
 * reachable as fallback.
 */

#include <stdio.h>
#include <string.h>
#include "dl_internal.h"
#include "dlfcn.h"

/*
 * Linker-defined symbols.  Both are marked weak so libdl can still
 * link into a non-PIE binary (dl_bootstrap_self will fail cleanly).
 *
 * __ehdr_start is emitted by ld whenever it is referenced; hidden
 * visibility forces a PC-relative LEA so the address is computed
 * without going through the GOT or requiring a relocation.
 */
extern const char _DYNAMIC[]
	__attribute__((weak, visibility("hidden")));
extern const char __ehdr_start[]
	__attribute__((weak, visibility("hidden")));

/* Storage for the main exe's descriptor — no malloc needed */
static struct dl_object self_obj;

int
dl_bootstrap_self(void)
{
	const Elf32_Dyn *dyn;
	vm_offset_t load_bias;
	struct dl_object *obj = &self_obj;

	if (&_DYNAMIC[0] == NULL || &__ehdr_start[0] == NULL) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "dl_bootstrap_self: _DYNAMIC / __ehdr_start missing "
			 "(not built as -pie?)");
		return -1;
	}

	/*
	 * For a PIE whose first PT_LOAD has p_vaddr == 0 (the normal
	 * case), the runtime address of the ELF header IS load_bias.
	 */
	load_bias = (vm_offset_t)&__ehdr_start[0];

	memset(obj, 0, sizeof(*obj));
	obj->path = "(main)";
	obj->dynamic = (const Elf32_Dyn *)&_DYNAMIC[0];
	obj->relocbase = load_bias;

	/*
	 * Walk _DYNAMIC.  The d_ptr fields hold link-time VAs (offsets
	 * from image base); add load_bias to make them runtime VAs.
	 */
	for (dyn = obj->dynamic; dyn->d_tag != DT_NULL; dyn++) {
		switch (dyn->d_tag) {
		case DT_SYMTAB:
			obj->symtab = (const Elf32_Sym *)
				(dyn->d_un.d_ptr + load_bias);
			break;
		case DT_STRTAB:
			obj->strtab = (const char *)
				(dyn->d_un.d_ptr + load_bias);
			break;
		case DT_STRSZ:
			obj->strsize = dyn->d_un.d_val;
			break;
		case DT_HASH: {
			const Elf32_Word *h = (const Elf32_Word *)
				(dyn->d_un.d_ptr + load_bias);
			obj->nbuckets = h[0];
			obj->nchains  = h[1];
			obj->buckets  = h + 2;
			obj->chains   = obj->buckets + obj->nbuckets;
			break;
		}
		default:
			break;
		}
	}

	if (obj->symtab == NULL || obj->strtab == NULL ||
	    obj->buckets == NULL) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "dl_bootstrap_self: missing DT_SYMTAB/STRTAB/HASH "
			 "(linked without --export-dynamic --hash-style=sysv?)");
		return -1;
	}

	obj->ref_count = 1;

	/* Prepend to the loaded-object list */
	obj->next = dl_obj_list;
	dl_obj_list = obj;

	return 0;
}
