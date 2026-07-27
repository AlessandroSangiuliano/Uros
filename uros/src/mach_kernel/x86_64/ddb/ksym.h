/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Kernel symbols, so a backtrace names functions (#428).
 *
 * ── Where they come from, and why there is no ksyms.bin here ─────────
 *
 * The 32-bit kernel has the build produce a `ksyms.bin` and loads it as a
 * module. That machinery is not needed on this target and is not ported:
 * GRUB already hands over the ELF section headers in the multiboot2 boot
 * information (tag 9), and among them are `.symtab` and `.strtab`, loaded
 * into memory as part of loading the kernel.
 *
 * So the symbols are already there before the first instruction of C. What
 * is missing is only somebody reading them — which is the whole of this
 * file, and it is smaller than the tool that would have generated the file
 * that was not needed.
 *
 * ⚠️ It costs one thing, and it is not free: those sections are *not* part
 * of the multiboot information structure, so the boot allocator's low-water
 * mark does not cover them. Unprotected, `boot_frame_alloc()` would hand out
 * the page holding the symbol table and the lookup would afterwards return
 * names read out of a page table. That is worse than having no symbols: a
 * wrong name in a backtrace sends the reader somewhere, and it looks exactly
 * like a right one. bootmem.c reserves them for that reason.
 */

#ifndef _X86_64_DDB_KSYM_H_
#define _X86_64_DDB_KSYM_H_

#include <stdint.h>

/*
 * Find the symbol and string tables in the boot information.
 *
 * Returns the number of symbols recorded, or zero if the loader described no
 * sections or the sections it described were never loaded — which is a
 * result and not a failure, and is reported as one.
 */
unsigned ksym_init(uint32_t info_pa);

/* How many symbols are usable, and whether there are any at all. */
unsigned ksym_count(void);

/*
 * The function containing `addr`, and how far into it that address is.
 *
 * Returns a null pointer when no symbol covers the address, rather than the
 * nearest one at any distance: a name attached to an address a megabyte away
 * is not information, it is a suggestion, and a backtrace is read at the
 * moment when a plausible wrong answer costs the most.
 */
const char *ksym_lookup(uint64_t addr, uint64_t *offset);

/*
 * The span the loaded symbol data occupies, so the boot allocator can keep
 * off it. Zero when there is nothing to protect.
 */
uint64_t ksym_data_end(uint32_t info_pa);

#endif	/* _X86_64_DDB_KSYM_H_ */
