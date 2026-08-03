/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/endian.h> for x86-64 (#453).
 *
 * Byte order, and the four conversions between host and network order that
 * device/net_io.c needs.
 *
 * ── Why these are builtins and not inline assembly ────────────────────
 *
 * i386's twin writes `rorw $8` and `bswap` by hand, which in 1990 was the
 * only way to get them.  Here they are __builtin_bswap*, and that is not a
 * matter of taste:
 *
 *   - a builtin folds when its argument is a constant, so ntohs(0x0800) in
 *     a protocol test costs nothing at all, where the asm version forces a
 *     register and an instruction because the compiler cannot see through
 *     an asm block;
 *   - the compiler picks the encoding, including the 16-bit form that is
 *     `rol $8` and the 32/64-bit forms that are `bswap`, and it picks
 *     correctly for whatever -march it is given;
 *   - an asm block is opaque to every later pass, so a hand-written swap in
 *     a loop blocks vectorisation and reordering that the builtin allows.
 *
 * This is one of the places where "study, never copy" is not a slogan: the
 * i386 file is a correct answer to a question the compiler now answers
 * better.
 */

#ifndef	_X86_64_ENDIAN_H_
#define	_X86_64_ENDIAN_H_

#include <stdint.h>

/*
 * The names, by byte significance from low address to high.  Kept as the
 * heritage tree spells them because network code tests BYTE_ORDER against
 * them by name.
 */
#define	LITTLE_ENDIAN	1234	/* least-significant byte first */
#define	BIG_ENDIAN	4321	/* most-significant byte first  */
#define	PDP_ENDIAN	3412	/* LSB first in word, MSW first in long */

#define	BYTE_ORDER	LITTLE_ENDIAN
#define	ENDIAN		LITTLE

/*
 * Host to network and back.
 *
 * ⚠️ htonl/ntohl are `unsigned int` and not `unsigned long`, unlike i386's.
 * On i386 the two are the same width so the difference could not show.  Here
 * long is 64 bits, and a 32-bit protocol field swapped as a 64-bit quantity
 * comes back with the four bytes it wanted at the wrong end -- silently, and
 * only for values that fit in 32 bits, which is all of them.  The width is
 * part of the contract these functions have with the wire, so it is written
 * as the width the wire uses.
 */
static inline uint16_t	ntohs(uint16_t w)	{ return __builtin_bswap16(w); }
static inline uint16_t	htons(uint16_t w)	{ return __builtin_bswap16(w); }
static inline uint32_t	ntohl(uint32_t l)	{ return __builtin_bswap32(l); }
static inline uint32_t	htonl(uint32_t l)	{ return __builtin_bswap32(l); }

#endif	/* _X86_64_ENDIAN_H_ */
