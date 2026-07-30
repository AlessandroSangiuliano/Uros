/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The widths, for x86-64 (#413).
 *
 * ── The one decision this file exists to make ─────────────────────────
 *
 * The i386 header defines natural_t as "the native integer type, e.g. 32 or
 * 64 or.. whatever register size the machine has", used "for entities that
 * might be either unsigned integers or pointers, and for type-casting
 * between the two".  On a machine where an integer and a pointer are the
 * same width that sentence describes one type.  On this one it describes
 * two, and following it literally would make natural_t sixty-four bits.
 *
 * That is not a widening of one typedef.  natural_t is what mach_port_t,
 * mach_msg_size_t, mach_msg_type_number_t, every port count, every make-send
 * count and every sequence number are made of.  Widening it doubles the
 * message header, doubles every port array in every message, and changes the
 * layout of every structure the kernel shares with a server — to express
 * numbers that were never near four billion in the first place.
 *
 * So natural_t keeps its width and gives up its second job:
 *
 *	natural_t	an unsigned number the interfaces exchange — 32 bits
 *	vm_offset_t	an address, or a distance between two — 64 bits
 *
 * ⚠️ On i386 those two are the same type, and MI code written there is
 * entitled to have assumed it.  Every place that stored an address in a
 * natural_t, or printed a vm_offset_t with %x, compiles here and is wrong
 * here; that is what #415 goes looking for, and it is why the split is
 * written down rather than discovered.
 *
 * ── Why an address is `unsigned long` and not `unsigned long long` ────
 *
 * Both are sixty-four bits under LP64, so the choice is about what the
 * compiler will say rather than what the machine will do: `long` is the
 * width of a pointer on every LP64 target, so `%lx` is right for a
 * vm_offset_t on all of them, and a build for a target where it is not would
 * fail here rather than silently print half an address.
 */

#ifndef	_MACH_X86_64_VM_TYPES_H_
#define _MACH_X86_64_VM_TYPES_H_

#ifdef	ASSEMBLER
#else	/* ASSEMBLER */

/*
 * The width the interfaces are written in: counts, sizes, names, ids.
 * Thirty-two bits here is a decision and not an oversight — see above.
 */
typedef unsigned int	natural_t;

/*
 * The signed counterpart.  Both exist to define other types in a
 * machine-independent way, and neither is a pointer.
 */
typedef int		integer_t;

/*
 * An integer at least thirty-two bits wide.  Still `int`: this says "at
 * least 32", and int is exactly 32 on both targets.
 */
typedef int		int32;
typedef unsigned int	uint32;

/*
 * An address, or an offset into an address space.  Sixty-four bits, and no
 * longer the same type as natural_t — that is the whole point of the file.
 */
typedef unsigned long	vm_offset_t;

/*
 * The difference between two vm_offset_t entities.  A size follows the
 * address it measures: a region can be larger than four gigabytes, so this
 * cannot be a natural_t either.
 */
typedef unsigned long	vm_size_t;

#endif	/* ASSEMBLER */

/*
 * If composing messages by hand (please dont).
 *
 * integer_t is thirty-two bits here, so this stays what it was — the tag
 * describes the wire, and the wire did not change.
 */
#define	MACH_MSG_TYPE_INTEGER_T	MACH_MSG_TYPE_INTEGER_32

#endif	/* _MACH_X86_64_VM_TYPES_H_ */
