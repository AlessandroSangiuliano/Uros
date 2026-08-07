/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The RPC trap glue — which this target does not have (#416).
 *
 * <mach/rpc.h> includes this unconditionally, so the file has to exist.  What
 * it must not do is contain something that looks like an implementation.
 *
 * ── What the i386 header is ───────────────────────────────────────────
 *
 * A second calling path: `mach_rpc_trap`, a signature describing the
 * arguments, and macros that reach into a thread's saved registers by name to
 * find the argument list, the return value, the user instruction pointer and
 * the user stack pointer — `USER_REGS(act)->ebp + 8` and its neighbours.
 * They exist so a server could be entered without composing a message.
 *
 * ── Why it is not ported, rather than not ported yet ──────────────────
 *
 * The macros are register names, and register names are the *last* thing to
 * transliterate: `ebp + 8` is not "rbp + 8", it is the i386 C calling
 * convention, in which arguments arrive on the stack.  The x86-64 convention
 * puts the first six in registers, so the argument list that macro finds does
 * not exist here in any location.  Writing the plausible-looking version —
 * rbp, rcx, rip, rsp — would produce a header that compiles, reads correctly,
 * and describes nothing.
 *
 * That is the failure this project refuses on purpose: a stub whose return
 * value is believable.  A path that has never been taken and cannot be
 * measured is not made real by having its header ported.
 *
 * So this target has no RPC trap.  Everything crosses on SYSCALL (#411) and
 * as a message.  If it ever earns one, it starts from a measurement showing
 * what composing the message costs — and then this file gets written, once,
 * against a mechanism that exists.
 *
 * ── What all this was really for, and what replaced it ────────────────
 *
 * Collocation: linking a server into the kernel's address space so that a
 * call reaches it by switching stacks and jumping rather than by composing a
 * message.  It bought two things at once — direct access to hardware, and a
 * crossing that cost nothing — and on 1990 processors there was no other way
 * to have either.
 *
 * An IOMMU splits those two apart and hands the first one back.  A driver in
 * user space can be given a device directly and still be unable to reach
 * anything the IOMMU has not mapped for it, so the hardware argument for
 * pulling it inside the kernel is gone (#432 for the remapping, #427 for the
 * device address space).  What is left of collocation is only the crossing
 * cost, and that is a number rather than an architecture: #392 measures it,
 * and if it turns out to matter the answer is a designed register fast path
 * (#391), not this.
 *
 * ⚠️ Nothing here defines MACH_RPC, MACH_RPC_ARGV or their neighbours.  Code
 * that wants them fails to compile, by name, at the line that wants them —
 * which is the only useful thing this file can do for it.
 */

#ifndef	_MACH_X86_64_RPC_H_
#define _MACH_X86_64_RPC_H_

/*
 * The one thing the machine-independent header cannot do without: it declares
 * `extern rpc_glue_vector_t _rpc_glue_vector` unconditionally, so the name has
 * to be a type.
 *
 * It is an incomplete one, deliberately.  A declaration parses; every
 * dereference of a member fails to compile because there are no members, and
 * the object itself is declared by nobody, so anything that reaches for it
 * fails to link with its own name in the message.  A structure filled in with
 * the six function pointers i386 has would parse *and* link, and would then be
 * a table of null pointers waiting to be called.
 */
typedef struct rpc_glue_vector *rpc_glue_vector_t;

#endif	/* _MACH_X86_64_RPC_H_ */
