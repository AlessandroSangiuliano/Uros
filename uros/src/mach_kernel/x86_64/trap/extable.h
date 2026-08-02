/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Instructions that are allowed to fault, and where to go instead (#453).
 *
 * copyin() and copyout() touch addresses a user task chose.  The address may
 * be unmapped, may have been unmapped since the check, or may never have
 * been the task's to name -- and none of those is a kernel bug, so none of
 * them may halt the kernel.  What has to happen is that the copy stops and
 * answers an error.
 *
 * ── Why a table and not a flag ────────────────────────────────────────
 *
 * trap_expect() already survives one fault, and it is not this.  It is a
 * single armed expectation, disarmed when it fires, written so that a test
 * can provoke a protection and continue; two processors copying at once
 * would overwrite each other's arrangement, and a fault anywhere else while
 * one was armed would be recovered as though it had been expected.
 *
 * A table has neither problem, because it is not state: it is a property of
 * the code, decided when the code was compiled.  Every instruction that may
 * fault carries its own recovery address, the handler looks up the faulting
 * rip, and nothing has to be armed, disarmed, or serialised between
 * processors.  This is what every kernel that does this seriously uses.
 *
 * ⚠️ The entries are addresses, not offsets, and the table is therefore 16
 * bytes an entry.  Linux stores signed 32-bit displacements to halve it,
 * which matters when the table has ten thousand entries; ours will have
 * tens, and a displacement is a second thing to get right for a saving that
 * would not be measurable here.  When it has thousands, this is the comment
 * that says what to change.
 */

#ifndef	_X86_64_TRAP_EXTABLE_H_
#define	_X86_64_TRAP_EXTABLE_H_

#ifndef	__ASSEMBLER__

#include <stdint.h>

struct ex_entry {
	uint64_t	fault_rip;	/* the instruction that may fault */
	uint64_t	resume_rip;	/* where to continue instead     */
};

/*
 * Where to resume for a fault at `rip', or zero if this instruction was not
 * one that is allowed to fault -- in which case the caller has a real bug
 * and should report it.  Zero is not a resume address any entry could hold.
 */
uint64_t ex_table_lookup(uint64_t rip);

/*
 * Annotate an instruction that may fault.  Both labels must already have been
 * emitted -- the fault label before the instruction, the resume label after
 * it -- so both references are BACKWARD.
 *
 * ⚠️ That is not a style choice.  A forward reference does not survive
 * .pushsection: the assembler resolves numeric local labels against the
 * stream it is writing, and inside the pushed section the label it would
 * look forward to has not been seen and never will be.  The error it gives
 * is "local label is not defined", with an instance number rather than a
 * line, which is how a routine that inlines in six places reports it.
 */
#define	EX_TABLE(fault, resume)						\
	".pushsection .ex_table, \"a\"\n\t"				\
	".align 8\n\t"							\
	".quad " #fault "b\n\t"						\
	".quad " #resume "b\n\t"					\
	".popsection\n\t"

#endif	/* __ASSEMBLER__ */

#endif	/* _X86_64_TRAP_EXTABLE_H_ */
