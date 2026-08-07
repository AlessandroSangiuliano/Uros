/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What migcom must know about the machine it is generating for (#416).
 *
 * ── Why this file has to exist ────────────────────────────────────────
 *
 * migcom runs on the build host and emits code for a target, and until now it
 * has answered every question about the target by measuring itself.  That was
 * survivable while the only target was i386 and the numbers happened to be
 * the ones a 32-bit host would give — and it has already been paid for once:
 * migcom carries its own cut-down <mach/message.h> in which the address field
 * of a descriptor is a uint32_t rather than a void *, precisely so that
 * `sizeof` on a 64-bit host would keep returning the 32-bit target's answer.
 *
 * A stub-out of a header, shaped to make the host lie in the right direction,
 * is not a target model.  It works for one target and silently produces the
 * wrong offsets for any other.  This is the target model: the numbers are
 * stated, per target, and the host is not asked.
 *
 * ── They are not checked here, and that is deliberate ─────────────────
 *
 * Nothing in this tool can verify these against the headers the kernel and
 * its servers actually compile — that is the whole difficulty.  What checks
 * them is the generated code: every fixed-size message carries a
 * _Static_assert tying migcom's arithmetic to the layout the compiler chose,
 * so a wrong number here fails the build of every stub for that target, by
 * name.  The verification lives where the target's compiler is.
 */

#ifndef	_MIGCOM_TARGET_H_
#define _MIGCOM_TARGET_H_

#include "type.h"

typedef struct mig_target {
	const char	*mt_name;

	/*
	 * The fixed parts of a message.  The header is six 32-bit fields on
	 * every target; the body is one 32-bit count, aligned so that the
	 * descriptors which follow it begin on an address boundary (#413) —
	 * which is why it is not the same size everywhere.
	 */
	u_int		mt_header_size;
	u_int		mt_body_size;

	/* The NDR record: eight bytes of encoding tags, alignment 1. */
	u_int		mt_ndr_size;

	/*
	 * One descriptor: an address, a 32-bit count and a 32-bit flags word,
	 * and aligned like the address it begins with.
	 */
	u_int		mt_descriptor_size;
	u_int		mt_descriptor_align;

	/*
	 * The widest alignment this target's ABI gives a scalar.
	 *
	 * ⚠️ This is the number that made the whole problem invisible.  On
	 * i386 a 64-bit scalar is aligned to *four*, so a message's fields
	 * never need padding between them and adding up sizes gives the right
	 * answer by accident.  On x86-64 it is eight, and adding up sizes
	 * stops being the same computation as laying out a structure.
	 */
	u_int		mt_max_align;

	/*
	 * Whether this target has the RPC-trap glue that the short-circuit
	 * path calls into — RPC_SIMPLE and its neighbours, declared in
	 * <mach/machine/rpc.h>.  x86-64 does not: those macros are i386
	 * register names describing an argument list that, under a convention
	 * where the first six arguments arrive in registers, is not anywhere.
	 * Emitting the calls anyway produces stubs that compile against a
	 * header without them and fail to link, which is a worse way to learn
	 * it than this one.
	 */
	boolean_t	mt_rpc_trap;
} mig_target_t;

extern const mig_target_t *Target;

/* Select by name; returns FALSE and changes nothing if it is not one we know. */
extern boolean_t TargetSelect(const char *name);

/* The names we know, for the diagnostic when one is not. */
extern const char *TargetNames(void);

/* Round `offset` up to the next multiple of `align` (a power of two). */
extern u_int TargetAlignUp(u_int offset, u_int align);

/*
 * The alignment a scalar of `bytes` bytes gets on the target: its own size
 * rounded up to a power of two, and never more than the ABI grants.
 */
extern u_int TargetScalarAlign(u_int bytes);

#endif	/* _MIGCOM_TARGET_H_ */
