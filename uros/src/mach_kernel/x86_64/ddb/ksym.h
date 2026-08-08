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
 * ⚠️ It costs one thing: those sections are *not* part of the multiboot
 * information structure, so nothing in the boot allocator's low-water mark
 * describes them. Unprotected, `boot_frame_alloc()` could hand out the page
 * holding the symbol table, and the lookup would afterwards return names
 * read out of a page table — worse than having no symbols, since a wrong
 * name sends the reader somewhere and looks exactly like a right one.
 *
 * Measured under gdb, this loader happens to place the information structure
 * immediately above the symbol data, so the existing check already covers
 * it and the reservation in bootmem.c changes nothing today. It is kept
 * because that adjacency is an arrangement rather than a guarantee.
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
 * The same for a RETURN address, which is one byte past the call and so is
 * not always inside the function it belongs to (#409).
 *
 * Everything a backtrace reads off a stack is one of these, and the case where
 * it matters is the common one rather than an exotic one: a function whose last
 * instruction is a call — every continuation in the scheduler — returns to the
 * first byte of the next function.  Use this for a frame's saved address and
 * ksym_lookup() for an instruction pointer; the difference between them is one
 * byte and an entirely different name.
 */
const char *ksym_lookup_call(uint64_t ret, uint64_t *offset);

/*
 * Check the tail-call property over every function in the image (#409).
 *
 * Returns how many failed; `checked` how many were looked at, and `worst` the
 * largest offset a plain lookup produced at those same addresses — the number
 * that made the defect visible, since a wrong name here does not look wrong,
 * it looks like a large offset.
 */
unsigned ksym_tailcall_check(unsigned *checked, uint64_t *worst);

/*
 * The span the loaded symbol data occupies, so the boot allocator can keep
 * off it. Zero when there is nothing to protect.
 */
uint64_t ksym_data_end(uint32_t info_pa);

/*
 * Is this address in a section that gets executed?  Asked before a breakpoint
 * is accepted: one anywhere else can never fire (#428).
 */
int span_is_code(uint64_t addr);

#endif	/* _X86_64_DDB_KSYM_H_ */
