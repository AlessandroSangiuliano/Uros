/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Reading the multiboot2 handoff (#406/#407).
 *
 * boot.S captured the boot-information pointer GRUB left in %ebx and has
 * carried it, unread, since the first increment.  This is what finally opens
 * it: a length-prefixed list of tags, each 8-byte aligned, ending with a tag
 * of type zero.  The one the pmap wants is the memory map — the only account
 * of which physical addresses are RAM, which the direct map must be sized
 * against instead of guessing.
 */

#ifndef _X86_64_BOOT_MULTIBOOT2_H_
#define _X86_64_BOOT_MULTIBOOT2_H_

#include <stdint.h>

#define MB2_BOOTLOADER_MAGIC	0x36d76289	/* what GRUB leaves in %eax */

#define MB2_TAG_END		0
#define MB2_TAG_MEMORY_MAP	6

struct mb2_tag {
	uint32_t type;
	uint32_t size;
};

/* Memory-map tag: a header followed by entry_size-sized entries. */
struct mb2_tag_mmap {
	uint32_t type;
	uint32_t size;
	uint32_t entry_size;
	uint32_t entry_version;
};

struct mb2_mmap_entry {
	uint64_t addr;
	uint64_t len;
	uint32_t type;
	uint32_t reserved;
};

#define MB2_MEM_AVAILABLE	1	/* every other type is not ours to use */

/*
 * The tag of the given type, or NULL if the loader did not supply one.
 *
 * info_pa is a physical address: GRUB places the structure in low memory and
 * the boot identity map still covers it, so it is dereferenced directly.
 * That is deliberate — this has to be readable before the direct map exists,
 * since the direct map is sized from what it says.
 */
const struct mb2_tag *mb2_find_tag(uint32_t info_pa, uint32_t type);

/*
 * One past the highest byte of available RAM, or zero when the loader gave
 * no memory map.  Regions the firmware has claimed are ignored: it is the
 * extent that matters, since the direct map spans holes rather than
 * describing them.
 */
uint64_t mb2_top_of_ram(uint32_t info_pa);

/* Total bytes marked available — what the machine actually has to spend. */
uint64_t mb2_usable_ram(uint32_t info_pa);

#endif	/* _X86_64_BOOT_MULTIBOOT2_H_ */
