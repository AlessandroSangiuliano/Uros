/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/ast.h> for x86-64 (#450).
 *
 * 🔴 EMPTY, and that is the answer rather than an omission (#515).
 *
 * AST_X86_64_FP was here, with MACHINE_AST_PER_THREAD naming it, described as
 * "the one AST bit the machine owns: a thread that has used the FPU carries it
 * so the context switch knows to save state".
 *
 * 🔑 It was never set and never tested -- the definition and that sentence
 * were the only two places the name appeared -- and the sentence was not true
 * of this kernel either: x86-64 saves unconditionally on every switch (#561),
 * with the only exemption declared by context_needs_vector_state() rather than
 * carried in an AST.  A comment asserting a property the tree does not have is
 * worse than none, because it reads as a reason not to look.
 *
 * It was i386's AST_I386_FP copied across during the port, and that one did
 * have a job: carrying an x87 error from the processor the PIC delivered IRQ
 * 13 to, over to the thread that earned it, because CR0.NE was clear.  #515
 * set the bit on both targets, so neither has anything to defer.
 *
 * ⚠️ MACHINE_AST_PER_THREAD is therefore not defined here at all, and
 * <kern/ast.h> supplies the machine-independent 0.  Defining it to 0 would
 * read as a machine that has one and set it to nothing.
 */

#ifndef _X86_64_AST_H_
#define _X86_64_AST_H_

#endif /* _X86_64_AST_H_ */
