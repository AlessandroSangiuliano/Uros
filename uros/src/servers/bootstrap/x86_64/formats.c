/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Which executable formats this machine can load (#422).
 *
 * ⚠️ ELF only, and a.out is not merely "not ported": there is no 64-bit a.out
 * and there never was.  i386 lists it because the tree still carries images
 * in that format from before ELF; nothing on this target can be one, so a
 * loader for it would be an entry that can never match.
 *
 * The table is walked in order by boot_load_program, which asks each format
 * whether it recognises the file.  One entry means one answer: either the
 * image is an ELF this libelf can open, or the failure names the format
 * rather than the eighth candidate.
 */

#include "bootstrap.h"

extern struct objfmt_switch elf_switch;

objfmt_switch_t formats[] = {
    &elf_switch,
    0
};

/*
 * Byte order, for the loaders that read big-endian fields.
 *
 * ⚠️ uint32_t and not `unsigned long', which is what i386's copy says.  There
 * the two are the same size and the code is correct by coincidence; here
 * `unsigned long' is eight bytes, the four masks below cover only the low
 * four, and the top half would be carried through unswapped -- a value that
 * is neither the input nor its byte-reversal.
 *
 * A network-order field in a file format is thirty-two bits by definition, so
 * saying so is both the fix and the documentation.
 */
uint32_t
ntohl(uint32_t arg)
{
	return ((arg & 0x000000ffU) << 24) |
	       ((arg & 0x0000ff00U) <<  8) |
	       ((arg & 0x00ff0000U) >>  8) |
	       ((arg & 0xff000000U) >> 24);
}
